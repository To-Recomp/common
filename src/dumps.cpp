// SPDX-License-Identifier: GPL-3.0-or-later
#include "torecomp/dumps.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>

#define TOML_HEADER_ONLY 1
#include "toml.hpp"

namespace torecomp {

const char* to_string(Status s) {
  switch (s) {
    case Status::Stable: return "stable";
    case Status::Beta: return "beta";
    case Status::Alpha: return "alpha";
    case Status::Unsupported: return "unsupported";
  }
  return "unsupported";
}

const char* to_string(Level l) {
  switch (l) {
    case Level::RedumpVerified: return "Redump verified";
    case Level::Compatible: return "Compatible (game files identical to a known dump)";
    case Level::Unknown: return "Unknown";
  }
  return "Unknown";
}

namespace {

template <typename T>
T required(const toml::table& t, std::string_view key, const std::string& where) {
  if (auto v = t[key].value<T>()) return *v;
  throw Error(where + ": missing or invalid '" + std::string(key) + "'");
}

template <typename T>
std::optional<T> optional_value(const toml::table& t, std::string_view key) {
  return t[key].value<T>();
}

Status parse_status(const std::string& s, const std::string& where) {
  if (s == "stable") return Status::Stable;
  if (s == "beta") return Status::Beta;
  if (s == "alpha") return Status::Alpha;
  if (s == "unsupported") return Status::Unsupported;
  throw Error(where + ": status must be stable, beta, alpha or unsupported (got '" + s + "')");
}

bool is_hex_sha1(const std::string& s) {
  return s.size() == 40 && std::all_of(s.begin(), s.end(), [](unsigned char c) {
           return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
         });
}

}  // namespace

Database Database::load(const std::filesystem::path& file) {
  std::ifstream in(file, std::ios::binary);
  if (!in) throw Error("cannot open " + file.string());
  std::stringstream ss;
  ss << in.rdbuf();
  return parse(ss.str(), file.string());
}

Database Database::parse(std::string_view text, const std::string& source) {
  toml::table root;
  try {
    root = toml::parse(text, source);
  } catch (const toml::parse_error& e) {
    throw Error(std::string(e.description()) + " (" + source + ":" + std::to_string(e.source().begin.line) + ")");
  }

  if (required<std::int64_t>(root, "format", source) != 1) throw Error(source + ": unsupported format version");

  Database db;
  const auto* releases = root["release"].as_array();
  if (!releases) throw Error(source + ": no [[release]] entries");
  for (const auto& rn : *releases) {
    const auto* rt = rn.as_table();
    if (!rt) throw Error(source + ": [[release]] must be a table");
    Release r;
    r.id = required<std::string>(*rt, "id", source);
    const std::string where = source + " release '" + r.id + "'";
    r.platform = required<std::string>(*rt, "platform", where);
    r.region = required<std::string>(*rt, "region", where);
    r.title = required<std::string>(*rt, "title", where);
    r.status = parse_status(required<std::string>(*rt, "status", where), where);
    r.tested = optional_value<bool>(*rt, "tested").value_or(false);

    const auto* discs = (*rt)["disc"].as_array();
    if (!discs || discs->empty()) throw Error(where + ": needs at least one [[release.disc]]");
    for (const auto& dn : *discs) {
      const auto* dt = dn.as_table();
      if (!dt) throw Error(where + ": [[release.disc]] must be a table");
      DiscRecord d;
      d.game_id = required<std::string>(*dt, "game_id", where);
      if (d.game_id.size() != 4) throw Error(where + ": game_id must be 4 characters");
      d.disc_number = static_cast<std::uint8_t>(required<std::int64_t>(*dt, "disc", where));
      d.version = static_cast<std::uint8_t>(optional_value<std::int64_t>(*dt, "version").value_or(0));
      d.size = static_cast<std::uint64_t>(required<std::int64_t>(*dt, "size", where));
      d.title = required<std::string>(*dt, "title", where);
      d.redump_id = optional_value<std::int64_t>(*dt, "redump_id");
      d.serial = optional_value<std::string>(*dt, "serial").value_or("");
      d.sha1 = optional_value<std::string>(*dt, "sha1").value_or("");
      d.content_sha1 = optional_value<std::string>(*dt, "content_sha1").value_or("");
      if (d.sha1.empty() && d.content_sha1.empty()) throw Error(where + ": a disc needs sha1 or content_sha1");
      if ((!d.sha1.empty() && !is_hex_sha1(d.sha1)) || (!d.content_sha1.empty() && !is_hex_sha1(d.content_sha1))) {
        throw Error(where + ": hashes must be 40 lowercase hex digits");
      }
      r.discs.push_back(std::move(d));
    }
    db.releases_.push_back(std::move(r));
  }
  return db;
}

Identification identify(const Database& db, gcm::Image& image, const Progress& progress) {
  Identification id;
  const Release* header_release = nullptr;
  const DiscRecord* header_disc = nullptr;
  std::vector<std::pair<const Release*, const DiscRecord*>> candidates;
  for (const auto& r : db.releases()) {
    for (const auto& d : r.discs) {
      if (d.game_id == image.game_id() && d.disc_number == image.disc_number() && d.version == image.version()) {
        candidates.push_back({&r, &d});
      }
    }
  }
  if (!candidates.empty()) {
    header_release = candidates.front().first;
    header_disc = candidates.front().second;
  }

  id.release = header_release;
  id.disc = header_disc;
  if (candidates.empty()) return id;  // not a known disc: skip the expensive hashing

  id.sha1 = hash_whole_image(image, progress);
  for (const auto& [r, d] : candidates) {
    if (!d->sha1.empty() && d->sha1 == id.sha1) {
      id.level = Level::RedumpVerified;
      id.release = r;
      id.disc = d;
      return id;
    }
  }

  id.content_sha1 = build_manifest(image, progress).content_sha1;
  for (const auto& [r, d] : candidates) {
    if (!d->content_sha1.empty() && d->content_sha1 == id.content_sha1) {
      id.level = Level::Compatible;
      id.release = r;
      id.disc = d;
      return id;
    }
  }
  return id;  // header known, files differ: Level::Unknown with the release set
}

std::vector<unsigned> missing_discs(const Release& release, const std::vector<Identification>& found) {
  std::vector<unsigned> missing;
  for (const auto& d : release.discs) {
    const bool present = std::any_of(found.begin(), found.end(), [&](const Identification& f) {
      return f.level != Level::Unknown && f.disc == &d;
    });
    if (!present) missing.push_back(d.disc_number);
  }
  return missing;
}

}  // namespace torecomp
