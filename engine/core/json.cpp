#include "engine/core/json.h"
#include <cstdlib>
#include <cstring>

namespace eng {

static const Json NULL_JSON;

const Json& Json::operator[](std::string_view key) const {
  if (type != Type::Object) return NULL_JSON;
  auto it = obj.find(std::string(key));
  return it == obj.end() ? NULL_JSON : it->second;
}
const Json& Json::operator[](size_t i) const {
  if (type != Type::Array || i >= arr.size()) return NULL_JSON;
  return arr[i];
}

namespace {
struct Parser {
  std::string_view s; size_t p; std::string err;

  void ws() { while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p; }
  bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(p); return false; }

  bool parseString(std::string& out) {
    if (p >= s.size() || s[p] != '"') return fail("expected string");
    ++p;
    while (p < s.size()) {
      char c = s[p++];
      if (c == '"') return true;
      if (c != '\\') { out += c; continue; }
      if (p >= s.size()) return fail("bad escape");
      char e = s[p++];
      switch (e) {
        case '"': out += '"'; break; case '\\': out += '\\'; break; case '/': out += '/'; break;
        case 'b': out += '\b'; break; case 'f': out += '\f'; break; case 'n': out += '\n'; break;
        case 'r': out += '\r'; break; case 't': out += '\t'; break;
        case 'u': {
          if (p + 4 > s.size()) return fail("bad \\u");
          unsigned cp = (unsigned)std::strtoul(std::string(s.substr(p, 4)).c_str(), nullptr, 16); p += 4;
          if (cp >= 0xD800 && cp <= 0xDBFF && p + 6 <= s.size() && s[p] == '\\' && s[p + 1] == 'u') {
            unsigned lo = (unsigned)std::strtoul(std::string(s.substr(p + 2, 4)).c_str(), nullptr, 16); p += 6;
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
          }
          if (cp < 0x80) out += (char)cp;
          else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
          else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
          else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
          break;
        }
        default: return fail("bad escape");
      }
    }
    return fail("unterminated string");
  }

  bool parseValue(Json& v) {
    ws();
    if (p >= s.size()) return fail("unexpected end");
    char c = s[p];
    if (c == '{') {
      v.type = Json::Type::Object; ++p; ws();
      if (p < s.size() && s[p] == '}') { ++p; return true; }
      for (;;) {
        ws(); std::string k; if (!parseString(k)) return false;
        ws(); if (p >= s.size() || s[p] != ':') return fail("expected ':'"); ++p;
        Json child; if (!parseValue(child)) return false;
        v.obj.emplace(std::move(k), std::move(child));
        ws(); if (p >= s.size()) return fail("unexpected end in object");
        if (s[p] == ',') { ++p; continue; }
        if (s[p] == '}') { ++p; return true; }
        return fail("expected ',' or '}'");
      }
    }
    if (c == '[') {
      v.type = Json::Type::Array; ++p; ws();
      if (p < s.size() && s[p] == ']') { ++p; return true; }
      for (;;) {
        Json child; if (!parseValue(child)) return false;
        v.arr.push_back(std::move(child));
        ws(); if (p >= s.size()) return fail("unexpected end in array");
        if (s[p] == ',') { ++p; continue; }
        if (s[p] == ']') { ++p; return true; }
        return fail("expected ',' or ']'");
      }
    }
    if (c == '"') { v.type = Json::Type::String; return parseString(v.str); }
    if (s.compare(p, 4, "true") == 0) { v.type = Json::Type::Bool; v.b = true; p += 4; return true; }
    if (s.compare(p, 5, "false") == 0) { v.type = Json::Type::Bool; v.b = false; p += 5; return true; }
    if (s.compare(p, 4, "null") == 0) { v.type = Json::Type::Null; p += 4; return true; }
    if (c == '-' || (c >= '0' && c <= '9')) {
      v.type = Json::Type::Number;
      char buf[64]; size_t n = 0;
      while (p < s.size() && n < 63 && (std::strchr("+-.eE0123456789", s[p]))) buf[n++] = s[p++];
      buf[n] = 0; v.num = std::strtod(buf, nullptr); return true;
    }
    return fail("unexpected character");
  }
};
} // namespace

Json Json::parse(std::string_view text, std::string* error) {
  Parser ps{text, 0, {}}; Json v;
  if (!ps.parseValue(v)) { if (error) *error = ps.err; return Json{}; }
  return v;
}

} // namespace eng
