// SPDX-License-Identifier: MIT
#include <algorithm>
#include <cstring>
#include <vector>

#include "common/util.h"
#include "hid/report_dump.h"
#include "test_harness.h"
#include "transport/usb_video.h"

int main() {
  using namespace usbdisplay;
  using namespace usbdisplay::transport;
  {
    // Verified layout: first URB carries L/width/height, continuations zero.
    std::vector<uint8_t> au(5000);
    au[0] = 0;
    au[1] = 0;
    au[2] = 0;
    au[3] = 1;
    for (size_t i = 4; i < au.size(); ++i) {
      au[i] = static_cast<uint8_t>(i * 3 + 1);
    }
    VideoUrbOptions options;
    options.urb_size = 4096;
    options.report_id = 2;
    std::vector<std::vector<uint8_t>> urbs;
    std::string err;
    CHECK(build_video_urbs(au.data(), au.size(), 1920, 1080, options, &urbs,
                           &err));
    CHECK(urbs.size() == 2);
    CHECK(urbs[0].size() == 4096);
    CHECK(urbs[0][0] == 0x02);
    CHECK(read_be32(urbs[0].data() + 1) == 5000);
    CHECK(urbs[0][5] == 0 && urbs[0][6] == 0 && urbs[0][7] == 0);
    CHECK(read_be32(urbs[0].data() + 8) == 1920);
    CHECK(read_be32(urbs[0].data() + 12) == 1080);
    CHECK(std::memcmp(urbs[0].data() + 16, au.data(), 4080) == 0);
    CHECK(urbs[1][0] == 0x02);
    for (int i = 1; i < 16; ++i) CHECK(urbs[1][i] == 0);
    CHECK(std::memcmp(urbs[1].data() + 16, au.data() + 4080, 5000 - 4080) ==
          0);
    CHECK(urbs[1][16 + 5000 - 4080] == 0);  // zero padding
    // The first bytes of the payload preserve the Annex-B start code.
    const uint8_t* payload = urbs[0].data() + 16;
    CHECK(payload[0] == 0 && payload[1] == 0 && payload[2] == 0 &&
          payload[3] == 1);
  }
  {
    // Small frame -> exactly one URB, no continuation header.
    std::vector<uint8_t> au = {0, 0, 0, 1, 0x09, 0x10};
    VideoUrbOptions options;
    std::vector<std::vector<uint8_t>> urbs;
    std::string err;
    CHECK(build_video_urbs(au.data(), au.size(), 1280, 720, options, &urbs,
                           &err));
    CHECK(urbs.size() == 1);
    CHECK(read_be32(urbs[0].data() + 1) == au.size());
    CHECK(read_be32(urbs[0].data() + 8) == 1280);
    CHECK(read_be32(urbs[0].data() + 12) == 720);
  }
  {
    // Smaller URB size (2048): payload budget 2032.
    std::vector<uint8_t> au(3000, 0x5A);
    au[0] = 0;
    au[1] = 0;
    au[2] = 1;
    VideoUrbOptions options;
    options.urb_size = 2048;
    std::vector<std::vector<uint8_t>> urbs;
    std::string err;
    CHECK(build_video_urbs(au.data(), au.size(), 640, 480, options, &urbs,
                           &err));
    CHECK(urbs.size() == 2);
    CHECK(urbs[0].size() == 2048);
    CHECK(read_be32(urbs[0].data() + 1) == 3000);
    CHECK(urbs[1][0] == 0x02 && urbs[1][1] == 0 && urbs[1][15] == 0);
  }
  {
    // Empty input and invalid URB size are rejected.
    VideoUrbOptions options;
    std::vector<std::vector<uint8_t>> urbs;
    std::string err;
    CHECK(!build_video_urbs(nullptr, 0, 1920, 1080, options, &urbs, &err));
    const uint8_t one = 0;
    options.urb_size = 8;
    CHECK(!build_video_urbs(&one, 1, 1920, 1080, options, &urbs, &err));
  }
  {
    // Dump sink round-trip + reassembly.
    const std::string path = "/tmp/usbdisplay_test_urbs.bin";
    std::vector<uint8_t> au(9000);
    au[0] = 0;
    au[1] = 0;
    au[2] = 0;
    au[3] = 1;
    for (size_t i = 4; i < au.size(); ++i) au[i] = static_cast<uint8_t>(i);
    VideoUrbOptions options;
    std::string err;
    auto sink = make_dump_video_sink(path, options);
    CHECK(sink->open(&err));
    CHECK(sink->send_frame(au.data(), au.size(), 1920, 1080, &err));
    CHECK(sink->packets_sent() == 3);
    CHECK(sink->frames_sent() == 1);
    sink->close();
    std::vector<hid::ReportDumpRecord> records;
    uint32_t report_size = 0;
    CHECK(hid::read_report_dump(path, &records, &report_size, &err));
    CHECK(report_size == 4096);
    CHECK(records.size() == 3);
    std::vector<uint8_t> rebuilt;
    uint32_t declared = 0;
    for (size_t i = 0; i < records.size(); ++i) {
      const auto& urb = records[i].report;
      CHECK(urb.size() == 4096);
      CHECK(urb[0] == 0x02);
      if (i == 0) {
        declared = read_be32(urb.data() + 1);
        CHECK(declared == au.size());
        CHECK(read_be32(urb.data() + 8) == 1920);
        CHECK(read_be32(urb.data() + 12) == 1080);
      } else {
        for (int j = 1; j < 16; ++j) CHECK(urb[j] == 0);
      }
      const size_t remaining = declared - rebuilt.size();
      const size_t take = std::min<size_t>(4080, remaining);
      rebuilt.insert(rebuilt.end(), urb.begin() + 16, urb.begin() + 16 + take);
    }
    CHECK(rebuilt == au);
    CHECK(declared == au.size());
    ::remove(path.c_str());
  }
  return testharness::finish("test_usb_video");
}
