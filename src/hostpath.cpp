// SPDX-License-Identifier: GPL-3.0-or-later
#include "torecomp/hostpath.hpp"

#include <cctype>
#include <fstream>
#include <map>
#include <set>

#include "torecomp/gcm.hpp"

namespace torecomp::hostpath {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kMaxComponent = 255;

bool is_hex(char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; }

std::string escape(unsigned char c) {
  static const char* digits = "0123456789ABCDEF";
  std::string s = "%";
  s += digits[c >> 4];
  s += digits[c & 15];
  return s;
}

bool is_device_name(const std::string& name) {
  std::string stem = name.substr(0, name.find('.'));
  while (!stem.empty() && stem.back() == ' ') stem.pop_back();
  std::string up;
  for (char c : stem) up += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL") return true;
  return up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) && up[3] >= '1' &&
         up[3] <= '9';
}

}  // namespace

std::string sanitize_component(const std::string& name) {
  if (name.empty() || name == "." || name == "..") throw Error("unsafe file name in the disc: '" + name + "'");
  std::string out;
  for (std::size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    if (c < 0x20 || c == 0x7F || std::string("<>:\"|?*\\/").find(static_cast<char>(c)) != std::string::npos) {
      out += escape(c);
    } else if (c == '%' && i + 2 < name.size() && is_hex(name[i + 1]) && is_hex(name[i + 2])) {
      out += "%25";
    } else {
      out += static_cast<char>(c);
    }
  }
  if (is_device_name(out)) out = escape(static_cast<unsigned char>(out[0])) + out.substr(1);
  if (out.back() == '.' || out.back() == ' ') {
    const unsigned char last = static_cast<unsigned char>(out.back());
    out.pop_back();
    out += escape(last);
  }
  if (out.size() > kMaxComponent) {
    throw Error("file name too long for a host file system (" + std::to_string(out.size()) + " bytes): '" + name + "'");
  }
  return out;
}

std::string fold(const std::string& s) {
  std::string r = s;
  for (char& c : r) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return r;
}

std::vector<std::string> plan(const std::vector<std::string>& disc_paths) {
  // Owner of every folded host path, keyed by the disc prefix and whether it names a folder.
  std::set<std::string> taken;
  std::map<std::string, std::string> assigned;  // disc prefix (folders end in '/') -> host path
  std::vector<std::string> result;
  result.reserve(disc_paths.size());

  for (const std::string& dp : disc_paths) {
    std::string host;
    std::string prefix;
    std::size_t start = 0;
    for (;;) {
      const std::size_t slash = dp.find('/', start);
      const bool last = slash == std::string::npos;
      const std::string comp = dp.substr(start, last ? std::string::npos : slash - start);
      prefix += comp;
      if (!last) prefix += '/';
      auto it = assigned.find(prefix);
      if (it == assigned.end()) {
        const std::string base = sanitize_component(comp);
        const std::string parent = host.empty() ? "" : host + "/";
        std::string cand = base;
        for (unsigned n = 1; taken.count(fold(parent + cand)) != 0; ++n) {
          const std::size_t dot = base.rfind('.');
          const std::size_t at = (dot == std::string::npos || dot == 0) ? base.size() : dot;
          cand = base.substr(0, at) + "~" + std::to_string(n) + base.substr(at);
          if (cand.size() > kMaxComponent) throw Error("cannot find a unique host name for '" + dp + "'");
        }
        taken.insert(fold(parent + cand));
        it = assigned.emplace(prefix, parent + cand).first;
      }
      host = it->second;
      if (last) break;
      start = slash + 1;
    }
    result.push_back(host);
  }
  return result;
}

fs::path to_fs(const std::string& utf8_rel) {
#if defined(__cpp_lib_char8_t)
  return fs::path(std::u8string(utf8_rel.begin(), utf8_rel.end()));
#else
  return fs::u8path(utf8_rel);
#endif
}

fs::path long_path(const fs::path& p) {
#ifdef _WIN32
  fs::path abs = fs::absolute(p).lexically_normal();
  abs.make_preferred();
  const std::wstring s = abs.native();
  if (s.rfind(L"\\\\?\\", 0) == 0) return abs;
  if (s.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + s.substr(2));
  return fs::path(L"\\\\?\\" + s);
#else
  return p;
#endif
}

PathMap PathMap::load(const fs::path& extraction_dir) {
  PathMap m;
  m.dir_ = extraction_dir;
  const fs::path file = extraction_dir / kPathMapFile;
  std::ifstream in(long_path(file), std::ios::binary);
  if (!in) throw Error("cannot open " + file.string());
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) continue;
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0 || tab + 1 == line.size()) {
      throw Error(file.string() + ": malformed line '" + line + "'");
    }
    const std::string disc = line.substr(0, tab);
    const std::string host = line.substr(tab + 1);
    m.exact_.emplace(disc, host);
    m.folded_.emplace(fold(disc), host);
  }
  return m;
}

std::string PathMap::find(const std::string& disc_path) const {
  auto it = exact_.find(disc_path);
  if (it != exact_.end()) return it->second;
  it = folded_.find(fold(disc_path));
  return it == folded_.end() ? std::string() : it->second;
}

fs::path PathMap::find_path(const std::string& disc_path) const {
  const std::string host = find(disc_path);
  if (host.empty()) return {};
  return dir_ / "files" / to_fs(host);
}

}  // namespace torecomp::hostpath
