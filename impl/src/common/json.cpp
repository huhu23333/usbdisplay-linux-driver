// SPDX-License-Identifier: MIT
#include "common/json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>

namespace usbdisplay {
namespace json {
namespace {

const Value& null_singleton() {
  static const Value v;
  return v;
}

void dump_string(const std::string& s, std::string* out) {
  out->push_back('"');
  for (unsigned char c : s) {
    switch (c) {
      case '"': *out += "\\\""; break;
      case '\\': *out += "\\\\"; break;
      case '\b': *out += "\\b"; break;
      case '\f': *out += "\\f"; break;
      case '\n': *out += "\\n"; break;
      case '\r': *out += "\\r"; break;
      case '\t': *out += "\\t"; break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          *out += buf;
        } else {
          out->push_back(static_cast<char>(c));
        }
    }
  }
  out->push_back('"');
}

class Parser {
 public:
  Parser(const std::string& text) : text_(text) {}

  bool parse(Value* out, std::string* err) {
    skip_ws();
    if (!parse_value(out, 0)) {
      if (err) *err = error_;
      return false;
    }
    skip_ws();
    if (pos_ != text_.size()) {
      if (err) *err = "trailing characters at offset " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  static constexpr int kMaxDepth = 64;

  bool parse_value(Value* out, int depth) {
    if (depth > kMaxDepth) {
      error_ = "nesting too deep";
      return false;
    }
    skip_ws();
    if (pos_ >= text_.size()) {
      error_ = "unexpected end of input";
      return false;
    }
    const char c = text_[pos_];
    switch (c) {
      case '{': return parse_object(out, depth);
      case '[': return parse_array(out, depth);
      case '"': {
        std::string s;
        if (!parse_string(&s)) return false;
        *out = Value(std::move(s));
        return true;
      }
      case 't':
        if (text_.compare(pos_, 4, "true") == 0) {
          pos_ += 4;
          *out = Value(true);
          return true;
        }
        error_ = "invalid literal";
        return false;
      case 'f':
        if (text_.compare(pos_, 5, "false") == 0) {
          pos_ += 5;
          *out = Value(false);
          return true;
        }
        error_ = "invalid literal";
        return false;
      case 'n':
        if (text_.compare(pos_, 4, "null") == 0) {
          pos_ += 4;
          *out = Value();
          return true;
        }
        error_ = "invalid literal";
        return false;
      default: return parse_number(out);
    }
  }

  bool parse_object(Value* out, int depth) {
    ++pos_;  // '{'
    Value obj = Value::object();
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      *out = std::move(obj);
      return true;
    }
    for (;;) {
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        error_ = "expected object key";
        return false;
      }
      std::string key;
      if (!parse_string(&key)) return false;
      skip_ws();
      if (pos_ >= text_.size() || text_[pos_] != ':') {
        error_ = "expected ':'";
        return false;
      }
      ++pos_;
      Value v;
      if (!parse_value(&v, depth + 1)) return false;
      obj.members()[key] = std::move(v);
      skip_ws();
      if (pos_ >= text_.size()) {
        error_ = "unterminated object";
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        *out = std::move(obj);
        return true;
      }
      error_ = "expected ',' or '}'";
      return false;
    }
  }

  bool parse_array(Value* out, int depth) {
    ++pos_;  // '['
    Value arr = Value::array();
    skip_ws();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      *out = std::move(arr);
      return true;
    }
    for (;;) {
      Value v;
      if (!parse_value(&v, depth + 1)) return false;
      arr.push_back(std::move(v));
      skip_ws();
      if (pos_ >= text_.size()) {
        error_ = "unterminated array";
        return false;
      }
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        *out = std::move(arr);
        return true;
      }
      error_ = "expected ',' or ']'";
      return false;
    }
  }

  bool parse_string(std::string* out) {
    ++pos_;  // opening quote
    out->clear();
    while (pos_ < text_.size()) {
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c == '\\') {
        ++pos_;
        if (pos_ >= text_.size()) {
          error_ = "unterminated escape";
          return false;
        }
        const char e = text_[pos_++];
        switch (e) {
          case '"': out->push_back('"'); break;
          case '\\': out->push_back('\\'); break;
          case '/': out->push_back('/'); break;
          case 'b': out->push_back('\b'); break;
          case 'f': out->push_back('\f'); break;
          case 'n': out->push_back('\n'); break;
          case 'r': out->push_back('\r'); break;
          case 't': out->push_back('\t'); break;
          case 'u': {
            unsigned cp = 0;
            if (!parse_hex4(&cp)) return false;
            if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= text_.size() &&
                text_[pos_] == '\\' && text_[pos_ + 1] == 'u') {
              pos_ += 2;
              unsigned low = 0;
              if (!parse_hex4(&low)) return false;
              if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
              }
            }
            append_utf8(cp, out);
            break;
          }
          default:
            error_ = "invalid escape";
            return false;
        }
        continue;
      }
      if (c < 0x20) {
        error_ = "control character in string";
        return false;
      }
      out->push_back(static_cast<char>(c));
      ++pos_;
    }
    error_ = "unterminated string";
    return false;
  }

  bool parse_hex4(unsigned* out) {
    if (pos_ + 4 > text_.size()) {
      error_ = "truncated \\u escape";
      return false;
    }
    unsigned v = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + i];
      v <<= 4;
      if (c >= '0' && c <= '9') v |= static_cast<unsigned>(c - '0');
      else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned>(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned>(c - 'A' + 10);
      else {
        error_ = "invalid hex digit";
        return false;
      }
    }
    pos_ += 4;
    *out = v;
    return true;
  }

  static void append_utf8(unsigned cp, std::string* out) {
    if (cp <= 0x7F) {
      out->push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
      out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
      out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
      out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
      out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
  }

  bool parse_number(Value* out) {
    const size_t start = pos_;
    if (pos_ < text_.size() && (text_[pos_] == '-' || text_[pos_] == '+')) ++pos_;
    bool digits = false;
    while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
      ++pos_;
      digits = true;
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
        digits = true;
      }
    }
    if (!digits) {
      error_ = "invalid number";
      return false;
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) ++pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') ++pos_;
    }
    char* end = nullptr;
    const double v = std::strtod(text_.c_str() + start, &end);
    if (end == text_.c_str() + start) {
      error_ = "invalid number";
      return false;
    }
    *out = Value(v);
    return true;
  }

  void skip_ws() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
        ++pos_;
      } else {
        break;
      }
    }
  }

  const std::string& text_;
  size_t pos_ = 0;
  std::string error_;
};

}  // namespace

const Value& Value::null_value() { return null_singleton(); }

Value::Value(bool b) : type_(Type::Bool), bool_(b) {}
Value::Value(double d) : type_(Type::Number), number_(d) {}
Value::Value(int i) : type_(Type::Number), number_(static_cast<double>(i)) {}
Value::Value(const char* s) : type_(Type::String), string_(s ? s : "") {}
Value::Value(std::string s) : type_(Type::String), string_(std::move(s)) {}

Value Value::array() {
  Value v;
  v.type_ = Type::Array;
  return v;
}
Value Value::object() {
  Value v;
  v.type_ = Type::Object;
  return v;
}

bool Value::as_bool(bool fallback) const {
  return type_ == Type::Bool ? bool_ : fallback;
}
double Value::as_number(double fallback) const {
  return type_ == Type::Number ? number_ : fallback;
}

const Value* Value::find(const std::string& key) const {
  if (type_ != Type::Object) return nullptr;
  const auto it = object_.find(key);
  return it == object_.end() ? nullptr : &it->second;
}
Value* Value::find(const std::string& key) {
  if (type_ != Type::Object) return nullptr;
  const auto it = object_.find(key);
  return it == object_.end() ? nullptr : &it->second;
}

const Value& Value::operator[](const std::string& key) const {
  const Value* v = find(key);
  return v ? *v : null_value();
}
Value& Value::operator[](const std::string& key) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    array_.clear();
    string_.clear();
    number_ = 0;
  }
  return object_[key];
}

void Value::push_back(Value v) {
  if (type_ != Type::Array) {
    type_ = Type::Array;
    object_.clear();
    string_.clear();
    number_ = 0;
  }
  array_.push_back(std::move(v));
}

void Value::set(const std::string& key, Value v) {
  if (type_ != Type::Object) {
    type_ = Type::Object;
    array_.clear();
    string_.clear();
    number_ = 0;
  }
  object_[key] = std::move(v);
}

std::string Value::dump() const {
  std::string out;
  dump_to(&out);
  return out;
}

void Value::dump_to(std::string* out) const {
  switch (type_) {
    case Type::Null: *out += "null"; break;
    case Type::Bool: *out += bool_ ? "true" : "false"; break;
    case Type::Number: {
      if (std::isfinite(number_) && number_ == std::floor(number_) &&
          std::fabs(number_) < 9.0e15) {
        *out += std::to_string(static_cast<long long>(number_));
      } else {
        char buf[40];
        std::snprintf(buf, sizeof(buf), "%.17g", number_);
        *out += buf;
      }
      break;
    }
    case Type::String: dump_string(string_, out); break;
    case Type::Array: {
      *out += '[';
      for (size_t i = 0; i < array_.size(); ++i) {
        if (i) *out += ',';
        array_[i].dump_to(out);
      }
      *out += ']';
      break;
    }
    case Type::Object: {
      *out += '{';
      bool first = true;
      for (const auto& kv : object_) {
        if (!first) *out += ',';
        first = false;
        dump_string(kv.first, out);
        *out += ':';
        kv.second.dump_to(out);
      }
      *out += '}';
      break;
    }
  }
}

bool Value::parse(const std::string& text, Value* out, std::string* err) {
  Parser p(text);
  return p.parse(out, err);
}

bool get_string(const Value& v, const std::string& key, std::string* out) {
  const Value* member = v.find(key);
  if (!member || !member->is_string()) return false;
  *out = member->as_string();
  return true;
}

}  // namespace json
}  // namespace usbdisplay
