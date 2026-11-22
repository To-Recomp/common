// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace torecomp::hostpath {

// Disc paths are '/'-separated UTF-8 names chosen by whoever mastered the disc. Host paths are what
// extraction writes. The mapping is the same on every OS (an extracted tree is portable) and is
// reproducible: the same disc always yields the same host paths.
//
// Rules, applied per path component:
//   - `< > : " | ? * \ /` and control characters become `%XX` (upper-case hex of the byte);
//   - a Windows device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with any extension and any trailing
//     spaces before the first dot) has its first character replaced by `%XX`;
//   - a trailing '.' or ' ' is replaced by `%2E` / `%20`;
//   - a literal '%' followed by two hex digits has its '%' written as `%25`, so the escapes are
//     unambiguous;
//   - a component that is empty, "." or ".." (which could leave the output folder) is rejected, as is
//     a component longer than 255 bytes after mapping;
//   - two entries whose host names are equal ignoring ASCII case (or that are a file and a folder
//     with the same name) cannot share a host name: the one that sorts later gets `~N` inserted before
//     its extension (`~1`, `~2`, ...). Case-insensitive file systems that fold more than ASCII case
//     are caught at write time: extraction refuses to overwrite a file.
// Throws torecomp::Error for an unmappable name.
std::string sanitize_component(const std::string& name);

// Host path ('/'-separated, relative to the extraction folder `files/`) of each disc path.
// `disc_paths` must be sorted and free of duplicates; the result has the same order.
std::vector<std::string> plan(const std::vector<std::string>& disc_paths);

// ASCII lower-casing, the comparison a case-insensitive host file system applies at least.
std::string fold(const std::string& s);

// Relative UTF-8 path to a std::filesystem::path without going through the Windows ANSI code page.
std::filesystem::path to_fs(const std::string& utf8_rel);

// On Windows: absolute extended-length form (`\\?\...`) so paths over MAX_PATH (260) work. Elsewhere
// the path is returned unchanged.
std::filesystem::path long_path(const std::filesystem::path& p);

// Name of the file in the extraction folder that records disc path -> host path, one
// "<disc path>\t<host path>\n" line per file, host path relative to `files/`.
inline constexpr const char* kPathMapFile = "paths.tsv";

class PathMap {
public:
  // Reads `<extraction folder>/paths.tsv`; throws torecomp::Error if it is missing or malformed.
  static PathMap load(const std::filesystem::path& extraction_dir);

  // Host path relative to `files/` for a disc path: exact match first, then ignoring ASCII case (a
  // game asking for "Data/A.bin" finds "data/a.bin"). Empty string when there is no such file.
  std::string find(const std::string& disc_path) const;
  // Same, as a full path under `<extraction folder>/files/`; empty path when not found.
  std::filesystem::path find_path(const std::string& disc_path) const;

private:
  std::filesystem::path dir_;
  std::unordered_map<std::string, std::string> exact_;
  std::unordered_map<std::string, std::string> folded_;
};

}  // namespace torecomp::hostpath
