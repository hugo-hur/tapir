// main.cpp — smoke test that the configured toolchain links against libarchive
// and libfuse, and reports which C++23 features are available.
// Replace with the real tapefs implementation.

#ifdef HAVE_CONFIG_H
#  include "config.h"        // defines FUSE_USE_VERSION + HAVE_STD_* before any use
#endif

#include <fuse.h>            // requires FUSE_USE_VERSION (from config.h)

#include <archive.h>
#include <archive_entry.h>

#include <cstdio>
#include <span>
#include <string_view>

#ifdef HAVE_STD_PRINT
#  include <print>
#endif
#ifdef HAVE_STD_EXPECTED
#  include <expected>
#endif

int main() {
    std::puts("tapefs configured OK");
    std::printf("  libarchive   : %s\n", archive_version_string());
    std::printf("  libfuse      : runtime %d, compiled for API %d\n",
                fuse_version(), FUSE_USE_VERSION);

#ifdef HAVE_STD_PRINT
    std::println("  std::print   : available");
#else
    std::printf("  std::print   : unavailable (fallback to printf)\n");
#endif

#ifdef HAVE_STD_EXPECTED
    std::expected<int, int> e{42};
    std::printf("  std::expected: available (value=%d)\n", e.value());
#else
    std::printf("  std::expected: unavailable\n");
#endif

    // exercise the libarchive writer API the project depends on
    struct archive* a = archive_write_new();
    archive_write_set_format_pax_restricted(a);
    archive_write_free(a);

    // trivial C++20 usage so the standard is actually compiled in
    constexpr std::string_view banner = "C++20 baseline ok";
    std::span<const char> sp{banner.data(), banner.size()};
    std::printf("  std baseline : %.*s\n", static_cast<int>(sp.size()), sp.data());
    return 0;
}
