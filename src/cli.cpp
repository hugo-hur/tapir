// SPDX-License-Identifier: LGPL-2.1-or-later
// Copyright (C) 2026 Hugo Hurskainen

#include "cli.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace tapir
{
    bool parse_tape_opts(int argc, char **argv, int start, TapeOpts &out,
                         const char *prog, const ExtraArgFn &extra)
    {
        for (int i = start; i < argc; ++i)
        {
            const char *a = argv[i];
            if ((std::strcmp(a, "-b") == 0 || std::strcmp(a, "--block-factor") == 0) && i + 1 < argc)
                out.bf = std::atoi(argv[++i]);
            else if ((std::strcmp(a, "-m") == 0 || std::strcmp(a, "--manifest-block-factor") == 0) && i + 1 < argc)
                out.mbf = std::atoi(argv[++i]);
            else if (std::strcmp(a, "-v") == 0 || std::strcmp(a, "--verbose") == 0)
                out.verbose = true;
            else if (a[0] != '-' && out.device.empty())
                out.device = a;
            else if (extra && extra(a, i, argc, argv))
                /* consumed by callback */;
            else
            {
                std::fprintf(stderr, "%s: unknown option: %s\n", prog, a);
                return false;
            }
        }
        return true;
    }

    std::vector<int> parse_int_list(const char *s)
    {
        std::vector<int> result;
        const std::string str(s);
        for (std::size_t p = 0; p < str.size();)
        {
            const std::size_t c = str.find(',', p);
            const std::string tok = str.substr(p, c == std::string::npos ? c : c - p);
            if (!tok.empty())
                result.push_back(std::atoi(tok.c_str()));
            if (c == std::string::npos) break;
            p = c + 1;
        }
        return result;
    }

} // namespace tapir
