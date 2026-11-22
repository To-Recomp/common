// SPDX-License-Identifier: GPL-3.0-or-later
// Plain ISO, CISO and GCZ containers. Layouts are public (CISO: libwbfs-style map; GCZ: header,
// block pointer table, Adler-32 table, deflate blocks).
#include "disc_source.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include "codecs.hpp"
#include "torecomp/gcm.hpp"

namespace torecomp::gcm {

namespace {

std::uint32_t le32(const std::uint8_t* p) {
  return std::uint32_t(p[0]) | (std::uint32_t(p[1]) << 8) | (std::uint32_t(p[2]) << 16) | (std::uint32_t(p[3]) << 24);
}
std::uint64_t le64(const std::uint8_t* p) { return le32(p) | (std::uint64_t(le32(p + 4)) << 32); }

// Shared file access with bounds-checked reads.
class FileBase : public DiscSource {
public:
  explicit FileBase(const std::filesystem::path& path) : path_(path), in_(path, std::ios::binary) {
    if (!in_) throw Error("cannot open " + path.string());
    in_.seekg(0, std::ios::end);
    const std::streamoff end = in_.tellg();
    if (end < 0) throw Error("cannot determine the size of " + path.string());
    file_size_ = static_cast<std::uint64_t>(end);
  }

protected:
  [[noreturn]] void fail(const std::string& what) const { throw Error(path_.string() + ": " + what); }

  void file_read(std::uint64_t offset, void* dst, std::size_t length) {
    if (offset > file_size_ || length > file_size_ - offset) fail("truncated file");
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(length));
    if (!in_) fail("read error");
  }

  std::filesystem::path path_;
  std::ifstream in_;
  std::uint64_t file_size_ = 0;
};

class PlainSource final : public FileBase {
public:
  using FileBase::FileBase;
  std::uint64_t size() const override { return file_size_; }
  const char* format() const override { return "ISO"; }
  void read(std::uint64_t offset, void* dst, std::size_t length) override { file_read(offset, dst, length); }
};

// CISO: 0x8000 byte header = "CISO", u32 little-endian block size, then one byte per block
// (1 = stored). Stored blocks follow the header back to back. Missing blocks read as zeros.
class CisoSource final : public FileBase {
public:
  explicit CisoSource(const std::filesystem::path& path) : FileBase(path) {
    std::array<std::uint8_t, kHeader> h;
    file_read(0, h.data(), h.size());
    block_size_ = le32(&h[4]);
    if (block_size_ < 0x8000 || block_size_ > (1u << 30) || (block_size_ & (block_size_ - 1)) != 0) {
      fail("invalid CISO block size");
    }
    std::uint64_t stored = 0;
    std::uint64_t blocks = 0;
    for (std::size_t i = 0; i < kMapSize; ++i) {
      if (h[8 + i] > 1) fail("invalid CISO block map");
      if (h[8 + i] == 1) {
        blocks = i + 1;
        ++stored;
      }
    }
    if (stored == 0) fail("empty CISO image");
    map_.assign(blocks, -1);
    std::int64_t next = 0;
    for (std::uint64_t i = 0; i < blocks; ++i) {
      if (h[8 + i] == 1) map_[i] = next++;
    }
    // The format does not record the disc size; it ends with the last stored block. A final
    // stored block may be cut short by the end of the file.
    const std::uint64_t last_pos = kHeader + std::uint64_t(next - 1) * block_size_;
    if (file_size_ <= last_pos) fail("truncated file");
    last_len_ = static_cast<std::uint32_t>(std::min<std::uint64_t>(block_size_, file_size_ - last_pos));
    size_ = (blocks - 1) * block_size_ + last_len_;
  }

  std::uint64_t size() const override { return size_; }
  const char* format() const override { return "CISO"; }

  void read(std::uint64_t offset, void* dst, std::size_t length) override {
    auto* out = static_cast<std::uint8_t*>(dst);
    while (length > 0) {
      const std::uint64_t block = offset / block_size_;
      const std::uint64_t in_block = offset % block_size_;
      const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(length, block_size_ - in_block));
      if (block >= map_.size() || map_[block] < 0) {
        std::memset(out, 0, n);
      } else {
        file_read(kHeader + std::uint64_t(map_[block]) * block_size_ + in_block, out, n);
      }
      out += n;
      offset += n;
      length -= n;
    }
  }

private:
  static constexpr std::size_t kHeader = 0x8000;
  static constexpr std::size_t kMapSize = kHeader - 8;
  std::uint32_t block_size_ = 0;
  std::uint32_t last_len_ = 0;
  std::uint64_t size_ = 0;
  std::vector<std::int64_t> map_;  // stored-block index per block, -1 when absent
};

// GCZ: 0x20 byte little-endian header, u64 block pointers (top bit = stored uncompressed), u32
// Adler-32 of every stored block, then the blocks (zlib streams).
class GczSource final : public FileBase {
public:
  explicit GczSource(const std::filesystem::path& path) : FileBase(path) {
    std::array<std::uint8_t, 0x20> h;
    file_read(0, h.data(), h.size());
    const std::uint64_t compressed_total = le64(&h[8]);
    size_ = le64(&h[16]);
    block_size_ = le32(&h[24]);
    const std::uint64_t num_blocks = le32(&h[28]);
    if (block_size_ == 0 || block_size_ > (64u << 20)) fail("invalid GCZ block size");
    if (num_blocks == 0 || num_blocks != (size_ + block_size_ - 1) / block_size_) fail("inconsistent GCZ header");
    const std::uint64_t table = num_blocks * 12;
    data_start_ = 0x20 + table;
    if (table > file_size_ || data_start_ > file_size_ || compressed_total > file_size_ - data_start_) {
      fail("truncated file");
    }
    std::vector<std::uint8_t> raw(static_cast<std::size_t>(table));
    file_read(0x20, raw.data(), raw.size());
    pointers_.resize(num_blocks);
    hashes_.resize(num_blocks);
    for (std::uint64_t i = 0; i < num_blocks; ++i) pointers_[i] = le64(&raw[i * 8]);
    for (std::uint64_t i = 0; i < num_blocks; ++i) hashes_[i] = le32(&raw[num_blocks * 8 + i * 4]);
    compressed_total_ = compressed_total;
    for (std::uint64_t i = 0; i < num_blocks; ++i) {
      const std::uint64_t begin = pointers_[i] & ~kStored;
      const std::uint64_t end = i + 1 < num_blocks ? (pointers_[i + 1] & ~kStored) : compressed_total_;
      if (end < begin || end > compressed_total_) fail("invalid GCZ block table");
    }
  }

  std::uint64_t size() const override { return size_; }
  const char* format() const override { return "GCZ"; }

  void read(std::uint64_t offset, void* dst, std::size_t length) override {
    auto* out = static_cast<std::uint8_t*>(dst);
    while (length > 0) {
      const std::uint64_t block = offset / block_size_;
      const std::size_t in_block = static_cast<std::size_t>(offset % block_size_);
      const std::size_t n = std::min<std::size_t>(length, block_size_ - in_block);
      load(block);
      std::memcpy(out, cache_.data() + in_block, n);
      out += n;
      offset += n;
      length -= n;
    }
  }

private:
  static constexpr std::uint64_t kStored = std::uint64_t(1) << 63;

  void load(std::uint64_t block) {
    if (block == cached_) return;
    const std::uint64_t begin = pointers_[block] & ~kStored;
    const std::uint64_t end = block + 1 < pointers_.size() ? (pointers_[block + 1] & ~kStored) : compressed_total_;
    const std::size_t stored_len = static_cast<std::size_t>(end - begin);
    const std::size_t plain_len = static_cast<std::size_t>(
        std::min<std::uint64_t>(block_size_, size_ - block * block_size_));
    raw_.resize(stored_len);
    file_read(data_start_ + begin, raw_.data(), stored_len);
    if (codec::adler32(raw_.data(), stored_len) != hashes_[block]) fail("GCZ block checksum mismatch");
    cache_.assign(block_size_, 0);
    cached_ = std::numeric_limits<std::uint64_t>::max();
    if (pointers_[block] & kStored) {
      if (stored_len != plain_len) fail("invalid GCZ stored block");
      std::memcpy(cache_.data(), raw_.data(), plain_len);
    } else {
      try {
        codec::zlib_inflate(raw_.data(), stored_len, cache_.data(), plain_len);
      } catch (const Error& e) {
        fail(e.what());
      }
    }
    cached_ = block;
  }

  std::uint64_t size_ = 0, data_start_ = 0, compressed_total_ = 0;
  std::uint32_t block_size_ = 0;
  std::vector<std::uint64_t> pointers_;
  std::vector<std::uint32_t> hashes_;
  std::vector<std::uint8_t> raw_, cache_;
  std::uint64_t cached_ = std::numeric_limits<std::uint64_t>::max();
};

}  // namespace

std::unique_ptr<DiscSource> open_source(const std::filesystem::path& path) {
  std::array<std::uint8_t, 4> magic{};
  {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw Error("cannot open " + path.string());
    in.read(reinterpret_cast<char*>(magic.data()), 4);
    if (in.gcount() != 4) magic.fill(0);
  }
  if (std::memcmp(magic.data(), "CISO", 4) == 0) return std::make_unique<CisoSource>(path);
  if (le32(magic.data()) == 0xB10BC001u) return std::make_unique<GczSource>(path);
  if (std::memcmp(magic.data(), "WIA\x01", 4) == 0) return open_wia_rvz(path, false);
  if (std::memcmp(magic.data(), "RVZ\x01", 4) == 0) return open_wia_rvz(path, true);
  return std::make_unique<PlainSource>(path);
}

}  // namespace torecomp::gcm
