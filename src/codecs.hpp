// SPDX-License-Identifier: GPL-3.0-or-later
// Thin wrappers over the optional decompression libraries (zlib, bzip2, liblzma, zstd).
// Each call must produce exactly `out_size` bytes. When the library was not found at
// configure time the call throws torecomp::Error naming the missing library.
#pragma once

#include <cstddef>
#include <cstdint>

namespace torecomp::codec {

void zlib_inflate(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out, std::size_t out_size);
std::uint32_t adler32(const std::uint8_t* data, std::size_t size);
void bzip2_decompress(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out, std::size_t out_size);
// `props` is the 7-Zip style property blob stored in WIA: 5 bytes for LZMA, 1 byte for LZMA2.
void lzma_decompress(const std::uint8_t* props, std::size_t props_size, bool lzma2, const std::uint8_t* in,
                     std::size_t in_size, std::uint8_t* out, std::size_t out_size);
void zstd_decompress(const std::uint8_t* in, std::size_t in_size, std::uint8_t* out, std::size_t out_size);

}  // namespace torecomp::codec
