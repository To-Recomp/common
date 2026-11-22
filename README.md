# to-recomp/common

Shared C++17 library used by every game recomp: disc reading, dump verification, extraction, and (later) instance configuration and the SDL3 window, input and audio layer. Rendering stays in each platform backend.

Not affiliated with or endorsed by Bandai Namco Entertainment, Sony Interactive Entertainment, Nintendo or any game publisher or developer. This project contains no game files; you bring your own copy of the game.

## Status (M1)

GameCube only, plain `.iso` images only (RVZ, CISO and NKit are not supported yet).

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

Tests use small synthetic discs built in memory; no game data is needed. Built and tested on Linux with GCC only.

## `torecomp-disc`

```
torecomp-disc info <disc.iso>
torecomp-disc hash <disc.iso>
torecomp-disc manifest <disc.iso>
torecomp-disc identify <dumps.toml> <disc.iso>...
torecomp-disc extract <dumps.toml> <disc.iso> <out>
torecomp-disc extract --unverified <disc.iso> <out>
```

`extract` writes `sys/` (boot.bin, bi2.bin, apploader.img, fst.bin, main.dol), `files/` and `paths.tsv`. Names that are unusable on Windows or macOS (`CON`, `a:b`, trailing dots, names differing only by case) are mapped the same way on every OS and never leave the output folder; `paths.tsv` maps each disc path to its host path (`torecomp/hostpath.hpp`, `PathMap`). It refuses discs the database does not recognise.

## `dumps.toml`

Lives in each game repository (see `tos-recomp/dumps.toml`). Format 1: one `[[release]]` per platform and region, with a `[[release.disc]]` for every disc, carrying `game_id`, `disc` (0-based), `size`, `sha1` (Redump) and/or `content_sha1`. Each release has a `status` of `stable`, `beta`, `alpha` or `unsupported`.

## Third-party code

`third_party/tomlplusplus`: toml++ 3.4.0 (MIT), see its `LICENSE`.

License: GPL v3 (`LICENSE`). Contributions: `CONTRIBUTING.md`.
