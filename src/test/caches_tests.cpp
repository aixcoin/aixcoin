// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit.

#include <node/caches.h>
#include <util/byte_units.h>

#include <boost/test/unit_test.hpp>

#include <cstdint>
#include <limits>

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
    BOOST_CHECK_EQUAL(GetDefaultCache(), DEFAULT_DB_CACHE);
}

BOOST_AUTO_TEST_CASE(oversized_dbcache_warning)
{
    BOOST_CHECK(!ShouldWarnOversizedDbCache(MIN_DB_CACHE, 1024_MiB));
    CheckWarnThreshold(GetDefaultCache(), 1024_MiB);

    {
        constexpr size_t total_ram{3072_MiB};
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
        BOOST_CHECK(!ShouldWarnOversizedDbCache(GetDefaultCache(), total_ram));
    }

    if constexpr (SIZE_MAX == UINT64_MAX) {
        for (const auto& total_ram : {4096_MiB, 8192_MiB, 16384_MiB, 32768_MiB}) {
            BOOST_CHECK(!ShouldWarnOversizedDbCache(GetDefaultCache(), total_ram));
        }
    }
}

BOOST_AUTO_TEST_SUITE_END()
