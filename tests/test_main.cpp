// SPDX-License-Identifier: GPL-3.0-or-later
// Tests use small synthetic GameCube images built in memory. No game data is involved.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <algorithm>
#include <chrono>
#include <iterator>

#include "torecomp/content.hpp"
#include "torecomp/dumps.hpp"
#include "torecomp/gcm.hpp"
#include "torecomp/hostpath.hpp"
#include "torecomp/sha1.hpp"

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

using namespace torecomp;
namespace fs = std::filesystem;

static int g_failures = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

#define CHECK_THROWS(expr)                                                   \
  do {                                                                       \
    bool threw_ = false;                                                     \
    try {                                                                    \
      expr;                                                                  \
    } catch (const torecomp::Error&) {                                       \
      threw_ = true;                                                         \
    }                                                                        \
    if (!threw_) {                                                           \
      std::fprintf(stderr, "%s:%d: expected torecomp::Error from %s\n", __FILE__, __LINE__, #expr); \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

// ---- synthetic disc ---------------------------------------------------------------------------

struct Node {
  bool dir = true;
  std::string data;
  std::map<std::string, Node> kids;
};

using Bytes = std::vector<std::uint8_t>;

static void put32(Bytes& b, std::size_t off, std::uint32_t v) {
  b[off] = v >> 24;
  b[off + 1] = v >> 16;
  b[off + 2] = v >> 8;
  b[off + 3] = v;
}

static void add_file(Node& root, const std::string& path, const std::string& data) {
  Node* n = &root;
  std::size_t start = 0;
  for (;;) {
    const std::size_t slash = path.find('/', start);
    if (slash == std::string::npos) {
      Node f;
      f.dir = false;
      f.data = data;
      n->kids[path.substr(start)] = f;
      return;
    }
    n = &n->kids[path.substr(start, slash - start)];
    start = slash + 1;
  }
}

struct Item {
  bool dir;
  std::string name;
  std::uint32_t a, b;  // dir: parent,next; file: offset,length
  const std::string* data;
};

static void flatten(const Node& n, std::uint32_t parent, std::vector<Item>& out) {
  for (const auto& [name, kid] : n.kids) {
    const std::uint32_t index = static_cast<std::uint32_t>(out.size());
    if (kid.dir) {
      out.push_back({true, name, parent, 0, nullptr});
      flatten(kid, index + 1, out);  // FST indices are 1-based here: entry 0 is the root
      out[index].b = static_cast<std::uint32_t>(out.size() + 1);
    } else {
      out.push_back({false, name, 0, static_cast<std::uint32_t>(kid.data.size()), &kid.data});
    }
  }
}

constexpr std::size_t kFst = 0x4000;
constexpr std::size_t kFiles = 0x10000;

// `fill` goes into every byte that is not part of the disc structure (the "padding").
static Bytes build_image(const Node& root, std::uint8_t fill, const char* id = "TEST", std::uint8_t disc = 0) {
  std::vector<Item> items;
  flatten(root, 0, items);

  Bytes names;
  std::vector<std::uint32_t> name_off;
  for (const auto& it : items) {
    name_off.push_back(static_cast<std::uint32_t>(names.size()));
    names.insert(names.end(), it.name.begin(), it.name.end());
    names.push_back(0);
  }
  const std::size_t entries = items.size() + 1;
  const std::size_t fst_size = entries * 12 + names.size();

  std::size_t pos = kFiles;
  std::vector<std::size_t> offsets(items.size(), 0);
  for (std::size_t i = 0; i < items.size(); ++i) {
    if (items[i].dir) continue;
    offsets[i] = pos;
    pos += (items[i].data->size() + 0xFFF) & ~std::size_t(0xFFF);
  }
  Bytes img(pos + 0x1000, fill);
  std::fill(img.begin(), img.begin() + 0x2440, 0);  // header and bi2 are data, never padding

  std::memcpy(&img[0], id, 4);
  img[4] = 'A';
  img[5] = 'F';
  img[6] = disc;
  img[7] = 0;
  put32(img, 0x1C, 0xC2339F3Du);
  std::memcpy(&img[0x20], "SYNTHETIC TEST DISC", 19);
  put32(img, 0x420, 0x3000);
  put32(img, 0x424, kFst);
  put32(img, 0x428, static_cast<std::uint32_t>(fst_size));
  put32(img, 0x42C, static_cast<std::uint32_t>(fst_size));

  // Apploader: 0x20 header, 0x40 body, no trailer.
  std::fill(img.begin() + 0x2440, img.begin() + 0x2440 + 0x60, 0x11);
  put32(img, 0x2440 + 0x14, 0x40);
  put32(img, 0x2440 + 0x18, 0);

  // DOL: one text section at 0x100, 0x20 bytes.
  std::fill(img.begin() + 0x3000, img.begin() + 0x3000 + 0x120, 0x22);
  std::fill(img.begin() + 0x3000, img.begin() + 0x3000 + 0x100, 0);
  put32(img, 0x3000, 0x100);
  put32(img, 0x3000 + 0x90, 0x20);

  put32(img, kFst + 8, static_cast<std::uint32_t>(entries));
  img[kFst] = 1;
  for (std::size_t i = 0; i < items.size(); ++i) {
    const std::size_t e = kFst + (i + 1) * 12;
    img[e] = items[i].dir ? 1 : 0;
    img[e + 1] = name_off[i] >> 16;
    img[e + 2] = name_off[i] >> 8;
    img[e + 3] = name_off[i];
    if (items[i].dir) {
      put32(img, e + 4, items[i].a);
      put32(img, e + 8, items[i].b);
    } else {
      put32(img, e + 4, static_cast<std::uint32_t>(offsets[i]));
      put32(img, e + 8, static_cast<std::uint32_t>(items[i].data->size()));
      std::memcpy(&img[offsets[i]], items[i].data->data(), items[i].data->size());
    }
  }
  std::memcpy(&img[kFst + entries * 12], names.data(), names.size());
  return img;
}

static Node sample_tree() {
  Node root;
  add_file(root, "opening.bnr", "banner-data");
  add_file(root, "audio/bgm01.dsp", std::string(5000, 'a'));
  add_file(root, "audio/voice/v001.dsp", "voice one");
  add_file(root, "zz_last.bin", "last");
  return root;
}

struct Temp {
  fs::path dir;
  Temp() {
    dir = fs::temp_directory_path() /
          ("torecomp-tests-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::remove_all(dir);
    fs::create_directories(dir);
  }
  ~Temp() { std::error_code ec; fs::remove_all(dir, ec); }
  fs::path write(const std::string& name, const Bytes& b) const {
    const fs::path p = dir / name;
    std::ofstream(p, std::ios::binary).write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return p;
  }
};

// ---- tests ------------------------------------------------------------------------------------

static void test_sha1() {
  CHECK(Sha1::hex_of("", 0) == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
  CHECK(Sha1::hex_of("abc", 3) == "a9993e364706816aba3e25717850c26c9cd0d89d");
  const std::string two_blocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
  CHECK(Sha1::hex_of(two_blocks.data(), two_blocks.size()) == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");

  // Splitting the input across update() calls must not change the result.
  std::string big(1000, 'x');
  Sha1 a;
  for (std::size_t i = 0; i < big.size(); i += 7) a.update(big.data() + i, std::min<std::size_t>(7, big.size() - i));
  CHECK(Sha1::hex(a.finish()) == Sha1::hex_of(big.data(), big.size()));
}

static void test_parse(const Temp& t) {
  const Node tree = sample_tree();
  gcm::Image img(t.write("a.iso", build_image(tree, 0)));
  CHECK(img.game_id() == "TEST");
  CHECK(img.disc_number() == 0);
  CHECK(img.main_dol().size == 0x120);
  CHECK(img.apploader().size == 0x60);
  CHECK(img.files().size() == 4);
  CHECK(img.files()[0].path == "audio/bgm01.dsp");
  CHECK(img.files()[0].size == 5000);
  CHECK(img.files()[1].path == "audio/voice/v001.dsp");
  CHECK(img.files()[2].path == "opening.bnr");
  CHECK(img.files()[3].path == "zz_last.bin");
}

static void test_malformed(const Temp& t) {
  // Wrong magic.
  Bytes bad = build_image(sample_tree(), 0);
  bad[0x1C] = 0;
  CHECK_THROWS(gcm::Image(t.write("magic.iso", bad)));

  // A file that claims to extend past the end of the image.
  Node huge;
  add_file(huge, "big.bin", "x");
  Bytes b = build_image(huge, 0);
  put32(b, kFst + 12 + 8, 0x7FFFFFFF);
  CHECK_THROWS(gcm::Image(t.write("range.iso", b)));

  // A path component that would escape the output folder.
  Node evil;
  add_file(evil, "..", "x");
  CHECK_THROWS(gcm::Image(t.write("dotdot.iso", build_image(evil, 0))));

  // A directory whose range loops back on itself.
  Bytes loop = build_image(sample_tree(), 0);
  // Entry 1 is the "audio" directory; make its "next" point at itself.
  put32(loop, kFst + 12 + 8, 1);
  CHECK_THROWS(gcm::Image(t.write("loop.iso", loop)));
}

static void test_content_hash_ignores_padding(const Temp& t) {
  const Node tree = sample_tree();
  gcm::Image zero(t.write("zero.iso", build_image(tree, 0x00)));
  gcm::Image junk(t.write("junk.iso", build_image(tree, 0xAB)));
  CHECK(hash_whole_image(zero) != hash_whole_image(junk));  // Redump-style hash sees padding
  CHECK(build_manifest(zero).content_sha1 == build_manifest(junk).content_sha1);

  Node changed = sample_tree();
  changed.kids["zz_last.bin"].data = "LAST";
  gcm::Image other(t.write("other.iso", build_image(changed, 0x00)));
  CHECK(build_manifest(zero).content_sha1 != build_manifest(other).content_sha1);
}

static std::string db_text(const std::string& sha1, const std::string& content) {
  return "format = 1\n[[release]]\nid = \"test\"\nplatform = \"gamecube\"\nregion = \"X\"\ntitle = \"Test Game\"\n"
         "status = \"alpha\"\n[[release.disc]]\ngame_id = \"TEST\"\ndisc = 0\nsize = 1\ntitle = \"Disc 1\"\n"
         "sha1 = \"" + sha1 + "\"\ncontent_sha1 = \"" + content + "\"\n"
         "[[release.disc]]\ngame_id = \"TEST\"\ndisc = 1\nsize = 1\ntitle = \"Disc 2\"\n"
         "content_sha1 = \"" + std::string(40, '1') + "\"\n";
}

static void test_identify(const Temp& t) {
  const Node tree = sample_tree();
  gcm::Image original(t.write("orig.iso", build_image(tree, 0xAB)));
  const std::string whole = hash_whole_image(original);
  const std::string content = build_manifest(original).content_sha1;
  const Database db = Database::parse(db_text(whole, content));

  Identification a = identify(db, original);
  CHECK(a.level == Level::RedumpVerified);
  CHECK(a.disc && a.disc->disc_number == 0);

  gcm::Image scrubbed(t.write("scrub.iso", build_image(tree, 0x00)));
  Identification b = identify(db, scrubbed);
  CHECK(b.level == Level::Compatible);

  Node changed = sample_tree();
  changed.kids["opening.bnr"].data = "tampered!!!";
  gcm::Image tampered(t.write("tamper.iso", build_image(changed, 0xAB)));
  Identification c = identify(db, tampered);
  CHECK(c.level == Level::Unknown);
  CHECK(c.release != nullptr);  // header recognised, files differ

  gcm::Image stranger(t.write("other-id.iso", build_image(tree, 0, "NOPE")));
  CHECK(identify(db, stranger).release == nullptr);

  // Only disc 1 of 2 was supplied.
  std::vector<Identification> found{a};
  const auto missing = missing_discs(db.releases()[0], found);
  CHECK(missing.size() == 1 && missing[0] == 1);
}

static void test_database_rejects_bad_input() {
  CHECK_THROWS(Database::parse("format = 2\n"));
  CHECK_THROWS(Database::parse(db_text("zz", "yy")));  // not hex
  CHECK_THROWS(Database::parse("format = 1\n[[release]]\nid = \"x\"\n"));  // missing fields
}

static void test_extract(const Temp& t) {
  const Node tree = sample_tree();
  gcm::Image img(t.write("ex.iso", build_image(tree, 0x00)));
  const std::string expected = build_manifest(img).content_sha1;

  const fs::path out = t.dir / "out";
  Manifest m = extract(img, out, expected);
  CHECK(m.content_sha1 == expected);
  CHECK(fs::exists(out / "sys" / "main.dol"));
  CHECK(fs::file_size(out / "sys" / "main.dol") == 0x120);
  CHECK(fs::file_size(out / "files" / "audio" / "bgm01.dsp") == 5000);
  std::ifstream f(out / "files" / "audio" / "voice" / "v001.dsp", std::ios::binary);
  std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  CHECK(content == "voice one");

  // Refuses a non-empty destination.
  CHECK_THROWS(extract(img, out));

  // A hash mismatch removes everything that was written.
  const fs::path out2 = t.dir / "out2";
  CHECK_THROWS(extract(img, out2, std::string(40, '0')));
  CHECK(!fs::exists(out2));
}

static std::string slurp(const fs::path& p) {
  std::ifstream f(hostpath::long_path(p), std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static void test_sanitize() {
  using hostpath::sanitize_component;
  CHECK(sanitize_component("plain.bin") == "plain.bin");
  CHECK(sanitize_component("a<b>c:d\"e|f?g*h") == "a%3Cb%3Ec%3Ad%22e%7Cf%3Fg%2Ah");
  CHECK(sanitize_component("CON") == "%43ON");
  CHECK(sanitize_component("nul.txt") == "%6Eul.txt");
  CHECK(sanitize_component("Com1") == "%43om1");
  CHECK(sanitize_component("LPT9.dat") == "%4CPT9.dat");
  CHECK(sanitize_component("COM0") == "COM0");
  CHECK(sanitize_component("CONSOLE") == "CONSOLE");
  CHECK(sanitize_component("name.") == "name%2E");
  CHECK(sanitize_component("name ") == "name%20");
  CHECK(sanitize_component("100%25") == "100%2525");  // a literal escape-looking sequence stays distinct
  CHECK(sanitize_component("100%") == "100%");
  CHECK(sanitize_component("caf\xC3\xA9.txt") == "caf\xC3\xA9.txt");
  CHECK_THROWS(sanitize_component(""));
  CHECK_THROWS(sanitize_component("."));
  CHECK_THROWS(sanitize_component(".."));
  CHECK_THROWS(sanitize_component(std::string(256, 'a')));
  CHECK(sanitize_component(std::string(255, 'a')).size() == 255);
}

static void test_plan_collisions() {
  using hostpath::plan;
  // Case-insensitive collisions: the later path (in sorted order) is renamed, reproducibly.
  const std::vector<std::string> paths = {"Data/A.bin", "Data/a.bin", "Data/a.BIN", "data/b.bin", "x", "x/y"};
  const auto host = plan(paths);
  CHECK(host.size() == paths.size());
  CHECK(host[0] == "Data/A.bin");
  CHECK(host[1] == "Data/a~1.bin");
  CHECK(host[2] == "Data/a~2.BIN");
  CHECK(host[3] == "data~1/b.bin");  // folder that differs only by case
  CHECK(host[4] == "x");
  CHECK(host[5] == "x~1/y");  // file and folder with one name
  CHECK(plan(paths) == host);
  std::set<std::string> seen;
  for (const auto& h : host) CHECK(seen.insert(hostpath::fold(h)).second);
}

static void test_extract_windows_unsafe_names(const Temp& t) {
  Node root;
  root.kids["CON"].dir = false;
  root.kids["CON"].data = "device";
  add_file(root, "dir./trail.", "trailing dot");
  add_file(root, "q?.bin", "question");
  root.kids["colon:name"].dir = false;
  root.kids["colon:name"].data = "colon";
  add_file(root, "Case.bin", "upper");
  add_file(root, "case.bin", "lower");
  add_file(root, "aux/nul.txt", "nested device");
  const fs::path iso = t.write("unsafe.iso", build_image(root, 0));
  gcm::Image img(iso);
  const std::string expected = build_manifest(img).content_sha1;

  const fs::path out = t.dir / "unsafe-out";
  const Manifest m = extract(img, out, expected);
  CHECK(m.content_sha1 == expected);  // mapping does not change the content hash

  std::map<std::string, std::string> host;
  for (const auto& l : m.lines) {
    if (l.kind == "file") host[l.path] = l.host_path;
  }
  CHECK(host["CON"] == "%43ON");
  CHECK(host["dir./trail."] == "dir%2E/trail%2E");
  CHECK(host["q?.bin"] == "q%3F.bin");
  CHECK(host["colon:name"] == "colon%3Aname");
  CHECK(host["Case.bin"] == "Case.bin");
  CHECK(host["case.bin"] == "case~1.bin");
  CHECK(host["aux/nul.txt"] == "%61ux/%6Eul.txt");
  CHECK(slurp(out / "files" / "%43ON") == "device");
  CHECK(slurp(out / "files" / "Case.bin") == "upper");
  CHECK(slurp(out / "files" / "case~1.bin") == "lower");
  CHECK(slurp(out / "files" / "%61ux" / "%6Eul.txt") == "nested device");

  // The disc path still finds the file, including with different case.
  const hostpath::PathMap map = hostpath::PathMap::load(out);
  CHECK(map.find("CON") == "%43ON");
  CHECK(map.find("con") == "%43ON");
  CHECK(map.find("DIR./TRAIL.") == "dir%2E/trail%2E");
  CHECK(map.find("case.bin") == "case~1.bin");  // exact match beats case folding
  CHECK(map.find("CASE.BIN") == "Case.bin");
  CHECK(map.find("missing.bin").empty());
  CHECK(slurp(map.find_path("Colon:Name")) == "colon");
  CHECK(map.find_path("missing.bin").empty());

  // Same disc, same mapping.
  gcm::Image again(iso);
  const fs::path out2 = t.dir / "unsafe-out2";
  const Manifest m2 = extract(again, out2);
  for (std::size_t i = 0; i < m.lines.size(); ++i) CHECK(m.lines[i].host_path == m2.lines[i].host_path);
}

static void test_extract_never_escapes(const Temp& t) {
  // Names that the image parser accepts must stay inside the output folder after mapping; names that
  // could escape it are rejected before anything is written.
  const char* bad[] = {"..", ".", "", "a/b", "a\\b", "/abs", "C:\\evil"};
  int n = 0;
  for (const char* name : bad) {
    Node root;
    root.kids[name].dir = false;
    root.kids[name].data = "x";
    const fs::path iso = t.write("evil" + std::to_string(n++) + ".iso", build_image(root, 0));
    // "a/b" and "/abs" become nested/absolute paths in the helper's tree only when split; here the
    // slash is inside one FST name, which the parser must refuse.
    bool contained = true;
    try {
      gcm::Image img(iso);
      const fs::path out = t.dir / ("evil-out" + std::to_string(n));
      extract(img, out);
      for (const auto& e : fs::recursive_directory_iterator(out)) {
        const fs::path rel = fs::relative(e.path(), out);
        for (const auto& part : rel) {
          if (part == "..") contained = false;
        }
      }
    } catch (const torecomp::Error&) {
    }
    CHECK(contained);
  }
  CHECK(!fs::exists(t.dir.parent_path() / "evil"));
  CHECK(!fs::exists(t.dir / "abs"));

  // A name of 'only' escapes, absolute, drive letters: refused by the parser.
  Node abs;
  abs.kids["/etc"].dir = false;
  CHECK_THROWS(gcm::Image(t.write("abs.iso", build_image(abs, 0))));
  Node drive;
  drive.kids["C:\\x"].dir = false;
  CHECK_THROWS(gcm::Image(t.write("drive.iso", build_image(drive, 0))));
}

static void test_extract_long_path(const Temp& t) {
  // 3 x 100 + file name: over Windows' MAX_PATH once combined with the temp folder.
  const std::string d(100, 'd');
  Node root;
  add_file(root, d + "/" + d + "/" + d + "/" + std::string(60, 'f') + ".bin", "deep");
  gcm::Image img(t.write("long.iso", build_image(root, 0)));
  const fs::path out = t.dir / "long-out";
  extract(img, out);
  const fs::path file = out / "files" / d / d / d / (std::string(60, 'f') + ".bin");
  CHECK(slurp(file) == "deep");
}

static void test_extract_refuses_overwrite(const Temp& t) {
  // The write-time guard that backs the collision handling on file systems that fold more than ASCII.
  Node root;
  add_file(root, "a.bin", "one");
  gcm::Image img(t.write("ow.iso", build_image(root, 0)));
  const fs::path out = t.dir / "ow-out";
  fs::create_directories(out);
  extract(img, out);  // empty folder is accepted
  CHECK_THROWS(extract(img, out));
}

// ---- compressed containers --------------------------------------------------------------------
// The writers below produce CISO, GCZ, WIA and RVZ files from a plain image so the readers can
// be checked against it. They are written from the format descriptions, independently of the
// readers (the readers are in src/). Formats and codecs that cannot be built here are skipped.

static void be32w(Bytes& b, std::uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
static void be64w(Bytes& b, std::uint64_t v) {
  be32w(b, static_cast<std::uint32_t>(v >> 32));
  be32w(b, static_cast<std::uint32_t>(v));
}
static void le32w(Bytes& b, std::uint32_t v) {
  for (int s = 0; s < 32; s += 8) b.push_back(static_cast<std::uint8_t>(v >> s));
}
static void le64w(Bytes& b, std::uint64_t v) {
  le32w(b, static_cast<std::uint32_t>(v));
  le32w(b, static_cast<std::uint32_t>(v >> 32));
}
static bool all_zero(const std::uint8_t* p, std::size_t n) {
  for (std::size_t i = 0; i < n; ++i) if (p[i]) return false;
  return true;
}
static Bytes slice(const Bytes& b, std::size_t off, std::size_t n) {
  return Bytes(b.begin() + static_cast<std::ptrdiff_t>(off), b.begin() + static_cast<std::ptrdiff_t>(off + n));
}

static Bytes write_ciso(const Bytes& img, std::uint32_t block) {
  Bytes out(0x8000, 0);
  std::memcpy(out.data(), "CISO", 4);
  out[4] = block;
  out[5] = block >> 8;
  out[6] = block >> 16;
  out[7] = block >> 24;
  for (std::size_t b = 0; b * block < img.size(); ++b) {
    const std::size_t n = std::min<std::size_t>(block, img.size() - b * block);
    if (all_zero(&img[b * block], n)) continue;
    out[8 + b] = 1;
    out.insert(out.end(), img.begin() + static_cast<std::ptrdiff_t>(b * block),
               img.begin() + static_cast<std::ptrdiff_t>(b * block + n));
  }
  return out;
}

static std::uint32_t adler(const Bytes& b) {
  std::uint32_t a = 1, c = 0;
  for (std::uint8_t x : b) {
    a = (a + x) % 65521;
    c = (c + a) % 65521;
  }
  return (c << 16) | a;
}

#ifdef TORECOMP_HAVE_ZLIB
static Bytes write_gcz(const Bytes& img, std::uint32_t block) {
  const std::size_t blocks = (img.size() + block - 1) / block;
  Bytes data, ptrs, hashes;
  for (std::size_t b = 0; b < blocks; ++b) {
    const Bytes plain = slice(img, b * block, std::min<std::size_t>(block, img.size() - b * block));
    Bytes z(compressBound(static_cast<uLong>(plain.size())));
    uLongf zn = static_cast<uLongf>(z.size());
    compress2(z.data(), &zn, plain.data(), static_cast<uLong>(plain.size()), 6);
    z.resize(zn);
    const bool stored = z.size() >= plain.size();  // incompressible blocks are stored as is
    const Bytes& chosen = stored ? plain : z;
    le64w(ptrs, data.size() | (stored ? (std::uint64_t(1) << 63) : 0));
    le32w(hashes, adler(chosen));
    data.insert(data.end(), chosen.begin(), chosen.end());
  }
  Bytes out;
  le32w(out, 0xB10BC001u);
  le32w(out, 0);
  le64w(out, data.size());
  le64w(out, img.size());
  le32w(out, block);
  le32w(out, static_cast<std::uint32_t>(blocks));
  out.insert(out.end(), ptrs.begin(), ptrs.end());
  out.insert(out.end(), hashes.begin(), hashes.end());
  out.insert(out.end(), data.begin(), data.end());
  return out;
}
#endif

enum Comp { C_NONE = 0, C_PURGE = 1, C_BZIP2 = 2, C_LZMA = 3, C_LZMA2 = 4, C_ZSTD = 5 };

static bool comp_available(int c) {
  switch (c) {
    case C_NONE: case C_PURGE: return true;
#ifdef TORECOMP_HAVE_BZIP2
    case C_BZIP2: return true;
#endif
#ifdef TORECOMP_HAVE_LZMA
    case C_LZMA: case C_LZMA2: return true;
#endif
#ifdef TORECOMP_HAVE_ZSTD
    case C_ZSTD: return true;
#endif
    default: return false;
  }
}

static Bytes sha1_bytes(const Bytes& b) {
  Sha1 s;
  s.update(b.data(), b.size());
  const auto d = s.finish();
  return Bytes(d.begin(), d.end());
}

static Bytes purge_encode(const Bytes& in) {
  Bytes out;
  std::size_t i = 0;
  while (i < in.size()) {
    if (in[i] == 0) { ++i; continue; }
    std::size_t j = i;
    // a run ends at 64 consecutive zero bytes
    std::size_t zeros = 0;
    while (j < in.size() && zeros < 64) {
      zeros = in[j] == 0 ? zeros + 1 : 0;
      ++j;
    }
    j -= zeros;
    be32w(out, static_cast<std::uint32_t>(i));
    be32w(out, static_cast<std::uint32_t>(j - i));
    out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(i), in.begin() + static_cast<std::ptrdiff_t>(j));
    i = j;
  }
  const Bytes h = sha1_bytes(out);
  out.insert(out.end(), h.begin(), h.end());
  return out;
}

static Bytes lzma_props(int comp) {
  if (comp == C_LZMA) return Bytes{(2 * 5 + 0) * 9 + 3, 0, 0, 0x10, 0};  // lc=3 lp=0 pb=2, 1 MiB dictionary
  return Bytes{18};  // LZMA2 dictionary byte: 2 << (18/2 + 11) = 1 MiB
}

static Bytes compress_with(int comp, const Bytes& in) {
  switch (comp) {
    case C_NONE: return in;
    case C_PURGE: return purge_encode(in);
#ifdef TORECOMP_HAVE_BZIP2
    case C_BZIP2: {
      Bytes out(in.size() + in.size() / 50 + 1024);
      unsigned int n = static_cast<unsigned int>(out.size());
      BZ2_bzBuffToBuffCompress(reinterpret_cast<char*>(out.data()), &n,
                               const_cast<char*>(reinterpret_cast<const char*>(in.data())),
                               static_cast<unsigned int>(in.size()), 9, 0, 0);
      out.resize(n);
      return out;
    }
#endif
#ifdef TORECOMP_HAVE_LZMA
    case C_LZMA: case C_LZMA2: {
      lzma_options_lzma opt;
      lzma_lzma_preset(&opt, 6);
      opt.dict_size = 1u << 20;
      opt.lc = 3;
      opt.lp = 0;
      opt.pb = 2;
      lzma_filter f[2] = {{comp == C_LZMA ? LZMA_FILTER_LZMA1 : LZMA_FILTER_LZMA2, &opt}, {LZMA_VLI_UNKNOWN, nullptr}};
      lzma_stream s = LZMA_STREAM_INIT;
      CHECK(lzma_raw_encoder(&s, f) == LZMA_OK);
      Bytes out(in.size() * 2 + 1024);
      s.next_in = in.data();
      s.avail_in = in.size();
      s.next_out = out.data();
      s.avail_out = out.size();
      CHECK(lzma_code(&s, LZMA_FINISH) == LZMA_STREAM_END);
      out.resize(out.size() - s.avail_out);
      lzma_end(&s);
      return out;
    }
#endif
#ifdef TORECOMP_HAVE_ZSTD
    case C_ZSTD: {
      Bytes out(ZSTD_compressBound(in.size()));
      out.resize(ZSTD_compress(out.data(), out.size(), in.data(), in.size(), 3));
      return out;
    }
#endif
    default: return {};
  }
}

// Reference lagged Fibonacci generator for the padding encoding. State is the 521 word array.
static Bytes lfg_generate(const Bytes& seed, std::uint64_t skip, std::size_t n) {
  std::vector<std::uint32_t> w(521);
  for (int i = 0; i < 17; ++i) w[i] = (seed[4 * i] << 24) | (seed[4 * i + 1] << 16) | (seed[4 * i + 2] << 8) | seed[4 * i + 3];
  for (int i = 17; i < 521; ++i) w[i] = (w[i - 17] << 23) ^ (w[i - 16] >> 9) ^ w[i - 1];
  auto advance = [&] {
    for (int i = 0; i < 32; ++i) w[i] ^= w[i + 489];
    for (int i = 32; i < 521; ++i) w[i] ^= w[i - 32];
  };
  for (int i = 0; i < 4; ++i) advance();
  Bytes stream;
  const std::size_t words = (skip + n + 3) / 4;
  while (stream.size() / 4 < words) {
    for (int i = 0; i < 521; ++i) {
      stream.push_back(w[i] >> 24);
      stream.push_back(w[i] >> 18);
      stream.push_back(w[i] >> 8);
      stream.push_back(w[i]);
    }
    advance();
  }
  return Bytes(stream.begin() + static_cast<std::ptrdiff_t>(skip), stream.begin() + static_cast<std::ptrdiff_t>(skip + n));
}

struct JunkRun {
  std::uint64_t disc_offset;  // must lie inside one group
  std::size_t length;
  Bytes seed;  // 68 bytes
};

struct WiaOpts {
  bool rvz = false;
  int comp = C_NONE;
  std::uint32_t chunk = 0x200000;
  std::vector<JunkRun> junk;  // RVZ packing
  std::uint32_t disc_type = 1;
  std::uint32_t split = 0;  // second raw-data entry starts here (0: single entry)
};

static Bytes write_wia(const Bytes& img, const WiaOpts& o) {
  struct E { std::uint64_t off, size; };
  std::vector<E> entries;
  if (o.split) {
    entries.push_back({0x80, o.split - 0x80});
    entries.push_back({o.split, img.size() - o.split});
  } else {
    entries.push_back({0x80, img.size() - 0x80});
  }
  Bytes body;  // everything after the headers; file offsets are 0x124 + position
  const std::size_t base = 0x48 + 0xDC;
  auto place = [&](const Bytes& b) {
    while ((base + body.size()) % 4) body.push_back(0);
    const std::size_t at = base + body.size();
    body.insert(body.end(), b.begin(), b.end());
    return at;
  };
  Bytes rawtab, grptab;
  std::uint32_t group_count = 0;
  for (const E& e : entries) {
    const std::uint64_t start = e.off & ~std::uint64_t(0x7FFF), size = e.size + (e.off - start);
    const std::uint32_t n = static_cast<std::uint32_t>((size + o.chunk - 1) / o.chunk);
    be64w(rawtab, e.off);
    be64w(rawtab, e.size);
    be32w(rawtab, group_count);
    be32w(rawtab, n);
    for (std::uint32_t g = 0; g < n; ++g) {
      const std::uint64_t gs = start + std::uint64_t(g) * o.chunk;
      const std::size_t len = static_cast<std::size_t>(std::min<std::uint64_t>(o.chunk, start + size - gs));
      Bytes data = slice(img, static_cast<std::size_t>(gs), len);
      std::uint32_t packed_size = 0;
      Bytes payload;
      bool msb = false;
      if (all_zero(data.data(), data.size())) {
        be32w(grptab, 0);
        be32w(grptab, 0);
        if (o.rvz) be32w(grptab, 0);
        ++group_count;
        continue;
      }
      if (o.rvz) {
        // pack: literal bytes, with the junk runs of this group replaced by seeds
        Bytes packed;
        std::size_t pos = 0;
        for (const JunkRun& j : o.junk) {
          if (j.disc_offset < gs || j.disc_offset >= gs + len) continue;
          const std::size_t at = static_cast<std::size_t>(j.disc_offset - gs);
          if (at > pos) {
            be32w(packed, static_cast<std::uint32_t>(at - pos));
            packed.insert(packed.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.begin() + static_cast<std::ptrdiff_t>(at));
          }
          be32w(packed, static_cast<std::uint32_t>(j.length) | 0x80000000u);
          packed.insert(packed.end(), j.seed.begin(), j.seed.end());
          pos = at + j.length;
        }
        if (!o.junk.empty() && packed.size()) {
          if (pos < len) {
            be32w(packed, static_cast<std::uint32_t>(len - pos));
            packed.insert(packed.end(), data.begin() + static_cast<std::ptrdiff_t>(pos), data.end());
          }
          data = packed;
          packed_size = static_cast<std::uint32_t>(packed.size());
        }
        Bytes z = compress_with(o.comp, data);
        if (o.comp != C_NONE && z.size() < data.size()) {
          payload = z;
          msb = true;
        } else {
          payload = data;  // stored with compression method NONE
        }
      } else {
        payload = compress_with(o.comp, data);
      }
      const std::size_t at = place(payload);
      be32w(grptab, static_cast<std::uint32_t>(at / 4));
      be32w(grptab, static_cast<std::uint32_t>(payload.size()) | (msb ? 0x80000000u : 0));
      if (o.rvz) be32w(grptab, packed_size);
      ++group_count;
    }
  }
  const Bytes rawz = compress_with(o.comp, rawtab), grpz = compress_with(o.comp, grptab);
  const std::size_t raw_off = place(rawz), grp_off = place(grpz);

  Bytes d2;
  be32w(d2, o.disc_type);
  be32w(d2, o.comp);
  be32w(d2, 3);
  be32w(d2, o.chunk);
  d2.insert(d2.end(), img.begin(), img.begin() + 0x80);
  be32w(d2, 0);  // partitions
  be32w(d2, 0x30);
  be64w(d2, 0);
  d2.insert(d2.end(), 20, 0);
  be32w(d2, static_cast<std::uint32_t>(entries.size()));
  be64w(d2, raw_off);
  be32w(d2, static_cast<std::uint32_t>(rawz.size()));
  be32w(d2, group_count);
  be64w(d2, grp_off);
  be32w(d2, static_cast<std::uint32_t>(grpz.size()));
  Bytes props = o.comp == C_LZMA || o.comp == C_LZMA2 ? lzma_props(o.comp) : Bytes{};
  d2.push_back(static_cast<std::uint8_t>(props.size()));
  props.resize(7, 0);
  d2.insert(d2.end(), props.begin(), props.end());

  Bytes out;
  for (char c : std::string(o.rvz ? "RVZ" : "WIA")) out.push_back(static_cast<std::uint8_t>(c));
  out.push_back(1);
  be32w(out, 0x01000000);
  be32w(out, o.rvz ? 0x00030000 : 0x00090000);
  be32w(out, static_cast<std::uint32_t>(d2.size()));
  const Bytes h2 = sha1_bytes(d2);
  out.insert(out.end(), h2.begin(), h2.end());
  be64w(out, img.size());
  be64w(out, base + body.size());
  const Bytes h1 = sha1_bytes(out);
  out.insert(out.end(), h1.begin(), h1.end());
  out.insert(out.end(), d2.begin(), d2.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

static Bytes read_all(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  return Bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Every observable property of the reader must match the plain image.
static void check_same_as_plain(const fs::path& plain, const fs::path& other, const char* format, bool same_size) {
  gcm::Image a(plain), b(other);
  CHECK(std::string(b.format()) == format);
  CHECK(a.game_id() == b.game_id());
  CHECK(a.title() == b.title());
  CHECK(a.disc_number() == b.disc_number());
  CHECK(a.files().size() == b.files().size());
  if (same_size) {
    CHECK(a.size() == b.size());
    CHECK(hash_whole_image(a) == hash_whole_image(b));
    // random access, including reads that straddle chunk and block boundaries
    for (std::uint64_t off : {0ull, 1ull, 0x7Full, 0x80ull, 0x7FFFull, 0x8000ull, 0x3800ull, 0x4000ull}) {
      Bytes x(0x9000), y(0x9000);
      if (off + x.size() > a.size()) continue;
      a.read(off, x.data(), x.size());
      b.read(off, y.data(), y.size());
      CHECK(x == y);
    }
  }
  CHECK(build_manifest(a).content_sha1 == build_manifest(b).content_sha1);
}

static std::string describe(const char* what, int comp, std::uint32_t chunk) {
  return std::string(what) + " comp=" + std::to_string(comp) + " chunk=" + std::to_string(chunk);
}

static void test_compressed_images(const Temp& t) {
  const Node tree = sample_tree();
  // 0xAA padding: every block has data. A second image with zero padding gives sparse files.
  Bytes dense = build_image(tree, 0xAA);
  Bytes sparse = build_image(tree, 0);
  // Padding written with the lagged Fibonacci generator, as on real discs (only inside the
  // gap between main.dol and the FST, which is not part of any file).
  JunkRun junk{0x3800, 0x400, Bytes(68)};
  for (std::size_t i = 0; i < 68; ++i) junk.seed[i] = static_cast<std::uint8_t>(i * 37 + 11);
  const Bytes stream = lfg_generate(junk.seed, 0x3800 % 0x8000, junk.length);
  std::copy(stream.begin(), stream.end(), dense.begin() + 0x3800);
  CHECK(!all_zero(stream.data(), stream.size()));

  const fs::path plain_dense = t.write("dense.iso", dense);
  const fs::path plain_sparse = t.write("sparse.iso", sparse);

  // CISO
  for (std::uint32_t block : {0x8000u, 0x10000u, 0x200000u}) {
    const fs::path p = t.write("d.ciso", write_ciso(dense, block));
    check_same_as_plain(plain_dense, p, "CISO", true);
    const fs::path q = t.write("s.ciso", write_ciso(sparse, block));
    check_same_as_plain(plain_sparse, q, "CISO", false);  // trailing zero blocks are not stored
  }
  {
    Bytes bad = write_ciso(dense, 0x8000);
    bad[4] = 3;  // block size 3
    CHECK_THROWS(gcm::Image(t.write("bad.ciso", bad)));
    Bytes cut = write_ciso(dense, 0x8000);
    cut.resize(cut.size() - 0x9000);
    CHECK_THROWS(gcm::Image(t.write("cut.ciso", cut)));
  }

#ifdef TORECOMP_HAVE_ZLIB
  for (std::uint32_t block : {0x8000u, 0x4000u}) {
    const fs::path p = t.write("d.gcz", write_gcz(dense, block));
    check_same_as_plain(plain_dense, p, "GCZ", true);
    const fs::path q = t.write("s.gcz", write_gcz(sparse, block));
    check_same_as_plain(plain_sparse, q, "GCZ", true);
  }
  {
    Bytes g = write_gcz(dense, 0x8000);
    g[g.size() - 100] ^= 0x55;  // corrupt block data: the Adler-32 check must catch it
    gcm::Image* img = nullptr;
    bool threw = false;
    try {
      img = new gcm::Image(t.write("bad.gcz", g));
      hash_whole_image(*img);
    } catch (const Error&) { threw = true; }
    delete img;
    CHECK(threw);
    Bytes cut = write_gcz(dense, 0x8000);
    cut.resize(cut.size() / 2);
    CHECK_THROWS(gcm::Image(t.write("cut.gcz", cut)));
  }
#else
  std::puts("note: GCZ not tested (no zlib)");
#endif

  // WIA and RVZ
  for (bool rvz : {false, true}) {
    for (int comp = C_NONE; comp <= C_ZSTD; ++comp) {
      if (rvz && comp == C_PURGE) continue;
      if (!rvz && comp == C_ZSTD) continue;
      if (!comp_available(comp)) {
        std::printf("note: %s compression %d not tested (library missing)\n", rvz ? "RVZ" : "WIA", comp);
        continue;
      }
      for (std::uint32_t chunk : rvz ? std::vector<std::uint32_t>{0x8000, 0x10000, 0x200000}
                                     : std::vector<std::uint32_t>{0x200000, 0x400000}) {
        for (std::uint32_t split : {0u, 0x10000u}) {
          WiaOpts o;
          o.rvz = rvz;
          o.comp = comp;
          o.chunk = chunk;
          o.split = split;
          if (rvz) o.junk.push_back(junk);
          const std::string label = describe(rvz ? "RVZ" : "WIA", comp, chunk);
          const fs::path p = t.write("d.wia", write_wia(dense, o));
          try {
            check_same_as_plain(plain_dense, p, rvz ? "RVZ" : "WIA", true);
            o.junk.clear();  // the sparse image has no generated padding
            const fs::path q = t.write("s.wia", write_wia(sparse, o));
            check_same_as_plain(plain_sparse, q, rvz ? "RVZ" : "WIA", true);
          } catch (const Error& e) {
            std::fprintf(stderr, "%s: unexpected error: %s\n", label.c_str(), e.what());
            ++g_failures;
          }
        }
      }
    }
  }

  // Malformed WIA/RVZ files must be rejected with torecomp::Error, never crash.
  {
    WiaOpts o;
    o.comp = C_PURGE;
    const Bytes good = write_wia(dense, o);
    Bytes b = good;
    b.resize(b.size() - 1);
    CHECK_THROWS(gcm::Image(t.write("cut.wia", b)));
    b = good;
    b[0x20] ^= 1;  // header checksum
    CHECK_THROWS(gcm::Image(t.write("hdr.wia", b)));
    b = good;
    b[0x124 + 3] ^= 0xFF;  // group data: PURGE checksum (hit when the group is read)
    {
      bool threw = false;
      try {
        gcm::Image img(t.write("grp.wia", b));
        hash_whole_image(img);
      } catch (const Error&) { threw = true; }
      CHECK(threw);
    }
    WiaOpts w;
    w.disc_type = 2;
    CHECK_THROWS(gcm::Image(t.write("wii.wia", write_wia(dense, w))));
    WiaOpts c;
    c.chunk = 0x1234;
    CHECK_THROWS(gcm::Image(t.write("chunk.wia", write_wia(dense, c))));
    WiaOpts z;
    z.rvz = true;
    z.comp = C_PURGE;
    CHECK_THROWS(gcm::Image(t.write("purge.rvz", write_wia(dense, z))));
  }

  // Bit flips anywhere in a container: either still readable or torecomp::Error.
  {
    std::vector<Bytes> files;
    files.push_back(write_ciso(dense, 0x8000));
    WiaOpts a;
    a.comp = C_PURGE;
    files.push_back(write_wia(dense, a));
    WiaOpts r;
    r.rvz = true;
    r.chunk = 0x8000;
    r.junk.push_back(junk);
    files.push_back(write_wia(dense, r));
#ifdef TORECOMP_HAVE_ZLIB
    files.push_back(write_gcz(dense, 0x8000));
#endif
    std::uint32_t seed = 12345;
    for (const Bytes& f : files) {
      const std::size_t limit = std::min<std::size_t>(f.size(), 0x400);  // headers and tables
      for (int i = 0; i < 300; ++i) {
        seed = seed * 1664525u + 1013904223u;
        Bytes b = f;
        const std::size_t at = (i < 150 ? seed % limit : seed % f.size());
        b[at] ^= static_cast<std::uint8_t>(1u << (seed >> 29));
        try {
          gcm::Image img(t.write("fuzz.bin", b));
          hash_whole_image(img);
        } catch (const Error&) {
        }
      }
    }
  }

  // Optional manual check against a real disc: TOREC_TEST_ISO=/path/game.iso converts it with the
  // writers above and compares content hashes and whole-image hashes (needs ~3x its size in RAM).
  if (const char* iso = std::getenv("TOREC_TEST_ISO")) {
    const Bytes big = read_all(iso);
    const fs::path ref = iso;
    gcm::Image a(ref);
    const std::string content = build_manifest(a).content_sha1;
    const std::string whole = hash_whole_image(a);
    auto compare = [&](const char* name, const Bytes& data, const char* format) {
      const fs::path p = t.write(std::string("real.") + name, data);
      gcm::Image b(p);
      const bool ok = std::string(b.format()) == format && build_manifest(b).content_sha1 == content &&
                      hash_whole_image(b) == whole;
      std::printf("real disc as %s: %s\n", name, ok ? "identical" : "DIFFERENT");
      CHECK(ok);
      fs::remove(p);
    };
    compare("ciso", write_ciso(big, 0x200000), "CISO");
#ifdef TORECOMP_HAVE_ZLIB
    compare("gcz", write_gcz(big, 0x8000), "GCZ");
#endif
    WiaOpts w;
    w.comp = C_PURGE;
    compare("wia-purge", write_wia(big, w), "WIA");
#ifdef TORECOMP_HAVE_ZSTD
    WiaOpts r;
    r.rvz = true;
    r.comp = C_ZSTD;
    compare("rvz-zstd", write_wia(big, r), "RVZ");
#endif
  }
}

int main() {
  Temp t;
  test_sha1();
  test_parse(t);
  test_malformed(t);
  test_content_hash_ignores_padding(t);
  test_identify(t);
  test_database_rejects_bad_input();
  test_extract(t);
  test_sanitize();
  test_plan_collisions();
  test_extract_windows_unsafe_names(t);
  test_extract_never_escapes(t);
  test_extract_long_path(t);
  test_extract_refuses_overwrite(t);
  test_compressed_images(t);
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("all tests passed");
  return 0;
}
