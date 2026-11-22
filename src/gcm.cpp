// SPDX-License-Identifier: GPL-3.0-or-later
#include "torecomp/gcm.hpp"

#include "disc_source.hpp"

#include <algorithm>
#include <array>
#include <cstring>

namespace torecomp::gcm {

namespace {

constexpr std::uint32_t kGcMagic = 0xC2339F3Du;
constexpr std::uint64_t kApploaderOffset = 0x2440;
constexpr std::uint64_t kMinImageSize = 0x2460;

std::uint32_t be32(const std::uint8_t* p) {
  return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) |
         std::uint32_t(p[3]);
}

std::string fixed_string(const std::uint8_t* p, std::size_t max_len) {
  std::size_t n = 0;
  while (n < max_len && p[n] != 0) ++n;
  return std::string(reinterpret_cast<const char*>(p), n);
}

bool valid_component(const std::string& name) {
  if (name.empty() || name == "." || name == ".." || name.size() > 255) return false;
  for (unsigned char c : name) {
    if (c < 0x20 || c == '/' || c == '\\') return false;
  }
  return true;
}

}  // namespace

Image::Image(const std::filesystem::path& path) : path_(path), source_(open_source(path)) {
  size_ = source_->size();
  if (size_ < kMinImageSize) throw Error(path.string() + ": too small to be a GameCube disc");

  std::array<std::uint8_t, 0x440> h;
  read(0, h.data(), h.size());
  if (be32(&h[0x1C]) != kGcMagic) {
    throw Error(path.string() + ": not a GameCube disc image (magic number missing). "
                "Supported: ISO, CISO, GCZ, WIA and RVZ. Wii discs and NKit images are not supported.");
  }
  for (int i = 0; i < 6; ++i) {
    if (h[i] < 0x20 || h[i] > 0x7E) throw Error(path.string() + ": invalid game ID");
  }
  game_id_.assign(reinterpret_cast<const char*>(&h[0]), 4);
  maker_.assign(reinterpret_cast<const char*>(&h[4]), 2);
  disc_number_ = h[6];
  version_ = h[7];
  title_ = fixed_string(&h[0x20], 64);

  const std::uint64_t dol_offset = be32(&h[0x420]);
  fst_.offset = be32(&h[0x424]);
  fst_.size = be32(&h[0x428]);

  // Apploader: 0x20 byte header whose size and trailer-size fields follow the date string.
  std::array<std::uint8_t, 0x20> ap;
  read(kApploaderOffset, ap.data(), ap.size());
  apploader_.offset = kApploaderOffset;
  apploader_.size = 0x20 + std::uint64_t(be32(&ap[0x14])) + be32(&ap[0x18]);
  if (apploader_.offset + apploader_.size > size_) throw Error(path.string() + ": apploader out of range");

  // main.dol: size is the end of the furthest section (18 section slots).
  if (dol_offset == 0 || dol_offset + 0x100 > size_) throw Error(path.string() + ": main.dol out of range");
  std::array<std::uint8_t, 0x100> dol;
  read(dol_offset, dol.data(), dol.size());
  std::uint64_t dol_end = 0x100;
  for (int i = 0; i < 18; ++i) {
    const std::uint64_t off = be32(&dol[4 * i]);
    const std::uint64_t len = be32(&dol[0x90 + 4 * i]);
    if (len != 0) dol_end = std::max(dol_end, off + len);
  }
  dol_.offset = dol_offset;
  dol_.size = dol_end;
  if (dol_.offset + dol_.size > size_) throw Error(path.string() + ": main.dol out of range");

  parse_fst();
}

Image::~Image() = default;
Image::Image(Image&&) noexcept = default;
Image& Image::operator=(Image&&) noexcept = default;

const char* Image::format() const { return source_->format(); }

void Image::read(std::uint64_t offset, void* dst, std::size_t length) {
  if (offset > size_ || length > size_ - offset) throw Error(path_.string() + ": read past end of image");
  source_->read(offset, dst, length);
}

void Image::read_chunks(std::uint64_t offset, std::uint64_t length,
                        const std::function<void(const std::uint8_t*, std::size_t)>& sink) {
  if (offset > size_ || length > size_ - offset) throw Error(path_.string() + ": read past end of image");
  static constexpr std::size_t kChunk = 1u << 20;
  std::vector<std::uint8_t> buf(static_cast<std::size_t>(std::min<std::uint64_t>(length, kChunk)));
  while (length > 0) {
    const std::size_t n = static_cast<std::size_t>(std::min<std::uint64_t>(length, buf.size()));
    read(offset, buf.data(), n);
    sink(buf.data(), n);
    offset += n;
    length -= n;
  }
}

void Image::parse_fst() {
  if (fst_.size < 12 || fst_.offset + fst_.size > size_) throw Error(path_.string() + ": FST out of range");
  std::vector<std::uint8_t> fst(static_cast<std::size_t>(fst_.size));
  read(fst_.offset, fst.data(), fst.size());

  if (fst[0] != 1) throw Error(path_.string() + ": FST root is not a directory");
  const std::uint64_t count = be32(&fst[8]);
  if (count < 1 || count * 12 > fst.size()) throw Error(path_.string() + ": FST entry count out of range");
  const std::uint8_t* names = fst.data() + count * 12;
  const std::size_t names_len = fst.size() - static_cast<std::size_t>(count * 12);

  struct Open {
    std::uint64_t end;
    std::string prefix;
  };
  std::vector<Open> stack{{count, ""}};

  for (std::uint64_t i = 1; i < count; ++i) {
    while (stack.size() > 1 && i >= stack.back().end) stack.pop_back();
    const std::uint8_t* e = &fst[i * 12];
    if (e[0] > 1) throw Error(path_.string() + ": invalid FST entry type");
    const std::uint32_t name_off = be32(e) & 0x00FFFFFFu;
    if (name_off >= names_len) throw Error(path_.string() + ": FST name offset out of range");
    const std::size_t max_len = names_len - name_off;
    const std::uint8_t* np = names + name_off;
    if (std::memchr(np, 0, max_len) == nullptr) throw Error(path_.string() + ": unterminated FST name");
    const std::string name = fixed_string(np, max_len);
    if (!valid_component(name)) throw Error(path_.string() + ": unsafe file name in FST");

    if (e[0] == 1) {
      const std::uint64_t next = be32(e + 8);
      if (next <= i || next > stack.back().end) throw Error(path_.string() + ": invalid FST directory range");
      stack.push_back({next, stack.back().prefix + name + "/"});
    } else {
      const std::uint64_t off = be32(e + 4);
      const std::uint64_t len = be32(e + 8);
      if (off > size_ || len > size_ - off) throw Error(path_.string() + ": file '" + name + "' lies outside the image");
      files_.push_back({stack.back().prefix + name, off, len});
    }
  }

  std::sort(files_.begin(), files_.end(),
            [](const FileEntry& a, const FileEntry& b) { return a.path < b.path; });
  for (std::size_t i = 1; i < files_.size(); ++i) {
    if (files_[i].path == files_[i - 1].path) throw Error(path_.string() + ": duplicate path '" + files_[i].path + "'");
  }
}

}  // namespace torecomp::gcm
