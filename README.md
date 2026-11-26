# to-recomp/common

Shared C++17 library used by every game recomp: disc reading, dump verification, extraction, and (later) instance configuration and the SDL3 window, input and audio layer. Rendering stays in each platform backend.

Not affiliated with or endorsed by Bandai Namco Entertainment, Sony Interactive Entertainment, Nintendo or any game publisher or developer. This project contains no game files; you bring your own copy of the game.

## Status (M1)

GameCube only. Disc images can be plain `.iso`, **CISO**, **GCZ**, **WIA** or **RVZ**; the format is detected from the file's magic number and every command works the same on all of them (same content hash, same `identify` result). Wii discs and NKit images are not supported.

| Format | Needs | Notes |
| --- | --- | --- |
| CISO | nothing | The format does not store the disc size: it ends at the last stored block, so a CISO whose last blocks are all zero is reported shorter than the ISO (the content hash is unaffected, the whole-image SHA-1 may differ). |
| GCZ | zlib | Every block's Adler-32 is verified. |
| WIA | bzip2, liblzma (PURGE and uncompressed need nothing) | Header and PURGE checksums are verified. |
| RVZ | zstd, bzip2, liblzma | Including RVZ-packed padding (lagged Fibonacci seeds). |

The decompression libraries are found by CMake (`ZLIB`, `BZip2`, `LibLZMA`, and zstd via `find_path`/`find_library`; install e.g. `zlib1g-dev libbz2-dev liblzma-dev libzstd-dev`). A missing one only disables the formats that need it: opening such an image fails with an error that names the library. Configure with `-DTORECOMP_REQUIRE_CODECS=ON` to make a missing library a configuration error instead.

| Piece | Header | What it does |
| --- | --- | --- |
| SHA-1 | `torecomp/sha1.hpp` | Streaming SHA-1. |
| GCM reader | `torecomp/gcm.hpp` | Parses the disc header, apploader, `main.dol` and file system. Every offset and name is bounds-checked; malformed images throw `torecomp::Error`. |
| Content hash and extraction | `torecomp/content.hpp` | Hashes the game's files independently of padding and layout, and extracts a disc while hashing it. Extraction removes its output when the hash does not match. |
| Dump database | `torecomp/dumps.hpp` | Reads a game's `dumps.toml` and classifies a disc as *Redump verified*, *Compatible* or *Unknown*. Reports which discs of a multi-disc game are missing. |

### Two levels of verification

- **Redump verified**: the SHA-1 of the whole image matches a Redump entry.
- **Compatible**: the image differs from Redump, but every game file is identical to a known dump. This is what a scrubbed image (zeroed padding) gives. The `content_sha1` is documented in `include/torecomp/content.hpp`.

A `content_sha1` generated from a scrubbed image has not been compared with a Redump-verified one. Regenerating the original padding to prove equality with Redump is not implemented.

## Build and test

```
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

Tests use small synthetic discs built in memory; no game data is needed. The tests contain their own minimal CISO, GCZ, WIA and RVZ writers (none, PURGE, bzip2, LZMA, LZMA2 and Zstandard, with RVZ padding packing) and check every reader against the plain image; codecs that are not installed are skipped with a note.

Manual check with a real disc: `TOREC_TEST_ISO=/path/to/game.iso ./build/torecomp_tests` converts that disc with the test writers (CISO, GCZ, WIA-PURGE, RVZ-Zstd; needs about three times the disc size in RAM) and verifies that the content hash and the whole-image SHA-1 are identical to the ISO's. A second manual check is to convert the disc to RVZ/WIA/GCZ/CISO with Dolphin (`dolphin-tool convert`) and compare `torecomp-disc hash` and `manifest` of both files. That is the only way to test images written by real encoders (the test writers are ours); it has not been done in CI.

Built and tested on Linux with GCC only.

## `torecomp-disc`

```
torecomp-disc info <disc>
torecomp-disc hash <disc>
torecomp-disc manifest <disc>
torecomp-disc identify <dumps.toml> <disc>...
torecomp-disc extract <dumps.toml> <disc> <out>
torecomp-disc extract --unverified <disc> <out>
```

`extract` writes `sys/` (boot.bin, bi2.bin, apploader.img, fst.bin, main.dol), `files/` and `paths.tsv`. Names that are unusable on Windows or macOS (`CON`, `a:b`, trailing dots, names differing only by case) are mapped the same way on every OS and never leave the output folder; `paths.tsv` maps each disc path to its host path (`torecomp/hostpath.hpp`, `PathMap`). It refuses discs the database does not recognise.

## `dumps.toml`

Lives in each game repository (see `tos-recomp/dumps.toml`). Format 1: one `[[release]]` per platform and region, with a `[[release.disc]]` for every disc, carrying `game_id`, `disc` (0-based), `size`, `sha1` (Redump) and/or `content_sha1`, and optionally `redump_id` and `serial` (the Redump entry and serial, shown to a player whose image is not the known one). Each release has a `status` of `stable`, `beta`, `alpha` or `unsupported`.

## Third-party code

`third_party/tomlplusplus`: toml++ 3.4.0 (MIT), see its `LICENSE`.

## AI assistance

This project is developed with the help of AI coding assistants (large language models). A large part of the code, tests, documentation and issues is written by AI agents; the maintainer directs the work and reviews it. The result can still contain mistakes: check what matters to you before relying on it. Generated code is under the same licence as the rest of the repository.

License: GPL v3 (`LICENSE`). Contributions: `CONTRIBUTING.md`.
