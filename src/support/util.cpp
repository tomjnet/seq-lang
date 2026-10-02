#include "support/util.hpp"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <system_error>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace seq {

namespace fs = std::filesystem;

std::size_t Utf8SequenceLength(std::string_view text, std::size_t pos) {
  const auto byte = [&](std::size_t i) {
    return static_cast<unsigned char>(text[i]);
  };
  const auto continuation = [&](std::size_t i) {
    return i < text.size() && (byte(i) & 0xC0) == 0x80;
  };
  if (pos >= text.size()) return 0;
  const unsigned char lead = byte(pos);
  if (lead < 0x80) return 1;
  if (lead >= 0xC2 && lead <= 0xDF) {
    return continuation(pos + 1) ? 2 : 0;
  }
  if (lead >= 0xE0 && lead <= 0xEF) {
    if (!continuation(pos + 1) || !continuation(pos + 2)) return 0;
    const unsigned char second = byte(pos + 1);
    if (lead == 0xE0 && second < 0xA0) return 0;   // overlong
    if (lead == 0xED && second >= 0xA0) return 0;  // surrogate
    return 3;
  }
  if (lead >= 0xF0 && lead <= 0xF4) {
    if (!continuation(pos + 1) || !continuation(pos + 2) ||
        !continuation(pos + 3)) {
      return 0;
    }
    const unsigned char second = byte(pos + 1);
    if (lead == 0xF0 && second < 0x90) return 0;   // overlong
    if (lead == 0xF4 && second >= 0x90) return 0;  // above U+10FFFF
    return 4;
  }
  return 0;
}

bool IsValidUtf8(std::string_view text) {
  std::size_t pos = 0;
  while (pos < text.size()) {
    const std::size_t length = Utf8SequenceLength(text, pos);
    if (length == 0) return false;
    pos += length;
  }
  return true;
}

std::size_t Utf8Length(std::string_view text) {
  std::size_t count = 0;
  for (const char c : text) {
    if ((static_cast<unsigned char>(c) & 0xC0) != 0x80) ++count;
  }
  return count;
}

std::vector<std::string> SplitLines(std::string_view text) {
  std::vector<std::string> lines;
  std::size_t start = 0;
  while (start <= text.size()) {
    const std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) {
      if (start < text.size()) lines.emplace_back(text.substr(start));
      break;
    }
    lines.emplace_back(text.substr(start, end - start));
    start = end + 1;
  }
  return lines;
}

std::string_view TrimWhitespace(std::string_view text) {
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.front())) != 0) {
    text.remove_prefix(1);
  }
  while (!text.empty() &&
         std::isspace(static_cast<unsigned char>(text.back())) != 0) {
    text.remove_suffix(1);
  }
  return text;
}

bool StartsWith(std::string_view text, std::string_view prefix) {
  return text.substr(0, prefix.size()) == prefix;
}

bool EndsWith(std::string_view text, std::string_view suffix) {
  return text.size() >= suffix.size() &&
         text.substr(text.size() - suffix.size()) == suffix;
}

std::string ToLower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string ToUpper(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return out;
}

std::string Join(const std::vector<std::string>& parts, std::string_view sep) {
  std::string out;
  for (std::size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) out.append(sep);
    out.append(parts[i]);
  }
  return out;
}

std::string FormatBytes(std::uintmax_t bytes) {
  char buffer[48];
  if (bytes < 1024) {
    std::snprintf(buffer, sizeof(buffer), "%llu bytes",
                  static_cast<unsigned long long>(bytes));
  } else if (bytes < 1024u * 1024u) {
    std::snprintf(buffer, sizeof(buffer), "%.1f KiB",
                  static_cast<double>(bytes) / 1024.0);
  } else if (bytes < 1024u * 1024u * 1024u) {
    std::snprintf(buffer, sizeof(buffer), "%.1f MiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else {
    std::snprintf(buffer, sizeof(buffer), "%.2f GiB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
  }
  return buffer;
}

bool ReadFile(const fs::path& path, std::string* out, std::string* error,
              std::uintmax_t max_bytes) {
  std::error_code ec;
  const fs::file_status status = fs::status(path, ec);
  if (ec || !fs::exists(status)) {
    *error = "file not found: " + path.string();
    return false;
  }
  if (!fs::is_regular_file(status)) {
    *error = "not a regular file: " + path.string();
    return false;
  }
  const std::uintmax_t size = fs::file_size(path, ec);
  if (!ec && size > max_bytes) {
    *error =
        "file is larger than " + FormatBytes(max_bytes) + ": " + path.string();
    return false;
  }
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    *error = "cannot open " + path.string();
    return false;
  }
  out->assign(std::istreambuf_iterator<char>(in),
              std::istreambuf_iterator<char>());
  if (in.bad()) {
    *error = "cannot read " + path.string();
    return false;
  }
  return true;
}

bool WriteFile(const fs::path& path, std::string_view data,
               std::string* error) {
  fs::path temporary = path;
  temporary += ".tmp";
  {
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    if (!out) {
      *error = "cannot create " + temporary.string();
      return false;
    }
    out.write(data.data(), static_cast<std::streamsize>(data.size()));
    out.flush();
    if (!out) {
      *error = "cannot write " + temporary.string();
      return false;
    }
  }
  std::error_code ec;
  fs::rename(temporary, path, ec);
  if (ec) {
    fs::remove(temporary, ec);
    *error = "cannot write " + path.string();
    return false;
  }
  return true;
}

fs::path ExecutablePath() {
#if defined(_WIN32)
  wchar_t buffer[32768];
  const DWORD length = GetModuleFileNameW(nullptr, buffer, 32768);
  if (length == 0 || length >= 32768) return {};
  return fs::path(std::wstring(buffer, length));
#else
  std::error_code ec;
  fs::path path = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return {};
  return path;
#endif
}

std::string TimestampUtc() {
  const std::time_t now = std::time(nullptr);
  std::tm parts{};
#if defined(_WIN32)
  gmtime_s(&parts, &now);
#else
  gmtime_r(&now, &parts);
#endif
  char buffer[32];
  std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts);
  return buffer;
}

std::string GenericRelative(const fs::path& path, const fs::path& base) {
  // Purely lexical: fs::relative would resolve symbolic links and report the
  // link's target instead of the name that is actually in the directory.
  const fs::path relative = path.lexically_relative(base);
  if (relative.empty()) return path.generic_string();
  return relative.generic_string();
}

bool IsSafeRelativePath(std::string_view relpath) {
  if (relpath.empty() || relpath.size() > 255) return false;
  if (!IsValidUtf8(relpath)) return false;
  if (relpath.front() == '/') return false;
  for (const char c : relpath) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 0x20 || u == 0x7F || c == '\\') return false;
  }
  std::size_t start = 0;
  while (start <= relpath.size()) {
    std::size_t end = relpath.find('/', start);
    if (end == std::string_view::npos) end = relpath.size();
    const std::string_view component = relpath.substr(start, end - start);
    if (component.empty() || component == "." || component == "..") {
      return false;
    }
    start = end + 1;
  }
  return true;
}

}  // namespace seq
