#ifndef SEQ_SUPPORT_JSON_HPP_
#define SEQ_SUPPORT_JSON_HPP_

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace seq {

// A small JSON value. Objects keep insertion order so that serialized
// manifests are stable from run to run.
class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };
  using Array = std::vector<Json>;
  using Member = std::pair<std::string, Json>;
  using Object = std::vector<Member>;

  Json() = default;
  Json(std::nullptr_t) {}
  Json(bool value) : type_(Type::kBool), bool_(value) {}
  template <std::integral T>
    requires(!std::same_as<T, bool>)
  Json(T value)
      : type_(Type::kNumber),
        is_integer_(true),
        integer_(static_cast<std::int64_t>(value)),
        number_(static_cast<double>(value)) {}
  Json(double value) : type_(Type::kNumber), number_(value) {}
  Json(std::string value) : type_(Type::kString), string_(std::move(value)) {}
  Json(std::string_view value) : type_(Type::kString), string_(value) {}
  Json(const char* value) : type_(Type::kString), string_(value) {}

  static Json MakeArray();
  static Json MakeObject();

  Type type() const { return type_; }
  bool is_null() const { return type_ == Type::kNull; }
  bool is_bool() const { return type_ == Type::kBool; }
  bool is_number() const { return type_ == Type::kNumber; }
  bool is_integer() const { return type_ == Type::kNumber && is_integer_; }
  bool is_string() const { return type_ == Type::kString; }
  bool is_array() const { return type_ == Type::kArray; }
  bool is_object() const { return type_ == Type::kObject; }

  bool AsBool() const { return bool_; }
  double AsDouble() const { return number_; }
  std::int64_t AsInt() const { return integer_; }
  const std::string& AsString() const { return string_; }
  const Array& AsArray() const { return array_; }
  Array& AsArray() { return array_; }
  const Object& AsObject() const { return object_; }

  // Object access. Find returns nullptr when the key is absent or this value
  // is not an object.
  const Json* Find(std::string_view key) const;
  Json& Set(std::string key, Json value);
  // Convenience lookups that return a fallback on a missing or mistyped key.
  std::string GetString(std::string_view key, std::string fallback = "") const;
  std::int64_t GetInt(std::string_view key, std::int64_t fallback = 0) const;
  bool GetBool(std::string_view key, bool fallback = false) const;

  // Array access.
  void Push(Json value);

  // Serializes the value. indent < 0 produces a single line.
  std::string Dump(int indent = 2) const;

  // Parses strict JSON. Returns false and sets *error on failure.
  static bool Parse(std::string_view text, Json* out, std::string* error);

 private:
  void DumpTo(std::string* out, int indent, int depth) const;

  Type type_ = Type::kNull;
  bool bool_ = false;
  bool is_integer_ = false;
  std::int64_t integer_ = 0;
  double number_ = 0.0;
  std::string string_;
  Array array_;
  Object object_;
};

// Appends `text` to `out` as a quoted JSON string.
void AppendJsonString(std::string_view text, std::string* out);

}  // namespace seq

#endif  // SEQ_SUPPORT_JSON_HPP_
