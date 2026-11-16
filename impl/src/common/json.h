// SPDX-License-Identifier: MIT
// 控制通道使用的精简 JSON 解析与生成，无外部依赖，
// 支持空值、布尔值、数字、字符串、数组与对象。
#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <vector>

namespace usbdisplay {
namespace json {

class Value {
 public:
  enum class Type { Null, Bool, Number, String, Array, Object };

  Value() = default;
  explicit Value(bool b);
  explicit Value(double d);
  explicit Value(int i);
  explicit Value(const char* s);
  explicit Value(std::string s);

  static Value array();
  static Value object();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::Null; }
  bool is_bool() const { return type_ == Type::Bool; }
  bool is_number() const { return type_ == Type::Number; }
  bool is_string() const { return type_ == Type::String; }
  bool is_array() const { return type_ == Type::Array; }
  bool is_object() const { return type_ == Type::Object; }

  bool as_bool(bool fallback = false) const;
  double as_number(double fallback = 0.0) const;
  const std::string& as_string() const { return string_; }

  const std::vector<Value>& items() const { return array_; }
  std::vector<Value>& items() { return array_; }
  const std::map<std::string, Value>& members() const { return object_; }
  std::map<std::string, Value>& members() { return object_; }

  // 访问对象成员，成员不存在或对象类型不符时返回空指针。
  const Value* find(const std::string& key) const;
  Value* find(const std::string& key);
  const Value& operator[](const std::string& key) const;
  Value& operator[](const std::string& key);

  void push_back(Value v);
  void set(const std::string& key, Value v);

  std::string dump() const;
  void dump_to(std::string* out) const;

  static bool parse(const std::string& text, Value* out, std::string* err);

 private:
  Type type_ = Type::Null;
  bool bool_ = false;
  double number_ = 0.0;
  std::string string_;
  std::vector<Value> array_;
  std::map<std::string, Value> object_;

  static const Value& null_value();
};

// 便捷函数：读取对象中指定键的字符串值。
bool get_string(const Value& v, const std::string& key, std::string* out);

}  // namespace json
}  // namespace usbdisplay
