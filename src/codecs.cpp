// SPDX-License-Identifier: GPL-3.0-or-later
#include "codecs.hpp"

#include <cstring>

#include "torecomp/gcm.hpp"

#ifdef TORECOMP_HAVE_ZLIB
#include <zlib.h>
#endif
#ifdef TORECOMP_HAVE_BZIP2
#include <bzlib.h>
#endif
#ifdef TORECOMP_HAVE_LZMA
#include <lzma.h>
#endif
#ifdef TORECOMP_HAVE_ZSTD
#include <zstd.h>
#endif

namespace torecomp::codec {

namespace {
[[noreturn, maybe_unused]] void missing(const char* lib) {
  throw Error(std::string("this image needs ") + lib +
              " decompression, but torecomp was built without it (install the library's development "
              "package and reconfigure)");
}
}  // namespace

void zlib_inflate([[maybe_unused]] const std::uint8_t* in, [[maybe_unused]] std::size_t in_size,
                  [[maybe_unused]] std::uint8_t* out, [[maybe_unused]] std::size_t out_size) {
#ifdef TORECOMP_HAVE_ZLIB
  uLongf produced = static_cast<uLongf>(out_size);
  if (uncompress(out, &produced, in, static_cast<uLong>(in_size)) != Z_OK || produced != out_size) {
    throw Error("corrupt zlib data");
  }
#else
  missing("zlib");
#endif
}

std::uint32_t adler32([[maybe_unused]] const std::uint8_t* data, [[maybe_unused]] std::size_t size) {
#ifdef TORECOMP_HAVE_ZLIB
  return static_cast<std::uint32_t>(::adler32(1, data, static_cast<uInt>(size)));
#else
  missing("zlib");
#endif
}

void bzip2_decompress([[maybe_unused]] const std::uint8_t* in, [[maybe_unused]] std::size_t in_size,
                      [[maybe_unused]] std::uint8_t* out, [[maybe_unused]] std::size_t out_size) {
#ifdef TORECOMP_HAVE_BZIP2
  unsigned int produced = static_cast<unsigned int>(out_size);
  if (BZ2_bzBuffToBuffDecompress(reinterpret_cast<char*>(out), &produced,
                                 const_cast<char*>(reinterpret_cast<const char*>(in)),
                                 static_cast<unsigned int>(in_size), 0, 0) != BZ_OK ||
      produced != out_size) {
    throw Error("corrupt bzip2 data");
  }
#else
  missing("bzip2");
#endif
}

void lzma_decompress([[maybe_unused]] const std::uint8_t* props, [[maybe_unused]] std::size_t props_size,
                     [[maybe_unused]] bool lzma2, [[maybe_unused]] const std::uint8_t* in,
                     [[maybe_unused]] std::size_t in_size, [[maybe_unused]] std::uint8_t* out,
                     [[maybe_unused]] std::size_t out_size) {
#ifdef TORECOMP_HAVE_LZMA
  lzma_options_lzma opt;
  std::memset(&opt, 0, sizeof opt);
  if (lzma2) {
    if (props_size < 1 || props[0] > 40) throw Error("invalid LZMA2 properties");
    const std::uint8_t p = props[0];
    opt.dict_size = p == 40 ? 0xFFFFFFFFu : ((2u | (p & 1u)) << (p / 2 + 11));
  } else {
    if (props_size < 5 || props[0] >= 9 * 5 * 5) throw Error("invalid LZMA properties");
    std::uint32_t d = props[0];
    opt.lc = d % 9;
    d /= 9;
    opt.pb = d / 5;
    opt.lp = d % 5;
    opt.dict_size = std::uint32_t(props[1]) | (std::uint32_t(props[2]) << 8) | (std::uint32_t(props[3]) << 16) |
                    (std::uint32_t(props[4]) << 24);
  }
  lzma_filter filters[2] = {{lzma2 ? LZMA_FILTER_LZMA2 : LZMA_FILTER_LZMA1, &opt}, {LZMA_VLI_UNKNOWN, nullptr}};
  lzma_stream strm = LZMA_STREAM_INIT;
  if (lzma_raw_decoder(&strm, filters) != LZMA_OK) throw Error("cannot initialise the LZMA decoder");
  strm.next_in = in;
  strm.avail_in = in_size;
  strm.next_out = out;
  strm.avail_out = out_size;
  lzma_ret r;
  do {
    r = lzma_code(&strm, LZMA_FINISH);
  } while (r == LZMA_OK && strm.avail_out != 0);
  const bool ok = (r == LZMA_OK || r == LZMA_STREAM_END) && strm.avail_out == 0;
  lzma_end(&strm);
  if (!ok) throw Error("corrupt LZMA data");
#else
  missing("LZMA");
#endif
}

void zstd_decompress([[maybe_unused]] const std::uint8_t* in, [[maybe_unused]] std::size_t in_size,
                     [[maybe_unused]] std::uint8_t* out, [[maybe_unused]] std::size_t out_size) {
#ifdef TORECOMP_HAVE_ZSTD
  const std::size_t r = ZSTD_decompress(out, out_size, in, in_size);
  if (ZSTD_isError(r) || r != out_size) throw Error("corrupt Zstandard data");
#else
  missing("Zstandard");
#endif
}

}  // namespace torecomp::codec
