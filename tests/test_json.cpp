// Json::parse: nesting, escapes (incl. \u surrogates), numbers, error positions, lookups.
#include "engine/core/json.h"
#include "tests/check.h"

using namespace eng;

static Json ok(const char* text) { std::string err; Json j = Json::parse(text, &err); CHECK_MSG(err.empty(), err); return j; }
static std::string bad(const char* text) { std::string err; Json j = Json::parse(text, &err); CHECK_MSG(!err.empty(), text); CHECK(j.isNull()); return err; }

TEST_MAIN({
  // nesting + lookups
  Json j = ok(R"({"a":{"b":[1,2,{"c":[[],{}]}]},"n":null,"t":true,"f":false,"s":"x"})");
  CHECK(j.isObject()); CHECK(j.size() == 5);
  CHECK(j["a"]["b"].isArray()); CHECK(j["a"]["b"].size() == 3);
  CHECK(j["a"]["b"][2]["c"][0].isArray()); CHECK(j["a"]["b"][2]["c"][0].size() == 0);
  CHECK(j["a"]["b"][2]["c"][1].isObject());
  CHECK(j["n"].isNull()); CHECK(j["t"].boolOr(false)); CHECK(!j["f"].boolOr(true));
  CHECK(j["a"]["b"][1].intOr(-1) == 2); CHECK(j["a"]["b"][1].numberOr(-1) == 2);
  CHECK(j.has("a")); CHECK(!j.has("zz")); CHECK(!j["a"].has("zz"));
  // misses are safe and chain
  CHECK(j["missing"]["deep"][7]["x"].isNull());
  CHECK(j["a"]["b"][99].isNull());
  CHECK(j["s"][0].isNull());   // indexing a string
  CHECK(j["a"]["b"]["notakey"].isNull());   // string key on an array
  // *Or by value: defaults returned when the type does not match
  CHECK(j["s"].stringOr("d") == "x");
  CHECK(j["n"].stringOr("d") == "d"); CHECK(j["t"].stringOr("d") == "d"); CHECK(j["missing"].stringOr("d") == "d");
  CHECK(j["s"].numberOr(3.5) == 3.5); CHECK(j["s"].intOr(4) == 4); CHECK(j["s"].boolOr(true));
  { std::string d = "default"; std::string r = j["missing"].stringOr(d); d = "changed"; CHECK(r == "default"); }
  // object key ordering does not matter; duplicate keys keep the first (std::map emplace)
  Json dup = ok(R"({"k":1,"k":2})"); CHECK(dup["k"].intOr(0) == 1);

  // escapes
  Json e = ok(R"(["a\"b", "back\\slash", "sl\/ash", "\b\f\n\r\t", "\u0041\u00e9", "\u20ac", "\ud83d\ude00", "\u0000x"])");
  CHECK(e[0].str == "a\"b"); CHECK(e[1].str == "back\\slash"); CHECK(e[2].str == "sl/ash");
  CHECK(e[3].str == "\b\f\n\r\t");
  CHECK(e[4].str == "A\xC3\xA9");                 // 2-byte UTF-8
  CHECK(e[5].str == "\xE2\x82\xAC");              // 3-byte (euro)
  CHECK(e[6].str == "\xF0\x9F\x98\x80");          // surrogate pair -> U+1F600 (4 bytes)
  CHECK(e[7].str.size() == 2 && e[7].str[0] == 0 && e[7].str[1] == 'x');
  Json raw = ok("\"caf\xC3\xA9 \xE2\x82\xAC\""); CHECK(raw.str == "caf\xC3\xA9 \xE2\x82\xAC");   // raw UTF-8 passes through

  // numbers
  Json n = ok("[0, -0, 1, -17, 3.25, -0.5, 1e3, 1E-2, -2.5e+2, 12345678901234, 1.7976931348623157e308, 0.1]");
  CHECK(n[0].num == 0); CHECK(n[2].num == 1); CHECK(n[3].num == -17); CHECK(n[4].num == 3.25); CHECK(n[5].num == -0.5);
  CHECK(n[6].num == 1000); CHECK(n[7].num == 0.01); CHECK(n[8].num == -250); CHECK(n[9].num == 12345678901234.0);
  CHECK(n[10].num == 1.7976931348623157e308); CHECK(n[11].num == 0.1);
  CHECK(n[3].intOr(0) == -17); CHECK(n[4].intOr(0) == 3);   // truncation
  CHECK(std::signbit(n[1].num));
  // number directly followed by structural chars
  Json n2 = ok("{\"a\":1,\"b\":2}"); CHECK(n2["a"].num == 1 && n2["b"].num == 2);
  Json n3 = ok("[1,2]"); CHECK(n3.size() == 2 && n3[1].num == 2);

  // whitespace
  Json w = ok(" \n\t\r{ \"a\" :\n[ 1 , 2 ] \t}\n"); CHECK(w["a"].size() == 2);
  CHECK(ok("  42 ").num == 42); CHECK(ok("\"s\"").isString()); CHECK(ok("[]").size() == 0); CHECK(ok("{}").isObject());

  // errors with positions
  std::string err;
  err = bad("");                    CHECK(err.find("unexpected end") != std::string::npos);
  err = bad("{\"a\":1");            CHECK(err.find("at 6") != std::string::npos);
  err = bad("[1,2");                CHECK(err.find("unexpected end in array") != std::string::npos);
  err = bad("{\"a\" 1}");           CHECK(err.find("expected ':'") != std::string::npos); CHECK(err.find("at 5") != std::string::npos);
  err = bad("{a:1}");               CHECK(err.find("expected string") != std::string::npos); CHECK(err.find("at 1") != std::string::npos);
  err = bad("[1 2]");               CHECK(err.find("expected ',' or ']'") != std::string::npos); CHECK(err.find("at 3") != std::string::npos);
  err = bad("{\"a\":1 \"b\":2}");   CHECK(err.find("expected ',' or '}'") != std::string::npos); CHECK(err.find("at 7") != std::string::npos);
  err = bad("\"abc");               CHECK(err.find("unterminated string") != std::string::npos);
  err = bad("\"a\\qb\"");           CHECK(err.find("bad escape") != std::string::npos); CHECK(err.find("at 4") != std::string::npos);
  err = bad("\"\\u12\"");           CHECK(err.find("bad \\u") != std::string::npos);
  err = bad("@");                   CHECK(err.find("unexpected character at 0") != std::string::npos);
  err = bad("[1, ?]");              CHECK(err.find("at 4") != std::string::npos);
  err = bad("{\"deep\":[[[{\"x\":}]]]}"); CHECK(err.find("at 16") != std::string::npos);
  // error pointer is optional
  CHECK(Json::parse("[", nullptr).isNull());
})
