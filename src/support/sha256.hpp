#ifndef SEQ_SUPPORT_SHA256_HPP_
#define SEQ_SUPPORT_SHA256_HPP_

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace seq {

// Incremental SHA-256 (FIPS 180-4).
class Sha256 {
 public:
  Sha256();
  void Update(const void* data, std::size_t size);
  void Update(std::string_view text) { Update(text.data(), text.size()); }
  // Finishes the hash and returns it as 64 lowercase hex digits. The object
  // must not be used afterwards.
  std::string HexDigest();

 private:
  void Transform(const std::uint8_t* block);

  std::array<std::uint32_t, 8> state_;
  std::array<std::uint8_t, 64> buffer_;
  std::size_t buffered_ = 0;
  std::uint64_t total_bytes_ = 0;
};

std::string Sha256Hex(std::string_view text);

// Hashes a file by streaming it. Returns false and sets *error if the file
// cannot be read.
bool Sha256File(const std::filesystem::path& path, std::string* hex,
                std::string* error);

}  // namespace seq

#endif  // SEQ_SUPPORT_SHA256_HPP_
