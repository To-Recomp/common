// SPDX-License-Identifier: GPL-3.0-or-later
// Tests use small synthetic GameCube images built in memory. No game data is involved.
#include <cstdio>
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
  if (g_failures) {
    std::fprintf(stderr, "%d check(s) failed\n", g_failures);
    return 1;
  }
  std::puts("all tests passed");
  return 0;
}
