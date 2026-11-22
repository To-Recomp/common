// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace torecomp {

// Streaming SHA-1 (FIPS 180-4). Used to compare dumps with Redump entries, not for security.
class Sha1 {
public:
  using Digest = std::array<std::uint8_t, 20>;

  Sha1() { reset(); }

  void reset();
  void update(const void* data, std::size_t size);
  // Returns the digest and resets the object.
  Digest finish();

  static std::string hex(const Digest& digest);
  static std::string hex_of(const void* data, std::size_t size);

private:
  void process_block(const std::uint8_t* block);

  std::uint32_t h_[5];
  std::uint8_t buffer_[64];
  std::size_t buffer_len_;
  std::uint64_t total_bytes_;
};

}  // namespace torecomp
