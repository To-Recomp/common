// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "torecomp/content.hpp"
#include "torecomp/gcm.hpp"

namespace torecomp {

// Per-version maturity of a game, as shown in the launcher.
enum class Status { Stable, Beta, Alpha, Unsupported };

const char* to_string(Status s);

struct DiscRecord {
  std::string game_id;  // 4 characters
  std::uint8_t disc_number = 0;  // 0-based
  std::uint8_t version = 0;
  std::uint64_t size = 0;
  std::string title;
  std::optional<std::int64_t> redump_id;
  std::string sha1;          // whole image, as listed by Redump (may be empty)
  std::string content_sha1;  // see Manifest (may be empty until generated from a trusted dump)
};

// One playable version of a game: a platform + region + the discs it needs.
struct Release {
  std::string id;
  std::string platform;
  std::string region;
  std::string title;
  Status status = Status::Unsupported;
  bool tested = false;  // verified by someone with a real dump
  std::vector<DiscRecord> discs;
};

// Contents of a game repository's dumps.toml (format 1).
class Database {
public:
  static Database load(const std::filesystem::path& file);
  static Database parse(std::string_view toml_text, const std::string& source_name = "dumps.toml");

  const std::vector<Release>& releases() const { return releases_; }

private:
  std::vector<Release> releases_;
};

enum class Level {
  RedumpVerified,  // whole-image SHA-1 matches a Redump entry
  Compatible,      // game files are identical to a known dump, image bytes differ (scrubbed, rebuilt)
  Unknown,         // not in the database, or game files differ
};

const char* to_string(Level l);

struct Identification {
  Level level = Level::Unknown;
  // Set when the disc header (game ID, disc number, version) is known, even if Level is Unknown.
  const Release* release = nullptr;
  const DiscRecord* disc = nullptr;
  std::string sha1;          // whole-image hash (always computed)
  std::string content_sha1;  // empty when the Redump hash already matched
};

Identification identify(const Database& db, gcm::Image& image, const Progress& progress = {});

// Disc numbers (0-based) of `release` that none of `found` provides at level != Unknown.
std::vector<unsigned> missing_discs(const Release& release, const std::vector<Identification>& found);

}  // namespace torecomp
