// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <node/caches.h>
#include <util/byte_units.h>

#include <boost/test/unit_test.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

using namespace node;

namespace {
void CheckWarnThreshold(size_t threshold, size_t total_ram)
{
    BOOST_CHECK(!ShouldWarnOversizedDbCache(threshold, total_ram));
    BOOST_CHECK( ShouldWarnOversizedDbCache(threshold + 1, total_ram));
}
} // namespace

BOOST_AUTO_TEST_SUITE(caches_tests)

BOOST_AUTO_TEST_CASE(default_dbcache_formula_by_total_ram)
{
    BOOST_CHECK(FALLBACK_RAM_BYTES >= 1024_MiB);
    for (const auto& [total_ram, expected] : std::array<std::pair<size_t, size_t>, 4>{{
        {512_MiB, MIN_DEFAULT_DBCACHE},
        {1024_MiB, MIN_DEFAULT_DBCACHE},
        {RESERVED_RAM - 1, MIN_DEFAULT_DBCACHE},
        {RESERVED_RAM, MIN_DEFAULT_DBCACHE}
    }}) {
        BOOST_CHECK_EQUAL(GetDefaultCache(total_ram), expected);
    }

    constexpr size_t total_ram{3072_MiB};
    BOOST_CHECK_EQUAL(GetDefaultCache(total_ram), (total_ram - RESERVED_RAM) / 4);

    if constexpr (SIZE_MAX == UINT64_MAX) {
        for (const auto& [total_ram_64, expected] : std::array<std::pair<size_t, size_t>, 3>{{
            {8192_MiB, 1536_MiB},
            {16384_MiB, MAX_DEFAULT_DBCACHE},
            {32768_MiB, MAX_DEFAULT_DBCACHE}
        }}) {
            BOOST_CHECK_EQUAL(GetDefaultCache(total_ram_64), expected);
        }
    }
}

BOOST_AUTO_TEST_CASE(default_dbcache_uses_current_total_ram)
{
    BOOST_CHECK_EQUAL(GetDefaultCache(), GetDefaultCache(GetTotalRam()));
}

BOOST_AUTO_TEST_CASE(oversized_dbcache_warning)
{
    BOOST_CHECK(!ShouldWarnOversizedDbCache(MIN_DB_CACHE, 1024_MiB));

    CheckWarnThreshold(GetDefaultCache(1024_MiB), 1024_MiB);
    CheckWarnThreshold(GetDefaultCache(FALLBACK_RAM_BYTES - 1_MiB), FALLBACK_RAM_BYTES - 1_MiB);

    {
        constexpr size_t total_ram{FALLBACK_RAM_BYTES};
        CheckWarnThreshold((total_ram / 100) * 75, total_ram);
    }

    if constexpr (SIZE_MAX == UINT64_MAX) {
        const size_t total_ram{16384_MiB};
        BOOST_CHECK(!ShouldWarnOversizedDbCache(/*dbcache=*/12'000_MiB, total_ram));
        BOOST_CHECK( ShouldWarnOversizedDbCache(/*dbcache=*/13'000_MiB, total_ram));
    }
}

BOOST_AUTO_TEST_CASE(default_dbcache_never_warns)
{
    for (const auto& total_ram : {1024_MiB, 2048_MiB, 3072_MiB}) {
        BOOST_CHECK(!ShouldWarnOversizedDbCache(GetDefaultCache(total_ram), total_ram));
    }

    if constexpr (SIZE_MAX == UINT64_MAX) {
        for (const auto& total_ram : {4096_MiB, 8192_MiB, 16384_MiB, 32768_MiB}) {
            BOOST_CHECK(!ShouldWarnOversizedDbCache(GetDefaultCache(total_ram), total_ram));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
