// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "torecomp/gcm.hpp"

namespace torecomp {

struct ManifestLine {
  std::string kind;  // "header", "bi2", "apploader", "dol" or "file"
  std::uint64_t size = 0;
  std::string sha1;
  std::string path;  // empty except for "file"
  // Where extract() wrote a "file" entry, relative to `files/` in the output folder ('/'-separated).
  // Differs from `path` when the disc name is unusable on some host (see hostpath.hpp). Not part of
  // text() and so not of the content hash; empty for build_manifest().
  std::string host_path;
};

// Hashes of what a disc contains, independent of where it is laid out and of what fills the unused
// space between files. Two images with the same content hash hold identical game data even when
// one was scrubbed (padding zeroed) or rebuilt. The whole-image SHA-1 used by Redump does not have
// that property.
//
// Covered: disc header bytes [0, 0x420) (game ID, title, magic), bi2.bin, the apploader, main.dol and
// every file (path, size, SHA-1), in path order. Not covered: the DOL/FST offsets stored in
// boot.bin, the FST bytes themselves, and the padding.
struct Manifest {
  std::vector<ManifestLine> lines;
  // SHA-1 of text(): the value stored as `content_sha1` in dumps.toml.
  std::string content_sha1;

  // One line per entry: "<kind>\t<size>\t<sha1>\t<path>\n".
  std::string text() const;
};

Manifest build_manifest(gcm::Image& image, const Progress& progress = {});

// Extracts the disc into `out_dir` (must not exist or be empty):
//   sys/boot.bin, sys/bi2.bin, sys/apploader.img, sys/fst.bin, sys/main.dol, files/<host path> and
//   paths.tsv (disc path -> host path, read back with hostpath::PathMap).
// Disc names that are unsafe or unwritable on Windows/macOS are mapped (hostpath.hpp), the same way
// on every OS; nothing is ever written outside `out_dir`.
// Hashes everything while copying. If `expected_content_sha1` is non-empty and the hash of what was
// read differs, everything written is removed and Error is thrown.
Manifest extract(gcm::Image& image, const std::filesystem::path& out_dir,
                 const std::string& expected_content_sha1 = {}, const Progress& progress = {});

// SHA-1 of the whole image file (what Redump lists).
std::string hash_whole_image(gcm::Image& image, const Progress& progress = {});

}  // namespace torecomp
