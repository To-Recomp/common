// SPDX-License-Identifier: GPL-3.0-or-later
// torecomp-disc: inspect, verify and extract GameCube disc images.
#include <algorithm>
#include <cstdio>
#include <iostream>
#include <string>
#include <vector>

#include "torecomp/content.hpp"
#include "torecomp/dumps.hpp"
#include "torecomp/gcm.hpp"

using namespace torecomp;

namespace {

void usage() {
  std::fputs(
      "usage: torecomp-disc <command> ...\n"
      "  info <disc>                       header, apploader, main.dol and file count\n"
      "  hash <disc>                       whole-image SHA-1 (what Redump lists)\n"
      "  manifest <disc>                   content manifest and content_sha1 for dumps.toml\n"
      "  identify <dumps.toml> <disc>...   match discs against the database, report missing discs\n"
      "  extract <dumps.toml> <disc> <out> extract a disc that the database recognises\n"
      "  extract --unverified <disc> <out> extract without a database (no hash check)\n",
      stderr);
}

Progress make_progress(const char* label) {
  return [label, last = std::uint64_t(101)](std::uint64_t done, std::uint64_t total) mutable {
    const std::uint64_t pct = total ? done * 100 / total : 100;
    if (pct != last) {
      last = pct;
      std::fprintf(stderr, "\r%s %3llu%%", label, static_cast<unsigned long long>(pct));
      if (pct == 100) std::fputc('\n', stderr);
    }
  };
}

int cmd_info(const char* path) {
  gcm::Image img(path);
  std::printf("file:       %s\n", path);
  std::printf("format:     %s\n", img.format());
  std::printf("size:       %llu\n", static_cast<unsigned long long>(img.size()));
  std::printf("game id:    %s (maker %s)\n", img.game_id().c_str(), img.maker().c_str());
  std::printf("disc:       %u (0-based), version %u\n", img.disc_number(), img.version());
  std::printf("title:      %s\n", img.title().c_str());
  std::printf("apploader:  0x%llx, %llu bytes\n", static_cast<unsigned long long>(img.apploader().offset),
              static_cast<unsigned long long>(img.apploader().size));
  std::printf("main.dol:   0x%llx, %llu bytes\n", static_cast<unsigned long long>(img.main_dol().offset),
              static_cast<unsigned long long>(img.main_dol().size));
  std::printf("files:      %zu\n", img.files().size());
  return 0;
}

int cmd_identify(const char* db_path, const std::vector<std::string>& discs) {
  const Database db = Database::load(db_path);
  std::vector<Identification> found;
  int rc = 0;
  for (const auto& p : discs) {
    gcm::Image img(p);
    Identification id = identify(db, img, make_progress("hashing"));
    std::printf("%s\n  game id %s, disc %u, version %u\n", p.c_str(), img.game_id().c_str(), img.disc_number() + 1,
                img.version());
    if (id.level != Level::Unknown) {
      std::printf("  %s: %s, disc %u of %zu\n", to_string(id.level), id.release->title.c_str(),
                  id.disc->disc_number + 1u, id.release->discs.size());
    } else if (id.release) {
      std::printf("  Unknown: the header matches '%s' but the game files differ from the known dump\n",
                  id.release->title.c_str());
      std::printf("  whole-image SHA-1 %s\n  content_sha1      %s\n", id.sha1.c_str(), id.content_sha1.c_str());
      rc = 1;
    } else {
      std::printf("  Unknown: not in %s\n", db_path);
      rc = 1;
    }
    found.push_back(std::move(id));
  }
  // Report missing discs for every release that was recognised at least once.
  std::vector<const Release*> seen;
  for (const auto& f : found) {
    if (f.release && std::find(seen.begin(), seen.end(), f.release) == seen.end()) seen.push_back(f.release);
  }
  for (const Release* r : seen) {
    const auto missing = missing_discs(*r, found);
    if (missing.empty()) {
      std::printf("%s: all %zu disc(s) present\n", r->title.c_str(), r->discs.size());
    } else {
      std::printf("%s: missing disc(s):", r->title.c_str());
      for (unsigned m : missing) std::printf(" %u", m + 1);
      std::printf("\n");
      rc = 1;
    }
  }
  return rc;
}

int cmd_extract(const char* db_path, const char* disc, const char* out) {
  gcm::Image img(disc);
  std::string expected;
  if (db_path) {
    const Database db = Database::load(db_path);
    const Identification id = identify(db, img, make_progress("verifying"));
    if (id.level == Level::Unknown) {
      std::fprintf(stderr, "refusing to extract: %s is not a known dump (see `identify`)\n", disc);
      return 1;
    }
    expected = id.disc->content_sha1;
  }
  const Manifest m = extract(img, out, expected, make_progress("extracting"));
  std::printf("extracted %zu entries, content_sha1 %s\n", m.lines.size(), m.content_sha1.c_str());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  try {
    if (argc < 3) {
      usage();
      return 2;
    }
    const std::string cmd = argv[1];
    if (cmd == "info" && argc == 3) return cmd_info(argv[2]);
    if (cmd == "hash" && argc == 3) {
      gcm::Image img(argv[2]);
      std::printf("%s\n", hash_whole_image(img, make_progress("hashing")).c_str());
      return 0;
    }
    if (cmd == "manifest" && argc == 3) {
      gcm::Image img(argv[2]);
      const Manifest m = build_manifest(img, make_progress("hashing"));
      std::fputs(m.text().c_str(), stdout);
      std::fprintf(stderr, "content_sha1 = \"%s\"\n", m.content_sha1.c_str());
      return 0;
    }
    if (cmd == "identify" && argc >= 4) return cmd_identify(argv[2], std::vector<std::string>(argv + 3, argv + argc));
    if (cmd == "extract" && argc == 5) {
      if (std::string(argv[2]) == "--unverified") return cmd_extract(nullptr, argv[3], argv[4]);
      return cmd_extract(argv[2], argv[3], argv[4]);
    }
    usage();
    return 2;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "error: %s\n", e.what());
    return 1;
  }
}
