// SPDX-License-Identifier: MIT
#include "audio/pulse_discovery.h"

#include <algorithm>
#include <cctype>
#include <cstdio>

#include "common/util.h"

#ifdef USBDISPLAY_HAVE_PULSE
#include <pulse/pulseaudio.h>
#endif

namespace usbdisplay {
namespace audio {
namespace {

std::string lower(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

bool sink_matches(const PulseSinkInfo& sink) {
  const std::string text = lower(sink.name + " " + sink.description);
  return text.find("usbdisplay") != std::string::npos ||
         text.find("usb_display") != std::string::npos ||
         text.find("actions_usb_display") != std::string::npos ||
         text.find("1de1") != std::string::npos ||
         text.find("f201") != std::string::npos;
}

#ifdef USBDISPLAY_HAVE_PULSE

struct QueryState {
  bool done = false;
  bool ok = false;
  std::string error;
  std::vector<PulseSinkInfo> sinks;
  std::string target;
  bool want_sinks = false;
};

void sink_info_callback(pa_context*, const pa_sink_info* info, int eol,
                        void* data) {
  QueryState* query = static_cast<QueryState*>(data);
  if (eol < 0) {
    query->done = true;
    query->ok = false;
    query->error = "查询 sink 列表失败";
    return;
  }
  if (eol > 0) {
    query->done = true;
    query->ok = true;
    return;
  }
  PulseSinkInfo sink;
  if (info->name != nullptr) sink.name = info->name;
  if (info->description != nullptr) sink.description = info->description;
  query->sinks.push_back(std::move(sink));
}

void server_info_callback(pa_context*, const pa_server_info* info, void* data) {
  QueryState* query = static_cast<QueryState*>(data);
  query->done = true;
  query->ok = info != nullptr && info->default_sink_name != nullptr;
  if (query->ok) {
    query->target = info->default_sink_name;
  } else {
    query->error = "读取默认 sink 失败";
  }
}

void context_state_callback(pa_context* context, void* data) {
  QueryState* query = static_cast<QueryState*>(data);
  switch (pa_context_get_state(context)) {
    case PA_CONTEXT_READY:
      if (query->want_sinks) {
        pa_context_get_sink_info_list(context, sink_info_callback, query);
      } else {
        pa_context_get_server_info(context, server_info_callback, query);
      }
      break;
    case PA_CONTEXT_FAILED:
    case PA_CONTEXT_TERMINATED:
      query->done = true;
      query->ok = false;
      if (query->error.empty()) query->error = "PulseAudio 连接失败";
      break;
    default:
      break;
  }
}

bool run_query(const std::string& app_name, QueryState* state,
               bool connect_and_wait = true) {
  (void)connect_and_wait;
  pa_mainloop* mainloop = pa_mainloop_new();
  if (mainloop == nullptr) {
    state->error = "创建 PulseAudio 主循环失败";
    return false;
  }
  pa_mainloop_api* api = pa_mainloop_get_api(mainloop);
  pa_context* context = pa_context_new(api, app_name.c_str());
  if (context == nullptr) {
    pa_mainloop_free(mainloop);
    state->error = "创建 PulseAudio 上下文失败";
    return false;
  }
  pa_context_set_state_callback(context, context_state_callback, state);
  if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
    state->error = "连接 PulseAudio 失败";
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
    return false;
  }
  const uint64_t deadline = mono_ms() + 3000;
  while (!state->done && mono_ms() < deadline) {
    if (pa_mainloop_prepare(mainloop, 100000) < 0) break;
    if (pa_mainloop_poll(mainloop) < 0) break;
    if (pa_mainloop_dispatch(mainloop) < 0) break;
  }
  const bool completed = state->done;
  const bool success = state->ok;
  pa_context_disconnect(context);
  pa_context_unref(context);
  pa_mainloop_free(mainloop);
  if (!completed) {
    state->error = "PulseAudio 查询超时";
    return false;
  }
  return success;
}

class LibpulseDiscovery : public PulseDiscovery {
 public:
  bool list_sinks(std::vector<PulseSinkInfo>* out, std::string* err) override {
    QueryState state;
    state.want_sinks = true;
    if (!run_query("usbdisplay", &state)) {
      if (err) *err = state.error;
      return false;
    }
    *out = std::move(state.sinks);
    return true;
  }

  bool get_default_sink(std::string* out, std::string* err) override {
    QueryState state;
    if (!run_query("usbdisplay", &state)) {
      if (err) *err = state.error;
      return false;
    }
    *out = state.target;
    return true;
  }

  bool set_default_sink(const std::string& name, std::string* err) override {
    QueryState state;
    state.target = name;
    pa_mainloop* mainloop = pa_mainloop_new();
    if (mainloop == nullptr) {
      if (err) *err = "创建 PulseAudio 主循环失败";
      return false;
    }
    pa_mainloop_api* api = pa_mainloop_get_api(mainloop);
    pa_context* context = pa_context_new(api, "usbdisplay");
    if (context == nullptr) {
      pa_mainloop_free(mainloop);
      if (err) *err = "创建 PulseAudio 上下文失败";
      return false;
    }
    pa_context_set_state_callback(
        context,
        [](pa_context* ctx, void* data) {
          QueryState* query = static_cast<QueryState*>(data);
          if (pa_context_get_state(ctx) == PA_CONTEXT_READY) {
            pa_context_set_default_sink(
                ctx, query->target.c_str(),
                [](pa_context*, int success, void* result_data) {
                  QueryState* result = static_cast<QueryState*>(result_data);
                  result->done = true;
                  result->ok = success != 0;
                },
                query);
          } else if (pa_context_get_state(ctx) == PA_CONTEXT_FAILED ||
                     pa_context_get_state(ctx) == PA_CONTEXT_TERMINATED) {
            query->done = true;
            query->ok = false;
          }
        },
        &state);
    if (pa_context_connect(context, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
      if (err) *err = "连接 PulseAudio 失败";
      pa_context_unref(context);
      pa_mainloop_free(mainloop);
      return false;
    }
    const uint64_t deadline = mono_ms() + 3000;
    while (!state.done && mono_ms() < deadline) {
      if (pa_mainloop_prepare(mainloop, 100000) < 0) break;
      if (pa_mainloop_poll(mainloop) < 0) break;
      if (pa_mainloop_dispatch(mainloop) < 0) break;
    }
    const bool completed = state.done;
    const bool success = state.ok;
    pa_context_disconnect(context);
    pa_context_unref(context);
    pa_mainloop_free(mainloop);
    if (!completed) {
      if (err) *err = "设置默认 sink 超时";
      return false;
    }
    if (!success) {
      if (err) *err = "设置默认 sink 失败";
      return false;
    }
    return true;
  }
};

#else  // !USBDISPLAY_HAVE_PULSE

class LibpulseDiscovery : public PulseDiscovery {
 public:
  bool list_sinks(std::vector<PulseSinkInfo>*, std::string* err) override {
    if (err) *err = "构建时未启用 PulseAudio 支持";
    return false;
  }
  bool get_default_sink(std::string*, std::string* err) override {
    if (err) *err = "构建时未启用 PulseAudio 支持";
    return false;
  }
  bool set_default_sink(const std::string&, std::string* err) override {
    if (err) *err = "构建时未启用 PulseAudio 支持";
    return false;
  }
};

#endif

}  // namespace

std::string select_usb_sink(const std::vector<PulseSinkInfo>& sinks) {
  for (const auto& sink : sinks) {
    if (sink_matches(sink)) return sink.name;
  }
  return {};
}

std::unique_ptr<PulseDiscovery> make_libpulse_discovery() {
  return std::unique_ptr<PulseDiscovery>(new LibpulseDiscovery());
}

}  // namespace audio
}  // namespace usbdisplay
