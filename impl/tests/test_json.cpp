// SPDX-License-Identifier: MIT
#include "common/json.h"
#include "test_harness.h"

using usbdisplay::json::Value;

int main() {
  std::string err;
  {
    Value v;
    CHECK(Value::parse("{\"a\":1,\"b\":[true,false,null],\"c\":\"x\"}",
                       &v, &err));
    CHECK(v.is_object());
    CHECK(v["a"].is_number() && v["a"].as_number() == 1.0);
    CHECK(v["b"].is_array() && v["b"].items().size() == 3);
    CHECK(v["b"].items()[0].as_bool(false) == true);
    CHECK(v["b"].items()[2].is_null());
    CHECK(v["c"].as_string() == "x");
    CHECK(v.dump() == "{\"a\":1,\"b\":[true,false,null],\"c\":\"x\"}");
  }
  {
    Value v;
    CHECK(Value::parse("\"a\\\"b\\\\c\\n\\u00e9\"", &v, &err));
    CHECK(v.as_string() == "a\"b\\c\n\xC3\xA9");
  }
  {
    Value v;
    CHECK(Value::parse("\"\\ud83d\\ude00\"", &v, &err));
    CHECK(v.as_string() == "\xF0\x9F\x98\x80");  // U+1F600
  }
  {
    Value v;
    CHECK(Value::parse(" 42 ", &v, &err));
    CHECK(v.as_number() == 42);
    CHECK(Value::parse("-3.5e2", &v, &err));
    CHECK(v.as_number() == -350.0);
  }
  {
    Value v;
    CHECK(!Value::parse("{\"a\":}", &v, &err));
    CHECK(!Value::parse("[1,2", &v, &err));
    CHECK(!Value::parse("{} junk", &v, &err));
    CHECK(!Value::parse("\"\\q\"", &v, &err));
  }
  {
    Value obj = Value::object();
    obj.set("jsonrpc", Value("2.0"));
    obj.set("method", Value("heartbeat"));
    Value params = Value::object();
    params.set("session-id", Value("abc"));
    obj.set("params", params);
    obj.set("id", Value(1));
    const std::string text = obj.dump();
    CHECK(text ==
          "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"heartbeat\","
          "\"params\":{\"session-id\":\"abc\"}}");
    Value reparsed;
    CHECK(Value::parse(text, &reparsed, &err));
    CHECK(reparsed["method"].as_string() == "heartbeat");
    std::string value;
    CHECK(usbdisplay::json::get_string(reparsed, "method", &value) &&
          value == "heartbeat");
    CHECK(!usbdisplay::json::get_string(reparsed, "missing", &value));
  }
  return testharness::finish("test_json");
}
