// Minimal JSON DOM parser (enough for glTF and map data). No dependencies.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace eng {

struct Json {
  enum class Type { Null, Bool, Number, String, Array, Object };
  Type type = Type::Null;
  bool b = false;
  double num = 0;
  std::string str;
  std::vector<Json> arr;
  std::map<std::string, Json> obj;

  bool isNull() const { return type == Type::Null; }
  bool isObject() const { return type == Type::Object; }
  bool isArray() const { return type == Type::Array; }
  bool isNumber() const { return type == Type::Number; }
  bool isString() const { return type == Type::String; }

  // Lookups return a static null on miss so chains never crash.
  const Json& operator[](std::string_view key) const;
  const Json& operator[](size_t i) const;
  bool has(std::string_view key) const { return type == Type::Object && obj.count(std::string(key)); }
  size_t size() const { return type == Type::Array ? arr.size() : type == Type::Object ? obj.size() : 0; }

  double numberOr(double d) const { return type == Type::Number ? num : d; }
  int intOr(int d) const { return type == Type::Number ? (int)num : d; }
  bool boolOr(bool d) const { return type == Type::Bool ? b : d; }
  std::string stringOr(std::string d) const { return type == Type::String ? str : d; }

  static Json parse(std::string_view text, std::string* error = nullptr);
};

} // namespace eng
