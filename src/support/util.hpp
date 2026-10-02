#ifndef SEQ_SUPPORT_UTIL_HPP_
#define SEQ_SUPPORT_UTIL_HPP_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace seq {

// --- Text ------------------------------------------------------------------

// Returns the byte length of the well-formed UTF-8 sequence that starts at
// text[pos], or 0 if the bytes there are not valid UTF-8 (overlong forms,
// surrogates, and values above U+10FFFF are invalid).
std::size_t Utf8SequenceLength(std::string_view text, std::size_t pos);

bool IsValidUtf8(std::string_view text);

// Number of code points in `text`, which must be valid UTF-8.
std::size_t Utf8Length(std::string_view text);

std::vector<std::string> SplitLines(std::string_view text);
std::string_view TrimWhitespace(std::string_view text);
bool StartsWith(std::string_view text, std::string_view prefix);
bool EndsWith(std::string_view text, std::string_view suffix);
std::string ToLower(std::string_view text);
std::string ToUpper(std::string_view text);
std::string Join(const std::vector<std::string>& parts, std::string_view sep);

// Formats a byte count for people, for example "1.2 KiB".
std::string FormatBytes(std::uintmax_t bytes);

// --- Files -----------------------------------------------------------------

// Reads a whole file. Fails if it is larger than max_bytes.
bool ReadFile(const std::filesystem::path& path, std::string* out,
              std::string* error,
              std::uintmax_t max_bytes = 64u * 1024u * 1024u);

// Writes `data` to a temporary sibling and renames it over `path`, so readers
// never observe a partially written file.
bool WriteFile(const std::filesystem::path& path, std::string_view data,
               std::string* error);

// Path of the running executable, or an empty path if it cannot be found.
std::filesystem::path ExecutablePath();

// UTC timestamp such as 2026-09-30T18:04:05Z.
std::string TimestampUtc();

// Relative path with forward slashes, for display and manifests.
std::string GenericRelative(const std::filesystem::path& path,
                            const std::filesystem::path& base);

// True if `relpath` is a safe relative path: nonempty, valid UTF-8 without
// control characters or backslashes, not absolute, and with no empty, ".", or
// ".." component.
bool IsSafeRelativePath(std::string_view relpath);

}  // namespace seq

#endif  // SEQ_SUPPORT_UTIL_HPP_
