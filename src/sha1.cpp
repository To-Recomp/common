// SPDX-License-Identifier: GPL-3.0-or-later
#include "torecomp/sha1.hpp"

#include <algorithm>
#include <cstring>

namespace torecomp {

namespace {
inline std::uint32_t rol(std::uint32_t v, unsigned s) { return (v << s) | (v >> (32 - s)); }
}  // namespace

void Sha1::reset() {
  h_[0] = 0x67452301u;
  h_[1] = 0xEFCDAB89u;
  h_[2] = 0x98BADCFEu;
  h_[3] = 0x10325476u;
  h_[4] = 0xC3D2E1F0u;
  buffer_len_ = 0;
  total_bytes_ = 0;
}

void Sha1::process_block(const std::uint8_t* p) {
  std::uint32_t w[80];
  for (int i = 0; i < 16; ++i) {
    w[i] = (std::uint32_t(p[4 * i]) << 24) | (std::uint32_t(p[4 * i + 1]) << 16) |
           (std::uint32_t(p[4 * i + 2]) << 8) | std::uint32_t(p[4 * i + 3]);
  }
  for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

  std::uint32_t a = h_[0], b = h_[1], c = h_[2], d = h_[3], e = h_[4];
  for (int i = 0; i < 80; ++i) {
    std::uint32_t f, k;
    if (i < 20) {
      f = (b & c) | (~b & d);
      k = 0x5A827999u;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
    e = d;
    d = c;
    c = rol(b, 30);
    b = a;
    a = t;
  }
  h_[0] += a;
  h_[1] += b;
  h_[2] += c;
  h_[3] += d;
  h_[4] += e;
}

void Sha1::update(const void* data, std::size_t size) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  total_bytes_ += size;
  if (buffer_len_ > 0) {
    const std::size_t take = std::min(size, sizeof(buffer_) - buffer_len_);
    std::memcpy(buffer_ + buffer_len_, p, take);
    buffer_len_ += take;
    p += take;
    size -= take;
    if (buffer_len_ == sizeof(buffer_)) {
      process_block(buffer_);
      buffer_len_ = 0;
    }
  }
  while (size >= 64) {
    process_block(p);
    p += 64;
    size -= 64;
  }
  if (size > 0) {
    std::memcpy(buffer_, p, size);
    buffer_len_ = size;
  }
}

Sha1::Digest Sha1::finish() {
  const std::uint64_t bits = total_bytes_ * 8;
  const std::uint8_t pad_start = 0x80;
  update(&pad_start, 1);
  const std::uint8_t zero = 0;
  while (buffer_len_ != 56) update(&zero, 1);
  std::uint8_t len_be[8];
  for (int i = 0; i < 8; ++i) len_be[i] = static_cast<std::uint8_t>(bits >> (56 - 8 * i));
  update(len_be, 8);

  Digest out;
  for (int i = 0; i < 5; ++i) {
    out[4 * i] = static_cast<std::uint8_t>(h_[i] >> 24);
    out[4 * i + 1] = static_cast<std::uint8_t>(h_[i] >> 16);
    out[4 * i + 2] = static_cast<std::uint8_t>(h_[i] >> 8);
    out[4 * i + 3] = static_cast<std::uint8_t>(h_[i]);
  }
  reset();
  return out;
}

std::string Sha1::hex(const Digest& digest) {
  static const char* digits = "0123456789abcdef";
  std::string s;
  s.reserve(40);
  for (std::uint8_t b : digest) {
    s.push_back(digits[b >> 4]);
    s.push_back(digits[b & 15]);
  }
  return s;
}

std::string Sha1::hex_of(const void* data, std::size_t size) {
  Sha1 h;
  h.update(data, size);
  return hex(h.finish());
}

}  // namespace torecomp
