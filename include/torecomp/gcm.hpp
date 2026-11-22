// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace torecomp {

class Error : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

// Called with (bytes done, bytes total) while a disc is read.
using Progress = std::function<void(std::uint64_t, std::uint64_t)>;

namespace gcm {

struct FileEntry {
  std::string path;  // '/'-separated, relative to the disc root, validated
  std::uint64_t offset;
  std::uint64_t size;
};

struct Region {
  std::uint64_t offset = 0;
  std::uint64_t size = 0;
};

// A plain (uncompressed) GameCube disc image. Everything read from the file is bounds-checked;
// a malformed image throws torecomp::Error from the constructor.
class Image {
public:
  explicit Image(const std::filesystem::path& path);

  const std::filesystem::path& path() const { return path_; }
  std::uint64_t size() const { return size_; }

  const std::string& game_id() const { return game_id_; }  // 4 characters, e.g. "GQSF"
  const std::string& maker() const { return maker_; }      // 2 characters, e.g. "AF"
  std::uint8_t disc_number() const { return disc_number_; }  // 0-based
  std::uint8_t version() const { return version_; }
  const std::string& title() const { return title_; }

  // Disc header: boot.bin (0x440 bytes) then bi2.bin (0x2000 bytes).
  Region boot_bin() const { return {0, 0x440}; }
  Region bi2_bin() const { return {0x440, 0x2000}; }
  Region apploader() const { return apploader_; }
  Region main_dol() const { return dol_; }
  Region fst() const { return fst_; }

  // Files sorted by path; no duplicates.
  const std::vector<FileEntry>& files() const { return files_; }

  void read(std::uint64_t offset, void* dst, std::size_t length);
  // Streams [offset, offset+length) in chunks of at most 1 MiB.
  void read_chunks(std::uint64_t offset, std::uint64_t length,
                   const std::function<void(const std::uint8_t*, std::size_t)>& sink);

private:
  void parse_fst();

  std::filesystem::path path_;
  std::ifstream in_;
  std::uint64_t size_ = 0;
  std::string game_id_, maker_, title_;
  std::uint8_t disc_number_ = 0, version_ = 0;
  Region apploader_, dol_, fst_;
  std::vector<FileEntry> files_;
};

}  // namespace gcm
}  // namespace torecomp
