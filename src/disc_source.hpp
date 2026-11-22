// SPDX-License-Identifier: GPL-3.0-or-later
// Internal: the byte-level backing store behind gcm::Image. One implementation per container
// format; each presents the disc as a flat, randomly readable byte range.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>

namespace torecomp::gcm {

class DiscSource {
public:
  virtual ~DiscSource() = default;
  // Size of the disc image the container represents.
  virtual std::uint64_t size() const = 0;
  // Format name: "ISO", "CISO", "GCZ", "WIA" or "RVZ".
  virtual const char* format() const = 0;
  // The caller has already checked offset + length <= size(). Throws torecomp::Error on any failure.
  virtual void read(std::uint64_t offset, void* dst, std::size_t length) = 0;
};

// Recognises the container by its magic number and opens it. Anything unrecognised is treated as
// a plain ISO.
std::unique_ptr<DiscSource> open_source(const std::filesystem::path& path);

std::unique_ptr<DiscSource> open_wia_rvz(const std::filesystem::path& path, bool rvz);

}  // namespace torecomp::gcm
