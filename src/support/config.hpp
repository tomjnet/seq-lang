#ifndef SEQ_SUPPORT_CONFIG_HPP_
#define SEQ_SUPPORT_CONFIG_HPP_

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace seq {

// One configurable setting and its built-in default.
struct Setting {
  const char* key;
  const char* default_value;
  const char* description;
};

// Every setting seqc understands. docs/architecture.md lists the same table.
const std::vector<Setting>& AllSettings();

// Settings resolved in precedence order: `--set key=value` flags, then
// SEQC_<SECTION>_<KEY> environment variables, then the configuration file,
// then built-in defaults.
class Config {
 public:
  // Returns false and sets *error on an unreadable or malformed configuration
  // file, an unknown key, or a malformed override.
  static bool Load(const std::vector<std::string>& overrides, Config* out,
                   std::string* error);

  // A Config holding only built-in defaults.
  static Config Defaults();

  std::string Get(std::string_view key) const;
  std::int64_t GetInt(std::string_view key) const;
  void Set(std::string_view key, std::string value);

  // $XDG_CONFIG_HOME/seqc/config.toml, defaulting to ~/.config/seqc/.
  static std::filesystem::path ConfigFilePath();
  // $XDG_CACHE_HOME/seqc, defaulting to ~/.cache/seqc.
  static std::filesystem::path CacheDir();

 private:
  std::map<std::string, std::string, std::less<>> values_;
};

// Parses the TOML subset used by config.toml: `[section]` headers and
// `key = value` lines whose value is a double-quoted string, an integer, or
// true/false. Keys are returned as "section.key".
bool ParseConfigToml(std::string_view text,
                     std::vector<std::pair<std::string, std::string>>* out,
                     std::string* error);

}  // namespace seq

#endif  // SEQ_SUPPORT_CONFIG_HPP_
