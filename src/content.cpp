// SPDX-License-Identifier: GPL-3.0-or-later
#include "torecomp/content.hpp"

#include <fstream>

#include "torecomp/hostpath.hpp"
#include "torecomp/sha1.hpp"

namespace torecomp {

namespace fs = std::filesystem;

std::string Manifest::text() const {
  std::string s;
  for (const auto& l : lines) {
    s += l.kind;
    s += '\t';
    s += std::to_string(l.size);
    s += '\t';
    s += l.sha1;
    s += '\t';
    s += l.path;
    s += '\n';
  }
  return s;
}

namespace {

struct Walker {
  gcm::Image& image;
  const Progress& progress;
  std::uint64_t done = 0;
  std::uint64_t total = 0;

  void tick(std::size_t n) {
    done += n;
    if (progress) progress(done, total);
  }

  // Hashes [offset, offset+length) and, when `dest` is set, writes it there.
  std::string hash_region(std::uint64_t offset, std::uint64_t length, const fs::path* dest) {
    Sha1 h;
    std::ofstream out;
    if (dest) {
      fs::create_directories(hostpath::long_path(dest->parent_path()));
      // Never overwrite: two disc paths that a case-insensitive host folds together end up here.
      if (fs::exists(hostpath::long_path(*dest))) {
        throw Error("refusing to overwrite " + dest->string() + " (two disc paths map to the same host file)");
      }
      out.open(hostpath::long_path(*dest), std::ios::binary | std::ios::trunc);
      if (!out) throw Error("cannot create " + dest->string());
    }
    image.read_chunks(offset, length, [&](const std::uint8_t* p, std::size_t n) {
      h.update(p, n);
      if (dest) out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
      tick(n);
    });
    if (dest) {
      out.close();
      if (!out) throw Error("write error on " + dest->string());
    }
    return Sha1::hex(h.finish());
  }

  void write_raw(const gcm::Region& r, const fs::path& dest) {
    fs::create_directories(hostpath::long_path(dest.parent_path()));
    std::ofstream out(hostpath::long_path(dest), std::ios::binary | std::ios::trunc);
    if (!out) throw Error("cannot create " + dest.string());
    image.read_chunks(r.offset, r.size, [&](const std::uint8_t* p, std::size_t n) {
      out.write(reinterpret_cast<const char*>(p), static_cast<std::streamsize>(n));
    });
    out.close();
    if (!out) throw Error("write error on " + dest.string());
  }
};

// Safety net on top of the name validation done while parsing the FST.
fs::path checked_destination(const fs::path& root, const std::string& rel) {
  const fs::path dest = (root / "files" / hostpath::to_fs(rel)).lexically_normal();
  const fs::path base = (root / "files").lexically_normal();
  auto b = base.begin();
  auto d = dest.begin();
  for (; b != base.end(); ++b, ++d) {
    if (d == dest.end() || *d != *b) throw Error("refusing to write outside the output folder: " + rel);
  }
  return dest;
}

Manifest walk(gcm::Image& image, const fs::path* out_dir, const Progress& progress) {
  Walker w{image, progress};
  w.total = 0x420 + image.bi2_bin().size + image.apploader().size + image.main_dol().size;
  for (const auto& f : image.files()) w.total += f.size;

  Manifest m;
  auto add = [&](const char* kind, std::uint64_t size, std::string sha1, std::string path = {}) {
    m.lines.push_back({kind, size, std::move(sha1), std::move(path), {}});
  };

  add("header", 0x420, w.hash_region(0, 0x420, nullptr));
  add("bi2", image.bi2_bin().size, w.hash_region(image.bi2_bin().offset, image.bi2_bin().size, nullptr));
  add("apploader", image.apploader().size, w.hash_region(image.apploader().offset, image.apploader().size, nullptr));
  add("dol", image.main_dol().size, w.hash_region(image.main_dol().offset, image.main_dol().size, nullptr));

  if (out_dir) {
    w.write_raw(image.boot_bin(), *out_dir / "sys" / "boot.bin");
    w.write_raw(image.bi2_bin(), *out_dir / "sys" / "bi2.bin");
    w.write_raw(image.apploader(), *out_dir / "sys" / "apploader.img");
    w.write_raw(image.fst(), *out_dir / "sys" / "fst.bin");
    w.write_raw(image.main_dol(), *out_dir / "sys" / "main.dol");
  }

  std::vector<std::string> host;
  if (out_dir) {
    std::vector<std::string> disc;
    for (const auto& f : image.files()) disc.push_back(f.path);
    host = hostpath::plan(disc);
  }
  std::string path_map;
  std::size_t index = 0;
  for (const auto& f : image.files()) {
    if (out_dir) {
      const std::string& h = host[index++];
      const fs::path dest = checked_destination(*out_dir, h);
      add("file", f.size, w.hash_region(f.offset, f.size, &dest), f.path);
      m.lines.back().host_path = h;
      path_map += f.path + '\t' + h + '\n';
    } else {
      add("file", f.size, w.hash_region(f.offset, f.size, nullptr), f.path);
    }
  }

  if (out_dir) {
    const fs::path file = *out_dir / hostpath::kPathMapFile;
    std::ofstream out(hostpath::long_path(file), std::ios::binary | std::ios::trunc);
    out.write(path_map.data(), static_cast<std::streamsize>(path_map.size()));
    out.close();
    if (!out) throw Error("cannot write " + file.string());
  }

  m.content_sha1 = Sha1::hex_of(m.text().data(), m.text().size());
  return m;
}

}  // namespace

Manifest build_manifest(gcm::Image& image, const Progress& progress) { return walk(image, nullptr, progress); }

Manifest extract(gcm::Image& image, const fs::path& out_dir, const std::string& expected_content_sha1,
                 const Progress& progress) {
  const fs::path long_out = hostpath::long_path(out_dir);
  const bool existed = fs::exists(long_out);
  if (existed && (!fs::is_directory(long_out) || !fs::is_empty(long_out))) {
    throw Error(out_dir.string() + " must not exist or must be an empty folder");
  }
  auto cleanup = [&] {
    std::error_code ec;
    if (existed) {
      for (const auto& e : fs::directory_iterator(long_out, ec)) fs::remove_all(e.path(), ec);
    } else {
      fs::remove_all(long_out, ec);
    }
  };

  try {
    fs::create_directories(long_out);
    Manifest m = walk(image, &out_dir, progress);
    if (!expected_content_sha1.empty() && m.content_sha1 != expected_content_sha1) {
      throw Error("extracted content hash " + m.content_sha1 + " does not match the expected " +
                  expected_content_sha1);
    }
    return m;
  } catch (const fs::filesystem_error& e) {
    cleanup();
    throw Error(std::string("extraction failed: ") + e.what());
  } catch (...) {
    cleanup();
    throw;
  }
}

std::string hash_whole_image(gcm::Image& image, const Progress& progress) {
  Sha1 h;
  std::uint64_t done = 0;
  image.read_chunks(0, image.size(), [&](const std::uint8_t* p, std::size_t n) {
    h.update(p, n);
    done += n;
    if (progress) progress(done, image.size());
  });
  return Sha1::hex(h.finish());
}

}  // namespace torecomp
