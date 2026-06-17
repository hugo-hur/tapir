// test_security.cpp — unit tests for tapir::security::UuidV4.
// Run via: make check

#include "security.hpp"

#include <array>
#include <cassert>
#include <cstdio>
#include <set>
#include <stdexcept>
#include <string>

using namespace tapir::security;

static int g_pass = 0, g_fail = 0;

#define CHECK(expr)                                                              \
    do {                                                                         \
        if (expr) { ++g_pass; }                                                  \
        else { ++g_fail;                                                         \
               std::fprintf(stderr, "FAIL: %s  (%s:%d)\n",                      \
                            #expr, __FILE__, __LINE__); }                        \
    } while (0)

#define CHECK_THROWS(expr)                                                       \
    do {                                                                         \
        try { (void)(expr);                                                      \
              ++g_fail;                                                          \
              std::fprintf(stderr, "FAIL (no throw): %s  (%s:%d)\n",            \
                           #expr, __FILE__, __LINE__); }                         \
        catch (const std::exception &) { ++g_pass; }                            \
    } while (0)

// ── helpers ───────────────────────────────────────────────────────────────────

static bool is_valid_uuid_string(const std::string &s)
{
    if (s.size() != 36) return false;
    if (s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-') return false;
    for (int i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) continue;
        char c = s[i];
        bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        if (!hex) return false;
    }
    return true;
}

static bool has_v4_markers(const std::string &s)
{
    // version nibble is s[14], must be '4'
    if (s[14] != '4') return false;
    // variant nibble is s[19], must be 8, 9, a, or b (10xx in binary)
    char v = s[19];
    return v == '8' || v == '9' || v == 'a' || v == 'b';
}

// ── tests ─────────────────────────────────────────────────────────────────────

static void test_generate_format()
{
    std::puts("-- generate: string format");
    UuidV4 u;
    std::string s = u;
    CHECK(is_valid_uuid_string(s));
    CHECK(has_v4_markers(s));
}

static void test_generate_lowercase()
{
    std::puts("-- generate: all hex chars are lowercase");
    for (int i = 0; i < 50; ++i) {
        std::string s = UuidV4();
        for (char c : s) {
            if (c == '-') continue;
            CHECK(!(c >= 'A' && c <= 'F'));
        }
    }
}

static void test_generate_uniqueness()
{
    std::puts("-- generate: no duplicate in 1000 UUIDs");
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        std::string s = UuidV4();
        CHECK(seen.find(s) == seen.end());
        seen.insert(s);
    }
}

static void test_parse_roundtrip()
{
    std::puts("-- parse: lowercase round-trip");
    const std::string known = "550e8400-e29b-41d4-a716-446655440000";
    UuidV4 u(known);
    std::string s = u;
    CHECK(s == known);
}

static void test_parse_uppercase_normalises()
{
    std::puts("-- parse: uppercase input normalises to lowercase");
    UuidV4 u("550E8400-E29B-41D4-A716-446655440000");
    std::string s = u;
    CHECK(s == "550e8400-e29b-41d4-a716-446655440000");
}

static void test_parse_mixed_case()
{
    std::puts("-- parse: mixed-case input");
    UuidV4 u("F47AC10b-58Cc-4372-A567-0E02b2c3d479");
    std::string s = u;
    CHECK(s == "f47ac10b-58cc-4372-a567-0e02b2c3d479");
}

static void test_parse_preserves_bytes()
{
    std::puts("-- parse: string constructor does not mangle version/variant");
    // This UUID has a non-standard version nibble (5, not 4). The string
    // constructor must store it verbatim — it is a parser, not a generator.
    const std::string v5 = "550e8400-e29b-51d4-a716-446655440000";
    UuidV4 u(v5);
    std::string s = u;
    CHECK(s == v5);
}

static void test_parse_invalid_length()
{
    std::puts("-- parse: wrong length throws");
    CHECK_THROWS(UuidV4("550e8400-e29b-41d4-a716-44665544000"));   // 35 chars
    CHECK_THROWS(UuidV4("550e8400-e29b-41d4-a716-4466554400000"));  // 37 chars
    CHECK_THROWS(UuidV4(""));
}

static void test_parse_invalid_dashes()
{
    std::puts("-- parse: wrong dash positions throw");
    CHECK_THROWS(UuidV4("550e8400Xe29b-41d4-a716-446655440000")); // dash 0 wrong
    CHECK_THROWS(UuidV4("550e8400-e29bX41d4-a716-446655440000")); // dash 1 wrong
    CHECK_THROWS(UuidV4("550e8400-e29b-41d4Xa716-446655440000")); // dash 2 wrong
    CHECK_THROWS(UuidV4("550e8400-e29b-41d4-a716X446655440000")); // dash 3 wrong
}

static void test_parse_invalid_hex()
{
    std::puts("-- parse: non-hex characters throw");
    CHECK_THROWS(UuidV4("550e8400-e29b-41d4-a716-44665544000g")); // 'g' invalid
    CHECK_THROWS(UuidV4("550e8400-e29b-41d4-a716-44665544000 ")); // space invalid
    CHECK_THROWS(UuidV4("550e8400-e29b-41d4-a716-44665544000Z")); // 'Z' invalid
}

static void test_bytes_ctor_version_variant()
{
    std::puts("-- bytes ctor: version and variant bits are stamped");
    std::array<std::byte, 16> zeros{};
    UuidV4 u(zeros);
    std::string s = u;
    CHECK(is_valid_uuid_string(s));
    CHECK(s[14] == '4');                                     // version 4
    char v = s[19];
    CHECK(v == '8' || v == '9' || v == 'a' || v == 'b');   // variant 1
    // All other fields should be zero since input was all-zeros.
    CHECK(s == "00000000-0000-4000-8000-000000000000");
}

static void test_bytes_ctor_known()
{
    std::puts("-- bytes ctor: known byte pattern produces expected string");
    std::array<std::byte, 16> b{};
    b[0] = std::byte(0xDE); b[1] = std::byte(0xAD);
    b[2] = std::byte(0xBE); b[3] = std::byte(0xEF);
    // bytes[6] and [8] will have version/variant forced; rest stays as-is.
    UuidV4 u(b);
    std::string s = u;
    CHECK(s.substr(0, 8) == "deadbeef");
    CHECK(s[14] == '4');
    char v = s[19];
    CHECK(v == '8' || v == '9' || v == 'a' || v == 'b');
}

static void test_to_bytes_roundtrip()
{
    std::puts("-- to-bytes: parse → bytes → values match");
    // f47ac10b-58cc-4372-a567-0e02b2c3d479 is a well-known v4 UUID.
    UuidV4 u("f47ac10b-58cc-4372-a567-0e02b2c3d479");
    std::array<std::byte, 16> b = u;
    CHECK(std::to_integer<uint8_t>(b[0]) == 0xf4);
    CHECK(std::to_integer<uint8_t>(b[1]) == 0x7a);
    CHECK(std::to_integer<uint8_t>(b[2]) == 0xc1);
    CHECK(std::to_integer<uint8_t>(b[3]) == 0x0b);
    CHECK(std::to_integer<uint8_t>(b[4]) == 0x58);
    CHECK(std::to_integer<uint8_t>(b[5]) == 0xcc);
    CHECK(std::to_integer<uint8_t>(b[6]) == 0x43);
    CHECK(std::to_integer<uint8_t>(b[7]) == 0x72);
    CHECK(std::to_integer<uint8_t>(b[8]) == 0xa5);
    CHECK(std::to_integer<uint8_t>(b[9]) == 0x67);
    CHECK(std::to_integer<uint8_t>(b[10]) == 0x0e);
    // byte 6 high nibble encodes version: 4
    CHECK((std::to_integer<uint8_t>(b[6]) >> 4) == 4);
    // byte 8 top two bits encode variant: 10
    CHECK((std::to_integer<uint8_t>(b[8]) >> 6) == 2);
}

static void test_generated_uuids_have_v4_markers()
{
    std::puts("-- generate: 100 UUIDs all have correct version/variant markers");
    for (int i = 0; i < 100; ++i) {
        std::string s = UuidV4();
        CHECK(has_v4_markers(s));
    }
}

// ── main ──────────────────────────────────────────────────────────────────────

int main()
{
    std::puts("=== test_security: UuidV4 ===");

    test_generate_format();
    test_generate_lowercase();
    test_generate_uniqueness();
    test_parse_roundtrip();
    test_parse_uppercase_normalises();
    test_parse_mixed_case();
    test_parse_preserves_bytes();
    test_parse_invalid_length();
    test_parse_invalid_dashes();
    test_parse_invalid_hex();
    test_bytes_ctor_version_variant();
    test_bytes_ctor_known();
    test_to_bytes_roundtrip();
    test_generated_uuids_have_v4_markers();

    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
