// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

// cli.hpp — shared argv parsing helpers for the admin tools (tfsck, mktapir).
//
// Both binaries share a common set of flags (-b, -m, -v) and the same positional
// convention (first non-flag arg is the tape device).  parse_tape_opts handles
// those, calling an optional extra-arg callback for subcommand-specific flags.

#ifndef TAPIR_CLI_HPP
#define TAPIR_CLI_HPP

#include <functional>
#include <string>
#include <vector>

namespace tapir
{
    // Common options present across every admin command.
    struct TapeOpts
    {
        std::string device;
        int bf      = 512;
        int mbf     = 512;
        bool verbose = false;
    };

    // extra(arg, i, argc, argv) is called for any unrecognised argument.
    // It may advance i to consume a following value. Return true to accept,
    // false to fall through to the "unknown option" error.
    using ExtraArgFn = std::function<bool(const char *arg, int &i, int argc, char **argv)>;

    // Scan argv[start..argc), filling out with:
    //   first non-flag argument      → device (when device is still empty)
    //   -b / --block-factor N        → bf
    //   -m / --manifest-block-factor → mbf
    //   -v / --verbose               → verbose
    // Unrecognised arguments are passed to extra; if extra is empty or rejects
    // them, prints "prog: unknown option: X\n" and returns false.
    bool parse_tape_opts(int argc, char **argv, int start, TapeOpts &out,
                         const char *prog, const ExtraArgFn &extra = {});

    // Parse a comma-separated list of non-negative integers ("0,2,5" → {0,2,5}).
    std::vector<int> parse_int_list(const char *s);

} // namespace tapir

#endif // TAPIR_CLI_HPP
