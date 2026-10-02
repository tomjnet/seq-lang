#include "support/json.hpp"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace seq {

namespace {

constexpr int kMaxDepth = 64;

void AppendUtf8(std::uint32_t cp, std::string* out) {
  if (cp < 0x80) {
    out->push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
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

class Parser {
 public:
  explicit Parser(std::string_view text) : text_(text) {}

  bool ParseDocument(Json* out, std::string* error) {
    SkipWhitespace();
    if (!ParseValue(out, 0)) {
      *error = error_ + " at offset " + std::to_string(pos_);
      return false;
    }
    SkipWhitespace();
    if (pos_ != text_.size()) {
      *error = "unexpected trailing content at offset " + std::to_string(pos_);
      return false;
    }
    return true;
  }

 private:
  bool Fail(const char* message) {
    if (error_.empty()) error_ = message;
    return false;
  }

  void SkipWhitespace() {
    while (pos_ < text_.size()) {
      const char c = text_[pos_];
      if (c != ' ' && c != '\t' && c != '\n' && c != '\r') break;
      ++pos_;
    }
  }

  bool Consume(std::string_view literal) {
    if (text_.substr(pos_, literal.size()) != literal) return false;
    pos_ += literal.size();
    return true;
  }

  bool ParseValue(Json* out, int depth) {
    if (depth > kMaxDepth) return Fail("nesting too deep");
    if (pos_ >= text_.size()) return Fail("unexpected end of input");
    const char c = text_[pos_];
    if (c == '{') return ParseObject(out, depth);
    if (c == '[') return ParseArray(out, depth);
    if (c == '"') {
      std::string value;
      if (!ParseString(&value)) return false;
      *out = Json(std::move(value));
      return true;
    }
    if (Consume("true")) {
      *out = Json(true);
      return true;
    }
    if (Consume("false")) {
      *out = Json(false);
      return true;
    }
    if (Consume("null")) {
      *out = Json();
      return true;
    }
    if (c == '-' || (c >= '0' && c <= '9')) return ParseNumber(out);
    return Fail("unexpected character");
  }

  bool ParseObject(Json* out, int depth) {
    ++pos_;  // '{'
    *out = Json::MakeObject();
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == '}') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != '"') {
        return Fail("expected object key");
      }
      std::string key;
      if (!ParseString(&key)) return false;
      SkipWhitespace();
      if (pos_ >= text_.size() || text_[pos_] != ':')
        return Fail("expected ':'");
      ++pos_;
      SkipWhitespace();
      Json value;
      if (!ParseValue(&value, depth + 1)) return false;
      out->Set(std::move(key), std::move(value));
      SkipWhitespace();
      if (pos_ >= text_.size()) return Fail("unterminated object");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == '}') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or '}'");
    }
  }

  bool ParseArray(Json* out, int depth) {
    ++pos_;  // '['
    *out = Json::MakeArray();
    SkipWhitespace();
    if (pos_ < text_.size() && text_[pos_] == ']') {
      ++pos_;
      return true;
    }
    while (true) {
      SkipWhitespace();
      Json value;
      if (!ParseValue(&value, depth + 1)) return false;
      out->Push(std::move(value));
      SkipWhitespace();
      if (pos_ >= text_.size()) return Fail("unterminated array");
      if (text_[pos_] == ',') {
        ++pos_;
        continue;
      }
      if (text_[pos_] == ']') {
        ++pos_;
        return true;
      }
      return Fail("expected ',' or ']'");
    }
  }

  bool ParseHex4(std::uint32_t* value) {
    if (pos_ + 4 > text_.size()) return Fail("truncated \\u escape");
    std::uint32_t result = 0;
    for (int i = 0; i < 4; ++i) {
      const char c = text_[pos_ + i];
      result <<= 4;
      if (c >= '0' && c <= '9') {
        result |= static_cast<std::uint32_t>(c - '0');
      } else if (c >= 'a' && c <= 'f') {
        result |= static_cast<std::uint32_t>(c - 'a' + 10);
      } else if (c >= 'A' && c <= 'F') {
        result |= static_cast<std::uint32_t>(c - 'A' + 10);
      } else {
        return Fail("invalid \\u escape");
      }
    }
    pos_ += 4;
    *value = result;
    return true;
  }

  bool ParseString(std::string* out) {
    ++pos_;  // opening quote
    while (true) {
      if (pos_ >= text_.size()) return Fail("unterminated string");
      const unsigned char c = static_cast<unsigned char>(text_[pos_]);
      if (c == '"') {
        ++pos_;
        return true;
      }
      if (c < 0x20) return Fail("control character in string");
      if (c != '\\') {
        out->push_back(static_cast<char>(c));
        ++pos_;
        continue;
      }
      ++pos_;
      if (pos_ >= text_.size()) return Fail("unterminated escape");
      const char e = text_[pos_++];
      switch (e) {
        case '"':
          out->push_back('"');
          break;
        case '\\':
          out->push_back('\\');
          break;
        case '/':
          out->push_back('/');
          break;
        case 'b':
          out->push_back('\b');
          break;
        case 'f':
          out->push_back('\f');
          break;
        case 'n':
          out->push_back('\n');
          break;
        case 'r':
          out->push_back('\r');
          break;
        case 't':
          out->push_back('\t');
          break;
        case 'u': {
          std::uint32_t cp = 0;
          if (!ParseHex4(&cp)) return false;
          if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (!Consume("\\u")) return Fail("unpaired surrogate");
            std::uint32_t low = 0;
            if (!ParseHex4(&low)) return false;
            if (low < 0xDC00 || low > 0xDFFF) return Fail("unpaired surrogate");
            cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
          } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
            return Fail("unpaired surrogate");
          }
          AppendUtf8(cp, out);
          break;
        }
        default:
          return Fail("invalid escape");
      }
    }
  }

  bool ParseNumber(Json* out) {
    const std::size_t start = pos_;
    bool integral = true;
    if (text_[pos_] == '-') ++pos_;
    if (pos_ >= text_.size()) return Fail("invalid number");
    if (text_[pos_] == '0') {
      ++pos_;
    } else if (text_[pos_] >= '1' && text_[pos_] <= '9') {
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
    } else {
      return Fail("invalid number");
    }
    if (pos_ < text_.size() && text_[pos_] == '.') {
      integral = false;
      ++pos_;
      const std::size_t digits = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
      if (pos_ == digits) return Fail("invalid number");
    }
    if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
      integral = false;
      ++pos_;
      if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) {
        ++pos_;
      }
      const std::size_t digits = pos_;
      while (pos_ < text_.size() && text_[pos_] >= '0' && text_[pos_] <= '9') {
        ++pos_;
      }
      if (pos_ == digits) return Fail("invalid number");
    }
    const std::string token(text_.substr(start, pos_ - start));
    if (integral) {
      errno = 0;
      char* end = nullptr;
      const long long value = std::strtoll(token.c_str(), &end, 10);
      if (errno == 0 && end != nullptr && *end == '\0') {
        *out = Json(value);
        return true;
      }
    }
    *out = Json(std::strtod(token.c_str(), nullptr));
    return true;
  }

  std::string_view text_;
  std::size_t pos_ = 0;
  std::string error_;
};

void AppendIndent(std::string* out, int indent, int depth) {
  if (indent < 0) return;
  out->push_back('\n');
  out->append(
      static_cast<std::size_t>(indent) * static_cast<std::size_t>(depth), ' ');
}

}  // namespace

void AppendJsonString(std::string_view text, std::string* out) {
  out->push_back('"');
  for (const char ch : text) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out->append("\\\"");
        break;
      case '\\':
        out->append("\\\\");
        break;
      case '\n':
        out->append("\\n");
        break;
      case '\r':
        out->append("\\r");
        break;
      case '\t':
        out->append("\\t");
        break;
      case '\b':
        out->append("\\b");
        break;
      case '\f':
        out->append("\\f");
        break;
      default:
        if (c < 0x20 || c == 0x7F) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out->append(buffer);
        } else {
          out->push_back(ch);
        }
    }
  }
  out->push_back('"');
}

Json Json::MakeArray() {
  Json value;
  value.type_ = Type::kArray;
  return value;
}

Json Json::MakeObject() {
  Json value;
  value.type_ = Type::kObject;
  return value;
}

const Json* Json::Find(std::string_view key) const {
  if (type_ != Type::kObject) return nullptr;
  for (const Member& member : object_) {
    if (member.first == key) return &member.second;
  }
  return nullptr;
}

Json& Json::Set(std::string key, Json value) {
  if (type_ != Type::kObject) {
    *this = MakeObject();
  }
  for (Member& member : object_) {
    if (member.first == key) {
      member.second = std::move(value);
      return *this;
    }
  }
  object_.emplace_back(std::move(key), std::move(value));
  return *this;
}

std::string Json::GetString(std::string_view key, std::string fallback) const {
  const Json* value = Find(key);
  if (value == nullptr || !value->is_string()) return fallback;
  return value->AsString();
}

std::int64_t Json::GetInt(std::string_view key, std::int64_t fallback) const {
  const Json* value = Find(key);
  if (value == nullptr || !value->is_integer()) return fallback;
  return value->AsInt();
}

bool Json::GetBool(std::string_view key, bool fallback) const {
  const Json* value = Find(key);
  if (value == nullptr || !value->is_bool()) return fallback;
  return value->AsBool();
}

void Json::Push(Json value) {
  if (type_ != Type::kArray) {
    *this = MakeArray();
  }
  array_.push_back(std::move(value));
}

std::string Json::Dump(int indent) const {
  std::string out;
  DumpTo(&out, indent, 0);
  return out;
}

void Json::DumpTo(std::string* out, int indent, int depth) const {
  switch (type_) {
    case Type::kNull:
      out->append("null");
      break;
    case Type::kBool:
      out->append(bool_ ? "true" : "false");
      break;
    case Type::kNumber:
      if (is_integer_) {
        out->append(std::to_string(integer_));
      } else if (!std::isfinite(number_)) {
        out->append("null");
      } else {
        char buffer[40];
        std::snprintf(buffer, sizeof(buffer), "%.17g", number_);
        out->append(buffer);
        // Keep a non-integer spelling so the value reads back as a double.
        if (std::string_view(buffer).find_first_of(".eE") ==
            std::string_view::npos) {
          out->append(".0");
        }
      }
      break;
    case Type::kString:
      AppendJsonString(string_, out);
      break;
    case Type::kArray:
      if (array_.empty()) {
        out->append("[]");
        break;
      }
      out->push_back('[');
      for (std::size_t i = 0; i < array_.size(); ++i) {
        if (i != 0) out->push_back(',');
        AppendIndent(out, indent, depth + 1);
        array_[i].DumpTo(out, indent, depth + 1);
      }
      AppendIndent(out, indent, depth);
      out->push_back(']');
      break;
    case Type::kObject:
      if (object_.empty()) {
        out->append("{}");
        break;
      }
      out->push_back('{');
      for (std::size_t i = 0; i < object_.size(); ++i) {
        if (i != 0) out->push_back(',');
        AppendIndent(out, indent, depth + 1);
        AppendJsonString(object_[i].first, out);
        out->append(indent < 0 ? ":" : ": ");
        object_[i].second.DumpTo(out, indent, depth + 1);
      }
      AppendIndent(out, indent, depth);
      out->push_back('}');
      break;
  }
}

bool Json::Parse(std::string_view text, Json* out, std::string* error) {
  Parser parser(text);
  return parser.ParseDocument(out, error);
}

}  // namespace seq
