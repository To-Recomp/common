// SPDX-License-Identifier: GPL-3.0-or-later
// WIA and RVZ containers (GameCube discs). Implemented from the public format description
// (Dolphin's docs/WiaAndRvz.md); no code is taken from Dolphin or wit.
//
// Layout: file head (0x48) + disc struct (0xDC) + raw-data table + group table + chunk data.
// All integers are big endian. For a GameCube disc every byte is "raw data": the raw-data
// entries map disc ranges onto runs of groups, each group holding `chunk_size` bytes compressed
// on its own. RVZ groups may additionally use "RVZ packing", which stores lagged-Fibonacci
// padding as a 68 byte seed instead of the bytes themselves.
//
// Wii images are not supported: partition entries need re-encryption and hash reconstruction.
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <vector>

#include "codecs.hpp"
#include "disc_source.hpp"
#include "torecomp/gcm.hpp"
#include "torecomp/sha1.hpp"

namespace torecomp::gcm {

namespace {

enum Compression : std::uint32_t { kNone = 0, kPurge = 1, kBzip2 = 2, kLzma = 3, kLzma2 = 4, kZstd = 5 };

constexpr std::uint64_t kSector = 0x8000;
constexpr std::uint32_t kMaxChunk = 256u << 20;
constexpr std::size_t kHeader1 = 0x48;
constexpr std::size_t kHeader2 = 0xDC;
constexpr std::uint32_t kSupportedVersion = 0x01000000;

std::uint32_t be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | std::uint32_t(p[3]);
}
std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t(be32(p)) << 32) | be32(p + 4); }

// Lagged Fibonacci generator (j = 32, k = 521, xor) that produces GameCube/Wii padding.
class Lfg {
public:
  explicit Lfg(const std::uint8_t* seed_be) {
    for (int i = 0; i < 17; ++i) buf_[i] = be32(seed_be + 4 * i);
    for (int i = 17; i < 521; ++i) buf_[i] = (buf_[i - 17] << 23) ^ (buf_[i - 16] >> 9) ^ buf_[i - 1];
    for (int i = 0; i < 4; ++i) step();
  }

  void skip(std::uint64_t bytes) {
    while (bytes-- > 0) next();
  }

  std::uint8_t next() {
    const std::uint32_t w = buf_[word_];
    std::uint8_t b;
    switch (byte_) {
      case 0: b = static_cast<std::uint8_t>(w >> 24); break;
      case 1: b = static_cast<std::uint8_t>(w >> 18); break;  // 18, not 16
      case 2: b = static_cast<std::uint8_t>(w >> 8); break;
      default: b = static_cast<std::uint8_t>(w); break;
    }
    if (++byte_ == 4) {
      byte_ = 0;
      if (++word_ == 521) {
        word_ = 0;
        step();
      }
    }
    return b;
  }

private:
  void step() {
    for (int i = 0; i < 32; ++i) buf_[i] ^= buf_[i + 521 - 32];
    for (int i = 32; i < 521; ++i) buf_[i] ^= buf_[i - 32];
  }

  std::uint32_t buf_[521];
  int word_ = 0, byte_ = 0;
};

bool sha1_equals(const std::uint8_t* data, std::size_t size, const std::uint8_t* expected) {
  Sha1 s;
  s.update(data, size);
  const Sha1::Digest d = s.finish();
  return std::memcmp(d.data(), expected, d.size()) == 0;
}

// PURGE: (offset, size, data) segments in ascending order, then a SHA-1 of everything before it.
void purge_decode(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out, std::size_t out_size) {
  if (in_size < 20) throw Error("truncated PURGE data");
  const std::size_t body = in_size - 20;
  if (!sha1_equals(in, body, in + body)) {
    throw Error("PURGE checksum mismatch");
  }
  std::memset(out, 0, out_size);
  std::size_t pos = 0;
  std::uint64_t prev_end = 0;
  while (pos < body) {
    if (body - pos < 8) throw Error("corrupt PURGE data");
    const std::uint64_t off = be32(in + pos), len = be32(in + pos + 4);
    pos += 8;
    if (len > body - pos || off < prev_end || off > out_size || len > out_size - off) {
      throw Error("corrupt PURGE data");
    }
    std::memcpy(out + off, in + pos, static_cast<std::size_t>(len));
    pos += static_cast<std::size_t>(len);
    prev_end = off + len;
  }
}

// RVZ packing: u32 size; top bit set = 68 byte LFG seed follows and `size` bytes are generated,
// otherwise `size` literal bytes follow. `disc_offset` is where out[0] lies on the disc.
void rvz_unpack(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out, std::size_t out_size,
                std::uint64_t disc_offset) {
  std::size_t pos = 0, done = 0;
  while (pos < in_size) {
    if (in_size - pos < 4) throw Error("corrupt RVZ packing");
    std::uint32_t n = be32(in + pos);
    pos += 4;
    const bool junk = (n & 0x80000000u) != 0;
    n &= 0x7FFFFFFFu;
    if (n > out_size - done) throw Error("corrupt RVZ packing");
    if (junk) {
      if (in_size - pos < 68) throw Error("corrupt RVZ packing");
      Lfg lfg(in + pos);
      pos += 68;
      lfg.skip((disc_offset + done) % kSector);
      for (std::uint32_t i = 0; i < n; ++i) out[done + i] = lfg.next();
    } else {
      if (n > in_size - pos) throw Error("corrupt RVZ packing");
      std::memcpy(out + done, in + pos, n);
      pos += n;
    }
    done += n;
  }
  if (done != out_size) throw Error("corrupt RVZ packing: size mismatch");
}

class WiaSource final : public DiscSource {
public:
  WiaSource(const std::filesystem::path& path, bool rvz) : path_(path), rvz_(rvz), in_(path, std::ios::binary) {
    if (!in_) throw Error("cannot open " + path.string());
    in_.seekg(0, std::ios::end);
    file_size_ = static_cast<std::uint64_t>(in_.tellg());
    try {
      parse();
    } catch (const Error& e) {
      throw Error(path.string() + ": " + e.what());
    }
  }

  std::uint64_t size() const override { return iso_size_; }
  const char* format() const override { return rvz_ ? "RVZ" : "WIA"; }

  void read(std::uint64_t offset, void* dst, std::size_t length) override {
    try {
      read_impl(offset, static_cast<std::uint8_t*>(dst), length);
    } catch (const Error& e) {
      throw Error(path_.string() + ": " + e.what());
    }
  }

private:
  struct Raw {
    std::uint64_t offset, size;  // disc range, offset rounded down to a sector
    std::uint32_t group_index, groups;
  };
  struct Group {
    std::uint32_t off4, size, packed;
  };

  void file_read(std::uint64_t offset, void* dst, std::size_t length) {
    if (offset > file_size_ || length > file_size_ - offset) throw Error("truncated file");
    in_.clear();
    in_.seekg(static_cast<std::streamoff>(offset));
    in_.read(static_cast<char*>(dst), static_cast<std::streamsize>(length));
    if (!in_) throw Error("read error");
  }

  void decompress(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out, std::size_t out_size) {
    switch (compression_) {
      case kPurge: purge_decode(in, in_size, out, out_size); break;
      case kBzip2: codec::bzip2_decompress(in, in_size, out, out_size); break;
      case kLzma: codec::lzma_decompress(props_, props_len_, false, in, in_size, out, out_size); break;
      case kLzma2: codec::lzma_decompress(props_, props_len_, true, in, in_size, out, out_size); break;
      case kZstd: codec::zstd_decompress(in, in_size, out, out_size); break;
      default:
        if (in_size != out_size) throw Error("size mismatch in uncompressed data");
        std::memcpy(out, in, out_size);
    }
  }

  std::vector<std::uint8_t> read_table(std::uint64_t offset, std::uint64_t stored, std::uint64_t plain) {
    if (plain > (64u << 20) || stored > file_size_) throw Error("table too large");
    std::vector<std::uint8_t> in(static_cast<std::size_t>(stored)), out(static_cast<std::size_t>(plain));
    file_read(offset, in.data(), in.size());
    decompress(in.data(), in.size(), out.data(), out.size());
    return out;
  }

  void parse() {
    std::array<std::uint8_t, kHeader1 + kHeader2> h{};
    if (file_size_ < kHeader1) throw Error("truncated file");
    file_read(0, h.data(), kHeader1);
    const std::uint32_t version_compatible = be32(&h[8]);
    const std::uint32_t h2_size = be32(&h[12]);
    iso_size_ = be64(&h[0x24]);
    if (!sha1_equals(h.data(), 0x34, &h[0x34])) {
      throw Error("corrupt header (checksum mismatch)");
    }
    if (version_compatible > kSupportedVersion) throw Error("unsupported format version");
    if (be64(&h[0x2C]) != file_size_) throw Error("file size differs from the size recorded in the header (truncated?)");
    if (h2_size < 0xD5 || h2_size > kHeader2) throw Error("invalid header");
    if (file_size_ < kHeader1 + h2_size) throw Error("truncated file");
    file_read(kHeader1, &h[kHeader1], h2_size);
    if (!sha1_equals(&h[kHeader1], h2_size, &h[0x10])) {
      throw Error("corrupt header (checksum mismatch)");
    }
    const std::uint8_t* d = &h[kHeader1];

    const std::uint32_t disc_type = be32(d);
    if (disc_type == 2 || be32(d + 0x10 + 0x18) == 0x5D1C9EA3u) {
      throw Error("Wii discs are not supported (GameCube only)");
    }
    compression_ = be32(d + 4);
    chunk_ = be32(d + 12);
    if (compression_ > (rvz_ ? kZstd : kLzma2) || (rvz_ && compression_ == kPurge)) {
      throw Error("unsupported compression method " + std::to_string(compression_));
    }
    if (chunk_ < kSector || chunk_ > kMaxChunk || chunk_ % kSector != 0 ||
        (chunk_ < 0x200000 && (chunk_ & (chunk_ - 1)) != 0) || (chunk_ > 0x200000 && chunk_ % 0x200000 != 0)) {
      throw Error("invalid chunk size");
    }
    std::memcpy(dhead_.data(), d + 0x10, dhead_.size());
    if (be32(d + 0x90) != 0) throw Error("images with partition entries (Wii) are not supported");
    props_len_ = d[0xD4];
    if (props_len_ > sizeof props_) throw Error("invalid compressor data");
    std::memcpy(props_, d + 0xD5, props_len_);

    const std::uint32_t n_raw = be32(d + 0xB4);
    const std::uint64_t raw_off = be64(d + 0xB8);
    const std::uint32_t raw_size = be32(d + 0xC0);
    const std::uint32_t n_groups = be32(d + 0xC4);
    const std::uint64_t group_off = be64(d + 0xC8);
    const std::uint32_t group_size = be32(d + 0xD0);
    const std::size_t group_entry = rvz_ ? 12 : 8;

    const auto rawtab = read_table(raw_off, raw_size, std::uint64_t(n_raw) * 0x18);
    for (std::uint32_t i = 0; i < n_raw; ++i) {
      const std::uint8_t* e = &rawtab[std::size_t(i) * 0x18];
      Raw r{be64(e), be64(e + 8), be32(e + 16), be32(e + 20)};
      const std::uint64_t aligned = r.offset & ~(kSector - 1);
      if (r.size > std::numeric_limits<std::uint64_t>::max() - r.offset) throw Error("invalid raw data entry");
      r.size += r.offset - aligned;
      r.offset = aligned;
      if (r.offset > iso_size_ || r.size > iso_size_ - r.offset) throw Error("raw data entry outside the disc");
      const std::uint64_t needed = (r.size + chunk_ - 1) / chunk_;
      if (needed > r.groups || r.group_index > n_groups || r.groups > n_groups - r.group_index) {
        throw Error("raw data entry has too few groups");
      }
      raw_.push_back(r);
    }
    const auto grptab = read_table(group_off, group_size, std::uint64_t(n_groups) * group_entry);
    groups_.reserve(n_groups);
    for (std::uint32_t i = 0; i < n_groups; ++i) {
      const std::uint8_t* e = &grptab[std::size_t(i) * group_entry];
      groups_.push_back({be32(e), be32(e + 4), rvz_ ? be32(e + 8) : 0});
    }
  }

  const std::vector<std::uint8_t>& group_data(const Raw& r, std::uint64_t g) {
    const std::uint64_t index = r.group_index + g;
    if (index == cached_) return cache_;
    cached_ = std::numeric_limits<std::uint64_t>::max();
    const std::uint64_t start = g * chunk_;
    const std::size_t plain = static_cast<std::size_t>(std::min<std::uint64_t>(chunk_, r.size - start));
    const Group& grp = groups_[static_cast<std::size_t>(index)];
    std::uint32_t stored = grp.size;
    bool compressed = compression_ != kNone;
    if (rvz_) {
      compressed = compressed && (stored & 0x80000000u) != 0;
      stored &= 0x7FFFFFFFu;
    }
    cache_.assign(plain, 0);
    if (stored != 0) {
      const std::uint64_t file_off = std::uint64_t(grp.off4) * 4;
      if (file_off > file_size_ || stored > file_size_ - file_off) throw Error("group lies outside the file");
      std::vector<std::uint8_t> in(stored);
      file_read(file_off, in.data(), in.size());
      const std::size_t unpacked = grp.packed != 0 ? grp.packed : plain;
      if (grp.packed != 0 && grp.packed > kMaxChunk) throw Error("invalid RVZ packed size");
      std::vector<std::uint8_t> mid;
      std::uint8_t* target = cache_.data();
      if (grp.packed != 0) {
        mid.resize(unpacked);
        target = mid.data();
      }
      if (compressed) {
        decompress(in.data(), in.size(), target, unpacked);
      } else {
        if (in.size() != unpacked) throw Error("size mismatch in uncompressed group");
        std::memcpy(target, in.data(), unpacked);
      }
      if (grp.packed != 0) rvz_unpack(mid.data(), mid.size(), cache_.data(), plain, r.offset + start);
    }
    cached_ = index;
    return cache_;
  }

  void read_impl(std::uint64_t offset, std::uint8_t* out, std::size_t length) {
    while (length > 0) {
      std::size_t n = 0;
      if (offset < dhead_.size()) {
        n = std::min<std::size_t>(length, dhead_.size() - static_cast<std::size_t>(offset));
        std::memcpy(out, dhead_.data() + offset, n);
      } else {
        const Raw* r = nullptr;
        for (const Raw& c : raw_) {
          if (offset >= c.offset && offset - c.offset < c.size) {
            r = &c;
            break;
          }
        }
        if (!r) throw Error("disc offset not covered by the image");
        const std::uint64_t rel = offset - r->offset;
        const std::uint64_t g = rel / chunk_;
        const auto& data = group_data(*r, g);
        const std::size_t in_group = static_cast<std::size_t>(rel - g * chunk_);
        n = std::min<std::size_t>(length, data.size() - in_group);
        std::memcpy(out, data.data() + in_group, n);
      }
      out += n;
      offset += n;
      length -= n;
    }
  }

  std::filesystem::path path_;
  bool rvz_;
  std::ifstream in_;
  std::uint64_t file_size_ = 0, iso_size_ = 0;
  std::uint32_t compression_ = 0, chunk_ = 0;
  std::uint8_t props_[7] = {};
  std::size_t props_len_ = 0;
  std::array<std::uint8_t, 0x80> dhead_{};
  std::vector<Raw> raw_;
  std::vector<Group> groups_;
  std::vector<std::uint8_t> cache_;
  std::uint64_t cached_ = std::numeric_limits<std::uint64_t>::max();
};

}  // namespace

std::unique_ptr<DiscSource> open_wia_rvz(const std::filesystem::path& path, bool rvz) {
  return std::make_unique<WiaSource>(path, rvz);
}

}  // namespace torecomp::gcm
