// Copyright (c) 2012-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <coins.h>

#include <consensus/consensus.h>
#include <random.h>
#include <uint256.h>
#include <util/log.h>
#include <util/trace.h>

#include <algorithm>

TRACEPOINT_SEMAPHORE(utxocache, add);
TRACEPOINT_SEMAPHORE(utxocache, spent);
TRACEPOINT_SEMAPHORE(utxocache, uncache);

std::optional<Coin> CCoinsView::GetCoin(const COutPoint& outpoint) const { return std::nullopt; }
std::optional<Coin> CCoinsView::PeekCoin(const COutPoint& outpoint) const { return GetCoin(outpoint); }
uint256 CCoinsView::GetBestBlock() const { return uint256(); }
std::vector<uint256> CCoinsView::GetHeadBlocks() const { return std::vector<uint256>(); }
void CCoinsView::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& hashBlock)
{
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) { }
}

std::unique_ptr<CCoinsViewCursor> CCoinsView::Cursor() const { return nullptr; }

bool CCoinsView::HaveCoin(const COutPoint &outpoint) const
{
    return GetCoin(outpoint).has_value();
}

CCoinsViewBacked::CCoinsViewBacked(CCoinsView *viewIn) : base(viewIn) { }
std::optional<Coin> CCoinsViewBacked::GetCoin(const COutPoint& outpoint) const { return base->GetCoin(outpoint); }
std::optional<Coin> CCoinsViewBacked::PeekCoin(const COutPoint& outpoint) const { return base->PeekCoin(outpoint); }
bool CCoinsViewBacked::HaveCoin(const COutPoint &outpoint) const { return base->HaveCoin(outpoint); }
uint256 CCoinsViewBacked::GetBestBlock() const { return base->GetBestBlock(); }
std::vector<uint256> CCoinsViewBacked::GetHeadBlocks() const { return base->GetHeadBlocks(); }
void CCoinsViewBacked::SetBackend(CCoinsView &viewIn) { base = &viewIn; }
void CCoinsViewBacked::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& hashBlock) { base->BatchWrite(cursor, hashBlock); }
std::unique_ptr<CCoinsViewCursor> CCoinsViewBacked::Cursor() const { return base->Cursor(); }
size_t CCoinsViewBacked::EstimateSize() const { return base->EstimateSize(); }

std::optional<Coin> CCoinsViewCache::PeekCoin(const COutPoint& outpoint) const
{
    if (auto it{cacheCoins.find(outpoint)}; it != cacheCoins.end()) {
        return it->second.coin.IsSpent() ? std::nullopt : std::optional{it->second.coin};
    }
    return base->PeekCoin(outpoint);
}

CCoinsViewCache::CCoinsViewCache(CCoinsView* baseIn, bool deterministic) :
    CCoinsViewBacked(baseIn), m_deterministic(deterministic),
    cacheCoins(0, SaltedOutpointHasher(/*deterministic=*/deterministic), CCoinsMap::key_equal{}, &m_cache_coins_memory_resource)
{
    m_sentinel.second.SelfRef(m_sentinel);
}

size_t CCoinsViewCache::DynamicMemoryUsage() const {
    return memusage::DynamicUsage(cacheCoins) + cachedCoinsUsage;
}

std::optional<Coin> CCoinsViewCache::FetchCoinFromBase(const COutPoint& outpoint) const
{
    return base->GetCoin(outpoint);
}

CCoinsMap::iterator CCoinsViewCache::FetchCoin(const COutPoint &outpoint) const {
    const auto [ret, inserted] = cacheCoins.try_emplace(outpoint);
    if (inserted) {
        if (auto coin{FetchCoinFromBase(outpoint)}) {
            ret->second.coin = std::move(*coin);
            cachedCoinsUsage += ret->second.coin.DynamicMemoryUsage();
            Assert(!ret->second.coin.IsSpent());
        } else {
            cacheCoins.erase(ret);
            return cacheCoins.end();
        }
    }
    return ret;
}

std::optional<Coin> CCoinsViewCache::GetCoin(const COutPoint& outpoint) const
{
    if (auto it{FetchCoin(outpoint)}; it != cacheCoins.end() && !it->second.coin.IsSpent()) return it->second.coin;
    return std::nullopt;
}

void CCoinsViewCache::AddCoin(const COutPoint &outpoint, Coin&& coin, bool possible_overwrite) {
    assert(!coin.IsSpent());
    if (coin.out.scriptPubKey.IsUnspendable()) return;
    CCoinsMap::iterator it;
    bool inserted;
    std::tie(it, inserted) = cacheCoins.emplace(std::piecewise_construct, std::forward_as_tuple(outpoint), std::tuple<>());
    bool fresh = false;
    if (!possible_overwrite) {
        if (!it->second.coin.IsSpent()) {
            throw std::logic_error("Attempted to overwrite an unspent coin (when possible_overwrite is false)");
        }
        // If the coin exists in this cache as a spent coin and is DIRTY, then
        // its spentness hasn't been flushed to the parent cache. We're
        // re-adding the coin to this cache now but we can't mark it as FRESH.
        // If we mark it FRESH and then spend it before the cache is flushed
        // we would remove it from this cache and would never flush spentness
        // to the parent cache.
        //
        // Re-adding a spent coin can happen in the case of a re-org (the coin
        // is 'spent' when the block adding it is disconnected and then
        // re-added when it is also added in a newly connected block).
        //
        // If the coin doesn't exist in the current cache, or is spent but not
        // DIRTY, then it can be marked FRESH.
        fresh = !it->second.IsDirty();
    }
    if (!inserted) {
        m_dirty_count -= it->second.IsDirty();
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    }
    it->second.coin = std::move(coin);
    CCoinsCacheEntry::SetDirty(*it, m_sentinel);
    ++m_dirty_count;
    if (fresh) CCoinsCacheEntry::SetFresh(*it, m_sentinel);
    cachedCoinsUsage += it->second.coin.DynamicMemoryUsage();
    TRACEPOINT(utxocache, add,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
}

void CCoinsViewCache::EmplaceCoinInternalDANGER(COutPoint&& outpoint, Coin&& coin) {
    const auto mem_usage{coin.DynamicMemoryUsage()};
    auto [it, inserted] = cacheCoins.try_emplace(std::move(outpoint), std::move(coin));
    if (inserted) {
        CCoinsCacheEntry::SetDirty(*it, m_sentinel);
        ++m_dirty_count;
        cachedCoinsUsage += mem_usage;
    }
}

void AddCoins(CCoinsViewCache& cache, const CTransaction &tx, int nHeight, bool check_for_overwrite) {
    bool fCoinbase = tx.IsCoinBase();
    const Txid& txid = tx.GetHash();
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        bool overwrite = check_for_overwrite ? cache.HaveCoin(COutPoint(txid, i)) : fCoinbase;
        // Coinbase transactions can always be overwritten, in order to correctly
        // deal with the pre-BIP30 occurrences of duplicate coinbase transactions.
        cache.AddCoin(COutPoint(txid, i), Coin(tx.vout[i], nHeight, fCoinbase), overwrite);
    }
}

bool CCoinsViewCache::SpendCoin(const COutPoint &outpoint, Coin* moveout) {
    CCoinsMap::iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) return false;
    m_dirty_count -= it->second.IsDirty();
    cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
    TRACEPOINT(utxocache, spent,
           outpoint.hash.data(),
           (uint32_t)outpoint.n,
           (uint32_t)it->second.coin.nHeight,
           (int64_t)it->second.coin.out.nValue,
           (bool)it->second.coin.IsCoinBase());
    if (moveout) {
        *moveout = std::move(it->second.coin);
    }
    if (it->second.IsFresh()) {
        cacheCoins.erase(it);
    } else {
        CCoinsCacheEntry::SetDirty(*it, m_sentinel);
        ++m_dirty_count;
        it->second.coin.Clear();
    }
    return true;
}

static const Coin coinEmpty;

const Coin& CCoinsViewCache::AccessCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    if (it == cacheCoins.end()) {
        return coinEmpty;
    } else {
        return it->second.coin;
    }
}

bool CCoinsViewCache::HaveCoin(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = FetchCoin(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

bool CCoinsViewCache::HaveCoinInCache(const COutPoint &outpoint) const {
    CCoinsMap::const_iterator it = cacheCoins.find(outpoint);
    return (it != cacheCoins.end() && !it->second.coin.IsSpent());
}

uint256 CCoinsViewCache::GetBestBlock() const {
    if (hashBlock.IsNull())
        hashBlock = base->GetBestBlock();
    return hashBlock;
}

void CCoinsViewCache::SetBestBlock(const uint256 &hashBlockIn) {
    hashBlock = hashBlockIn;
}

void CCoinsViewCache::BatchWrite(CoinsViewCacheCursor& cursor, const uint256& hashBlockIn)
{
    for (auto it{cursor.Begin()}; it != cursor.End(); it = cursor.NextAndMaybeErase(*it)) {
        if (!it->second.IsDirty()) { // TODO a cursor can only contain dirty entries
            continue;
        }
        auto [itUs, inserted]{cacheCoins.try_emplace(it->first)};
        if (inserted) {
            if (it->second.IsFresh() && it->second.coin.IsSpent()) {
                cacheCoins.erase(itUs); // TODO fresh coins should have been removed at spend
            } else {
                // The parent cache does not have an entry, while the child cache does.
                // Move the data up and mark it as dirty.
                CCoinsCacheEntry& entry{itUs->second};
                assert(entry.coin.DynamicMemoryUsage() == 0);
                if (cursor.WillErase(*it)) {
                    // Since this entry will be erased,
                    // we can move the coin into us instead of copying it
                    entry.coin = std::move(it->second.coin);
                } else {
                    entry.coin = it->second.coin;
                }
                CCoinsCacheEntry::SetDirty(*itUs, m_sentinel);
                ++m_dirty_count;
                cachedCoinsUsage += entry.coin.DynamicMemoryUsage();
                // We can mark it FRESH in the parent if it was FRESH in the child
                // Otherwise it might have just been flushed from the parent's cache
                // and already exist in the grandparent
                if (it->second.IsFresh()) CCoinsCacheEntry::SetFresh(*itUs, m_sentinel);
            }
        } else {
            // Found the entry in the parent cache
            if (it->second.IsFresh() && !itUs->second.coin.IsSpent()) {
                // The coin was marked FRESH in the child cache, but the coin
                // exists in the parent cache. If this ever happens, it means
                // the FRESH flag was misapplied and there is a logic error in
                // the calling code.
                throw std::logic_error("FRESH flag misapplied to coin that exists in parent cache");
            }

            if (itUs->second.IsFresh() && it->second.coin.IsSpent()) {
                // The grandparent cache does not have an entry, and the coin
                // has been spent. We can just delete it from the parent cache.
                m_dirty_count -= itUs->second.IsDirty();
                cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                cacheCoins.erase(itUs);
            } else {
                // A normal modification.
                cachedCoinsUsage -= itUs->second.coin.DynamicMemoryUsage();
                if (cursor.WillErase(*it)) {
                    // Since this entry will be erased,
                    // we can move the coin into us instead of copying it
                    itUs->second.coin = std::move(it->second.coin);
                } else {
                    itUs->second.coin = it->second.coin;
                }
                cachedCoinsUsage += itUs->second.coin.DynamicMemoryUsage();
                if (!itUs->second.IsDirty()) {
                    CCoinsCacheEntry::SetDirty(*itUs, m_sentinel);
                    ++m_dirty_count;
                }
                // NOTE: It isn't safe to mark the coin as FRESH in the parent
                // cache. If it already existed and was spent in the parent
                // cache then marking it FRESH would prevent that spentness
                // from being flushed to the grandparent.
            }
        }
    }
    SetBestBlock(hashBlockIn);
}

void CCoinsViewCache::Flush(bool reallocate_cache)
{
    auto cursor{CoinsViewCacheCursor(m_dirty_count, m_sentinel, cacheCoins, /*will_erase=*/true)};
    base->BatchWrite(cursor, hashBlock);
    Assume(m_dirty_count == 0);
    cacheCoins.clear();
    if (reallocate_cache) {
        ReallocateCache();
    }
    cachedCoinsUsage = 0;
}

void CCoinsViewCache::Sync()
{
    auto cursor{CoinsViewCacheCursor(m_dirty_count, m_sentinel, cacheCoins, /*will_erase=*/false)};
    base->BatchWrite(cursor, hashBlock);
    Assume(m_dirty_count == 0);
    if (m_sentinel.second.Next() != &m_sentinel) {
        /* BatchWrite must clear flags of all entries */
        throw std::logic_error("Not all unspent flagged entries were cleared");
    }
}

void CCoinsViewCache::Reset() noexcept
{
    cacheCoins.clear();
    cachedCoinsUsage = 0;
    m_dirty_count = 0;
    SetBestBlock(uint256::ZERO);
}

void CCoinsViewCache::Uncache(const COutPoint& hash)
{
    CCoinsMap::iterator it = cacheCoins.find(hash);
    if (it != cacheCoins.end() && !it->second.IsDirty()) {
        cachedCoinsUsage -= it->second.coin.DynamicMemoryUsage();
        TRACEPOINT(utxocache, uncache,
               hash.hash.data(),
               (uint32_t)hash.n,
               (uint32_t)it->second.coin.nHeight,
               (int64_t)it->second.coin.out.nValue,
               (bool)it->second.coin.IsCoinBase());
        cacheCoins.erase(it);
    }
}

unsigned int CCoinsViewCache::GetCacheSize() const {
    return cacheCoins.size();
}

bool CCoinsViewCache::HaveInputs(const CTransaction& tx) const
{
    if (!tx.IsCoinBase()) {
        for (unsigned int i = 0; i < tx.vin.size(); i++) {
            if (!HaveCoin(tx.vin[i].prevout)) {
                return false;
            }
        }
    }
    return true;
}

void CCoinsViewCache::ReallocateCache()
{
    // Cache should be empty when we're calling this.
    assert(cacheCoins.size() == 0);
    cacheCoins.~CCoinsMap();
    m_cache_coins_memory_resource.~CCoinsMapMemoryResource();
    ::new (&m_cache_coins_memory_resource) CCoinsMapMemoryResource{};
    ::new (&cacheCoins) CCoinsMap{0, SaltedOutpointHasher{/*deterministic=*/m_deterministic}, CCoinsMap::key_equal{}, &m_cache_coins_memory_resource};
}

void CCoinsViewCache::SanityCheck() const
{
    size_t recomputed_usage = 0;
    size_t count_dirty = 0;
    for (const auto& [_, entry] : cacheCoins) {
        if (entry.coin.IsSpent()) {
            assert(entry.IsDirty() && !entry.IsFresh()); // A spent coin must be dirty and cannot be fresh
        } else {
            assert(entry.IsDirty() || !entry.IsFresh()); // An unspent coin must not be fresh if not dirty
        }

        // Recompute cachedCoinsUsage.
        recomputed_usage += entry.coin.DynamicMemoryUsage();

        // Count the number of entries we expect in the linked list.
        if (entry.IsDirty()) ++count_dirty;
    }
    // Iterate over the linked list of flagged entries.
    size_t count_linked = 0;
    for (auto it = m_sentinel.second.Next(); it != &m_sentinel; it = it->second.Next()) {
        // Verify linked list integrity.
        assert(it->second.Next()->second.Prev() == it);
        assert(it->second.Prev()->second.Next() == it);
        // Verify they are actually flagged.
        assert(it->second.IsDirty());
        // Count the number of entries actually in the list.
        ++count_linked;
    }
    assert(count_dirty == count_linked && count_dirty == m_dirty_count);
    assert(recomputed_usage == cachedCoinsUsage);
}

static const uint64_t MIN_TRANSACTION_OUTPUT_WEIGHT{WITNESS_SCALE_FACTOR * ::GetSerializeSize(CTxOut())};
static const uint64_t MAX_OUTPUTS_PER_BLOCK{MAX_BLOCK_WEIGHT / MIN_TRANSACTION_OUTPUT_WEIGHT};

const Coin& AccessByTxid(const CCoinsViewCache& view, const Txid& txid)
{
    COutPoint iter(txid, 0);
    while (iter.n < MAX_OUTPUTS_PER_BLOCK) {
        const Coin& alternate = view.AccessCoin(iter);
        if (!alternate.IsSpent()) return alternate;
        ++iter.n;
    }
    return coinEmpty;
}

template <typename ReturnType, typename Func>
static ReturnType ExecuteBackedWrapper(Func func, const std::vector<std::function<void()>>& err_callbacks)
{
    try {
        return func();
    } catch(const std::runtime_error& e) {
        for (const auto& f : err_callbacks) {
            f();
        }
        LogError("Error reading from database: %s\n", e.what());
        // Starting the shutdown sequence and returning false to the caller would be
        // interpreted as 'entry not found' (as opposed to unable to read data), and
        // could lead to invalid interpretation. Just exit immediately, as we can't
        // continue anyway, and all writes should be atomic.
        std::abort();
    }
}

std::optional<Coin> CCoinsViewErrorCatcher::GetCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<std::optional<Coin>>([&]() { return CCoinsViewBacked::GetCoin(outpoint); }, m_err_callbacks);
}

bool CCoinsViewErrorCatcher::HaveCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<bool>([&]() { return CCoinsViewBacked::HaveCoin(outpoint); }, m_err_callbacks);
}

std::optional<Coin> CCoinsViewErrorCatcher::PeekCoin(const COutPoint& outpoint) const
{
    return ExecuteBackedWrapper<std::optional<Coin>>([&]() { return CCoinsViewBacked::PeekCoin(outpoint); }, m_err_callbacks);
}

CoinsViewOverlay::CoinsViewOverlay(CCoinsView* base_in, bool deterministic) noexcept :
    CCoinsViewCache{base_in, deterministic}, m_hasher{deterministic} {}

bool CoinsViewOverlay::ProcessInput() const noexcept
{
    const auto i{m_input_head++};
    if (i >= m_inputs.size()) [[unlikely]] return false;

    auto& input{m_inputs[i]};
    // Inputs spending a coin from a tx earlier in the block won't be in the cache or db
    if (std::ranges::binary_search(m_txids, m_hasher(input.outpoint.hash))) {
        input.ready = true;
        return true;
    }

    if (auto coin{base->PeekCoin(input.outpoint)}) [[likely]] input.coin.emplace(std::move(*coin));
    input.ready = true;
    return true;
}

std::optional<Coin> CoinsViewOverlay::FetchCoinFromBase(const COutPoint& outpoint) const
{
    // This assumes ConnectBlock accesses all inputs in the same order as they are added to m_inputs
    // in StartFetching. Some outpoints are not accessed because they are created by the block, so we scan until we
    // come across the requested input.
    for (const auto i : std::views::iota(m_input_tail, m_inputs.size())) [[likely]] {
        auto& input{m_inputs[i]};
        if (input.outpoint != outpoint) continue;
        // We advance the tail since the input is cached and not accessed through this method again.
        m_input_tail = i + 1;
        // Check if the coin is ready to be read.
        while (!input.ready) {
            ProcessInput();
        }
        // We can move the coin since we won't access this input again.
        if (input.coin) [[likely]] return std::move(*input.coin);
        // This block has missing or spent inputs or there is a txid quick hash collision.
        break;
    }

    // We will only get in here for BIP30 checks, txid quick hash collisions or a block with missing or spent inputs.
    return base->PeekCoin(outpoint);
}

[[nodiscard]] CCoinsViewCache::ResetGuard CoinsViewOverlay::StartFetching(const CBlock& block LIFETIMEBOUND) noexcept
{
    Assert(m_inputs.empty());
    // Loop through the inputs of the block and set them in the queue. Also construct the set of txids to filter.
    for (const auto& tx : block.vtx | std::views::drop(1)) [[likely]] {
        for (const auto& input : tx->vin) [[likely]] m_inputs.emplace_back(input.prevout);
        m_txids.emplace_back(m_hasher(tx->GetHash()));
    }
    // Only start if we have something to fetch.
    if (!m_inputs.empty()) [[likely]] {
        // Sort txids so we can do binary search lookups.
        std::ranges::sort(m_txids);
    }
    if (m_inputs.empty()) {
        m_txids.clear();
    }
    return CreateResetGuard();
}

void CoinsViewOverlay::StopFetching() noexcept
{
    // Clear the queue (it holds references into the currently processed block).
    m_inputs.clear();
    m_input_head = 0;
    m_input_tail = 0;
    m_txids.clear();
}

void CoinsViewOverlay::Reset() noexcept
{
    StopFetching();
    CCoinsViewCache::Reset();
}
