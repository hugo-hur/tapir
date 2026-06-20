// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// test_cli.cpp — unit tests for the shared CLI argument parser (cli.cpp):
// parse_tape_opts (common -b/-m/-v + positional device + extra-arg callback) and
// parse_int_list. Device-free, exercises the library directly. Supersedes the old
// test_cli_flags.sh, which could only spawn the binaries and grep the error path —
// here we also cover the happy paths (device, flags, callback). Run via: make check

#include "cli.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace tapir;

static int g_pass = 0, g_fail = 0;
#define CHECK(expr)                                                              \
    do {                                                                         \
        if (expr) { ++g_pass; }                                                  \
        else { ++g_fail;                                                         \
               std::fprintf(stderr, "FAIL: %s  (%s:%d)\n", #expr, __FILE__, __LINE__); } \
    } while (0)

// Build a mutable, NUL-terminated argv (parse_tape_opts takes char**, like main).
struct Argv
{
    std::vector<std::string> store;
    std::vector<char *> ptrs;
    Argv(std::initializer_list<const char *> a)
    {
        for (const char *s : a) store.emplace_back(s);
        for (auto &s : store) ptrs.push_back(s.data());
        ptrs.push_back(nullptr);
    }
    int argc() const { return static_cast<int>(store.size()); }
    char **argv() { return ptrs.data(); }
};

static void test_defaults_and_device()
{
    std::puts("-- parse_tape_opts: positional device + defaults");
    Argv a{"prog", "/dev/nst0"};
    TapeOpts o;
    CHECK(parse_tape_opts(a.argc(), a.argv(), 1, o, "prog"));
    CHECK(o.device == "/dev/nst0");
    CHECK(o.bf == 512 && o.mbf == 512 && o.verbose == false);
}

static void test_short_flags()
{
    std::puts("-- parse_tape_opts: short -b/-m/-v");
    Argv a{"prog", "/dev/x", "-b", "128", "-m", "64", "-v"};
    TapeOpts o;
    CHECK(parse_tape_opts(a.argc(), a.argv(), 1, o, "prog"));
    CHECK(o.device == "/dev/x");
    CHECK(o.bf == 128);
    CHECK(o.mbf == 64);
    CHECK(o.verbose == true);
}

static void test_long_flags()
{
    std::puts("-- parse_tape_opts: long flags, device after the flags");
    Argv a{"prog", "--block-factor", "256", "--manifest-block-factor", "32", "--verbose", "/dev/y"};
    TapeOpts o;
    CHECK(parse_tape_opts(a.argc(), a.argv(), 1, o, "prog"));
    CHECK(o.bf == 256);
    CHECK(o.mbf == 32);
    CHECK(o.verbose == true);
    CHECK(o.device == "/dev/y");
}

static void test_unknown_flag_rejected()
{
    std::puts("-- parse_tape_opts: unknown flag -> false (prints to stderr)");
    Argv a{"prog", "/dev/x", "--bogus-flag"};
    TapeOpts o;
    CHECK(parse_tape_opts(a.argc(), a.argv(), 1, o, "prog") == false);
}

static void test_extra_callback_flag()
{
    std::puts("-- parse_tape_opts: extra callback handles --force");
    bool force = false;
    auto extra = [&](const char *arg, int &, int, char **) -> bool {
        if (std::strcmp(arg, "--force") == 0) { force = true; return true; }
        return false;
    };
    Argv a{"prog", "/dev/x", "--force"};
    TapeOpts o;
    CHECK(parse_tape_opts(a.argc(), a.argv(), 1, o, "prog", extra));
    CHECK(force);
    CHECK(o.device == "/dev/x");
}

static void test_extra_callback_consumes_value()
{
    std::puts("-- parse_tape_opts: extra callback consumes a value, parsing continues");
    std::string captured;
    auto extra = [&](const char *arg, int &i, int argc, char **argv) -> bool {
        if (std::strcmp(arg, "-f") == 0 && i + 1 < argc) { captured = argv[++i]; return true; }
        return false;
    };
    // Mimic a subcommand: device came from argv[2], parsing starts at 3.
    Argv a{"prog", "import", "/dev/x", "-f", "0,2,5", "-m", "64"};
    TapeOpts o;
    o.device = "/dev/x";
    CHECK(parse_tape_opts(a.argc(), a.argv(), 3, o, "prog", extra));
    CHECK(captured == "0,2,5");
    CHECK(o.mbf == 64); // -m after the consumed -f value still parsed
}

static void test_parse_int_list()
{
    std::puts("-- parse_int_list: comma lists, empty tokens, trailing comma");
    CHECK((parse_int_list("0,2,5") == std::vector<int>{0, 2, 5}));
    CHECK((parse_int_list("3") == std::vector<int>{3}));
    CHECK(parse_int_list("").empty());
    CHECK((parse_int_list("1,,2") == std::vector<int>{1, 2}));
    CHECK((parse_int_list("0,2,") == std::vector<int>{0, 2}));
}

int main()
{
    test_defaults_and_device();
    test_short_flags();
    test_long_flags();
    test_unknown_flag_rejected();
    test_extra_callback_flag();
    test_extra_callback_consumes_value();
    test_parse_int_list();
    std::printf("=== %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
