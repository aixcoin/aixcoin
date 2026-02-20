// Copyright (c) The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_NODE_DBCACHE_H
#define BITCOIN_NODE_DBCACHE_H

#include <util/byte_units.h>

#include <cstddef>
#include <cstdint>
#include <limits>

//! min. -dbcache (bytes)
static constexpr size_t MIN_DB_CACHE{4_MiB};
//! -dbcache default (bytes)
static constexpr size_t DEFAULT_DB_CACHE{450_MiB};
//! Maximum dbcache size on current architecture.
static constexpr size_t MAX_DBCACHE_BYTES{SIZE_MAX == UINT64_MAX ? std::numeric_limits<size_t>::max() : 1024_MiB};

namespace node {
size_t GetDefaultCache() noexcept;
} // namespace node

#endif // BITCOIN_NODE_DBCACHE_H
