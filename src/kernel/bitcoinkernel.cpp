// Copyright (c) 2022-present The Aixcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#define AIXCOINKERNEL_BUILD

#include <kernel/aixcoinkernel.h>

#include <chain.h>
#include <coins.h>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <kernel/caches.h>
#include <kernel/chainparams.h>
#include <kernel/checks.h>
#include <kernel/context.h>
#include <kernel/notifications_interface.h>
#include <kernel/warning.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/chainstate.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <serialize.h>
#include <streams.h>
#include <sync.h>
#include <uint256.h>
#include <undo.h>
#include <util/check.h>
#include <util/fs.h>
#include <util/result.h>
#include <util/signalinterrupt.h>
#include <util/task_runner.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>

#include <cstddef>
#include <cstring>
#include <exception>
#include <functional>
#include <list>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using kernel::ChainstateRole;
using util::ImmediateTaskRunner;

// Define G_TRANSLATION_FUN symbol in libaixcoinkernel library so users of the
// library aren't required to export this symbol
extern const TranslateFn G_TRANSLATION_FUN{nullptr};

static const kernel::Context AIXk_context_static{};

namespace {

bool is_valid_flag_combination(script_verify_flags flags)
{
    if (flags & SCRIPT_VERIFY_CLEANSTACK && ~flags & (SCRIPT_VERIFY_P2SH | SCRIPT_VERIFY_WITNESS)) return false;
    if (flags & SCRIPT_VERIFY_WITNESS && ~flags & SCRIPT_VERIFY_P2SH) return false;
    return true;
}

class WriterStream
{
private:
    AIXk_WriteBytes m_writer;
    void* m_user_data;

public:
    WriterStream(AIXk_WriteBytes writer, void* user_data)
        : m_writer{writer}, m_user_data{user_data} {}

    //
    // Stream subset
    //
    void write(std::span<const std::byte> src)
    {
        if (m_writer(src.data(), src.size(), m_user_data) != 0) {
            throw std::runtime_error("Failed to write serialization data");
        }
    }

    template <typename T>
    WriterStream& operator<<(const T& obj)
    {
        ::Serialize(*this, obj);
        return *this;
    }
};

template <typename C, typename CPP>
struct Handle {
    static C* ref(CPP* cpp_type)
    {
        return reinterpret_cast<C*>(cpp_type);
    }

    static const C* ref(const CPP* cpp_type)
    {
        return reinterpret_cast<const C*>(cpp_type);
    }

    template <typename... Args>
    static C* create(Args&&... args)
    {
        auto cpp_obj{std::make_unique<CPP>(std::forward<Args>(args)...)};
        return ref(cpp_obj.release());
    }

    static C* copy(const C* ptr)
    {
        auto cpp_obj{std::make_unique<CPP>(get(ptr))};
        return ref(cpp_obj.release());
    }

    static const CPP& get(const C* ptr)
    {
        return *reinterpret_cast<const CPP*>(ptr);
    }

    static CPP& get(C* ptr)
    {
        return *reinterpret_cast<CPP*>(ptr);
    }

    static void operator delete(void* ptr)
    {
        delete reinterpret_cast<CPP*>(ptr);
    }
};

} // namespace

struct AIXk_BlockTreeEntry: Handle<AIXk_BlockTreeEntry, CBlockIndex> {};
struct AIXk_Block : Handle<AIXk_Block, std::shared_ptr<const CBlock>> {};
struct AIXk_BlockValidationState : Handle<AIXk_BlockValidationState, BlockValidationState> {};

namespace {

BCLog::Level get_bclog_level(AIXk_LogLevel level)
{
    switch (level) {
    case AIXk_LogLevel_INFO: {
        return BCLog::Level::Info;
    }
    case AIXk_LogLevel_DEBUG: {
        return BCLog::Level::Debug;
    }
    case AIXk_LogLevel_TRACE: {
        return BCLog::Level::Trace;
    }
    }
    assert(false);
}

BCLog::LogFlags get_bclog_flag(AIXk_LogCategory category)
{
    switch (category) {
    case AIXk_LogCategory_BENCH: {
        return BCLog::LogFlags::BENCH;
    }
    case AIXk_LogCategory_BLOCKSTORAGE: {
        return BCLog::LogFlags::BLOCKSTORAGE;
    }
    case AIXk_LogCategory_COINDB: {
        return BCLog::LogFlags::COINDB;
    }
    case AIXk_LogCategory_LEVELDB: {
        return BCLog::LogFlags::LEVELDB;
    }
    case AIXk_LogCategory_MEMPOOL: {
        return BCLog::LogFlags::MEMPOOL;
    }
    case AIXk_LogCategory_PRUNE: {
        return BCLog::LogFlags::PRUNE;
    }
    case AIXk_LogCategory_RAND: {
        return BCLog::LogFlags::RAND;
    }
    case AIXk_LogCategory_REINDEX: {
        return BCLog::LogFlags::REINDEX;
    }
    case AIXk_LogCategory_VALIDATION: {
        return BCLog::LogFlags::VALIDATION;
    }
    case AIXk_LogCategory_KERNEL: {
        return BCLog::LogFlags::KERNEL;
    }
    case AIXk_LogCategory_ALL: {
        return BCLog::LogFlags::ALL;
    }
    }
    assert(false);
}

AIXk_SynchronizationState cast_state(SynchronizationState state)
{
    switch (state) {
    case SynchronizationState::INIT_REINDEX:
        return AIXk_SynchronizationState_INIT_REINDEX;
    case SynchronizationState::INIT_DOWNLOAD:
        return AIXk_SynchronizationState_INIT_DOWNLOAD;
    case SynchronizationState::POST_INIT:
        return AIXk_SynchronizationState_POST_INIT;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

AIXk_Warning cast_AIXk_warning(kernel::Warning warning)
{
    switch (warning) {
    case kernel::Warning::UNKNOWN_NEW_RULES_ACTIVATED:
        return AIXk_Warning_UNKNOWN_NEW_RULES_ACTIVATED;
    case kernel::Warning::LARGE_WORK_INVALID_CHAIN:
        return AIXk_Warning_LARGE_WORK_INVALID_CHAIN;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

struct LoggingConnection {
    std::unique_ptr<std::list<std::function<void(const std::string&)>>::iterator> m_connection;
    void* m_user_data;
    std::function<void(void* user_data)> m_deleter;

    LoggingConnection(AIXk_LogCallback callback, void* user_data, AIXk_DestroyCallback user_data_destroy_callback)
    {
        LOCK(cs_main);

        auto connection{LogInstance().PushBackCallback([callback, user_data](const std::string& str) { callback(user_data, str.c_str(), str.length()); })};

        // Only start logging if we just added the connection.
        if (LogInstance().NumConnections() == 1 && !LogInstance().StartLogging()) {
            LogError("Logger start failed.");
            LogInstance().DeleteCallback(connection);
            if (user_data && user_data_destroy_callback) {
                user_data_destroy_callback(user_data);
            }
            throw std::runtime_error("Failed to start logging");
        }

        m_connection = std::make_unique<std::list<std::function<void(const std::string&)>>::iterator>(connection);
        m_user_data = user_data;
        m_deleter = user_data_destroy_callback;

        LogDebug(BCLog::KERNEL, "Logger connected.");
    }

    ~LoggingConnection()
    {
        LOCK(cs_main);
        LogDebug(BCLog::KERNEL, "Logger disconnecting.");

        // Switch back to buffering by calling DisconnectTestLogger if the
        // connection that we are about to remove is the last one.
        if (LogInstance().NumConnections() == 1) {
            LogInstance().DisconnectTestLogger();
        } else {
            LogInstance().DeleteCallback(*m_connection);
        }

        m_connection.reset();
        if (m_user_data && m_deleter) {
            m_deleter(m_user_data);
        }
    }
};

class KernelNotifications final : public kernel::Notifications
{
private:
    AIXk_NotificationInterfaceCallbacks m_cbs;

public:
    KernelNotifications(AIXk_NotificationInterfaceCallbacks cbs)
        : m_cbs{cbs}
    {
    }

    ~KernelNotifications()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data_destroy = nullptr;
        m_cbs.user_data = nullptr;
    }

    kernel::InterruptResult blockTip(SynchronizationState state, const CBlockIndex& index, double verification_progress) override
    {
        if (m_cbs.block_tip) m_cbs.block_tip(m_cbs.user_data, cast_state(state), AIXk_BlockTreeEntry::ref(&index), verification_progress);
        return {};
    }
    void headerTip(SynchronizationState state, int64_t height, int64_t timestamp, bool presync) override
    {
        if (m_cbs.header_tip) m_cbs.header_tip(m_cbs.user_data, cast_state(state), height, timestamp, presync ? 1 : 0);
    }
    void progress(const bilingual_str& title, int progress_percent, bool resume_possible) override
    {
        if (m_cbs.progress) m_cbs.progress(m_cbs.user_data, title.original.c_str(), title.original.length(), progress_percent, resume_possible ? 1 : 0);
    }
    void warningSet(kernel::Warning id, const bilingual_str& message) override
    {
        if (m_cbs.warning_set) m_cbs.warning_set(m_cbs.user_data, cast_AIXk_warning(id), message.original.c_str(), message.original.length());
    }
    void warningUnset(kernel::Warning id) override
    {
        if (m_cbs.warning_unset) m_cbs.warning_unset(m_cbs.user_data, cast_AIXk_warning(id));
    }
    void flushError(const bilingual_str& message) override
    {
        if (m_cbs.flush_error) m_cbs.flush_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
    void fatalError(const bilingual_str& message) override
    {
        if (m_cbs.fatal_error) m_cbs.fatal_error(m_cbs.user_data, message.original.c_str(), message.original.length());
    }
};

class KernelValidationInterface final : public CValidationInterface
{
public:
    AIXk_ValidationInterfaceCallbacks m_cbs;

    explicit KernelValidationInterface(const AIXk_ValidationInterfaceCallbacks vi_cbs) : m_cbs{vi_cbs} {}

    ~KernelValidationInterface()
    {
        if (m_cbs.user_data && m_cbs.user_data_destroy) {
            m_cbs.user_data_destroy(m_cbs.user_data);
        }
        m_cbs.user_data = nullptr;
        m_cbs.user_data_destroy = nullptr;
    }

protected:
    void BlockChecked(const std::shared_ptr<const CBlock>& block, const BlockValidationState& stateIn) override
    {
        if (m_cbs.block_checked) {
            m_cbs.block_checked(m_cbs.user_data,
                                AIXk_Block::copy(AIXk_Block::ref(&block)),
                                AIXk_BlockValidationState::ref(&stateIn));
        }
    }

    void NewPoWValidBlock(const CBlockIndex* pindex, const std::shared_ptr<const CBlock>& block) override
    {
        if (m_cbs.pow_valid_block) {
            m_cbs.pow_valid_block(m_cbs.user_data,
                                  AIXk_Block::copy(AIXk_Block::ref(&block)),
                                  AIXk_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockConnected(const ChainstateRole& role, const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_connected) {
            m_cbs.block_connected(m_cbs.user_data,
                                  AIXk_Block::copy(AIXk_Block::ref(&block)),
                                  AIXk_BlockTreeEntry::ref(pindex));
        }
    }

    void BlockDisconnected(const std::shared_ptr<const CBlock>& block, const CBlockIndex* pindex) override
    {
        if (m_cbs.block_disconnected) {
            m_cbs.block_disconnected(m_cbs.user_data,
                                     AIXk_Block::copy(AIXk_Block::ref(&block)),
                                     AIXk_BlockTreeEntry::ref(pindex));
        }
    }
};

struct ContextOptions {
    mutable Mutex m_mutex;
    std::unique_ptr<const CChainParams> m_chainparams GUARDED_BY(m_mutex);
    std::shared_ptr<KernelNotifications> m_notifications GUARDED_BY(m_mutex);
    std::shared_ptr<KernelValidationInterface> m_validation_interface GUARDED_BY(m_mutex);
};

class Context
{
public:
    std::unique_ptr<kernel::Context> m_context;

    std::shared_ptr<KernelNotifications> m_notifications;

    std::unique_ptr<util::SignalInterrupt> m_interrupt;

    std::unique_ptr<ValidationSignals> m_signals;

    std::unique_ptr<const CChainParams> m_chainparams;

    std::shared_ptr<KernelValidationInterface> m_validation_interface;

    Context(const ContextOptions* options, bool& sane)
        : m_context{std::make_unique<kernel::Context>()},
          m_interrupt{std::make_unique<util::SignalInterrupt>()}
    {
        if (options) {
            LOCK(options->m_mutex);
            if (options->m_chainparams) {
                m_chainparams = std::make_unique<const CChainParams>(*options->m_chainparams);
            }
            if (options->m_notifications) {
                m_notifications = options->m_notifications;
            }
            if (options->m_validation_interface) {
                m_signals = std::make_unique<ValidationSignals>(std::make_unique<ImmediateTaskRunner>());
                m_validation_interface = options->m_validation_interface;
                m_signals->RegisterSharedValidationInterface(m_validation_interface);
            }
        }

        if (!m_chainparams) {
            m_chainparams = CChainParams::Main();
        }
        if (!m_notifications) {
            m_notifications = std::make_shared<KernelNotifications>(AIXk_NotificationInterfaceCallbacks{
                nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr});
        }

        if (!kernel::SanityChecks(*m_context)) {
            sane = false;
        }
    }

    ~Context()
    {
        if (m_signals) {
            m_signals->UnregisterSharedValidationInterface(m_validation_interface);
        }
    }
};

//! Helper struct to wrap the ChainstateManager-related Options
struct ChainstateManagerOptions {
    mutable Mutex m_mutex;
    ChainstateManager::Options m_chainman_options GUARDED_BY(m_mutex);
    node::BlockManager::Options m_blockman_options GUARDED_BY(m_mutex);
    std::shared_ptr<const Context> m_context;
    node::ChainstateLoadOptions m_chainstate_load_options GUARDED_BY(m_mutex);

    ChainstateManagerOptions(const std::shared_ptr<const Context>& context, const fs::path& data_dir, const fs::path& blocks_dir)
        : m_chainman_options{ChainstateManager::Options{
              .chainparams = *context->m_chainparams,
              .datadir = data_dir,
              .notifications = *context->m_notifications,
              .signals = context->m_signals.get()}},
          m_blockman_options{node::BlockManager::Options{
              .chainparams = *context->m_chainparams,
              .blocks_dir = blocks_dir,
              .notifications = *context->m_notifications,
              .block_tree_db_params = DBParams{
                  .path = data_dir / "blocks" / "index",
                  .cache_bytes = kernel::CacheSizes{DEFAULT_KERNEL_CACHE}.block_tree_db,
              }}},
          m_context{context}, m_chainstate_load_options{node::ChainstateLoadOptions{}}
    {
    }
};

struct ChainMan {
    std::unique_ptr<ChainstateManager> m_chainman;
    std::shared_ptr<const Context> m_context;

    ChainMan(std::unique_ptr<ChainstateManager> chainman, std::shared_ptr<const Context> context)
        : m_chainman(std::move(chainman)), m_context(std::move(context)) {}
};

} // namespace

struct AIXk_Transaction : Handle<AIXk_Transaction, std::shared_ptr<const CTransaction>> {};
struct AIXk_TransactionOutput : Handle<AIXk_TransactionOutput, CTxOut> {};
struct AIXk_ScriptPubkey : Handle<AIXk_ScriptPubkey, CScript> {};
struct AIXk_LoggingConnection : Handle<AIXk_LoggingConnection, LoggingConnection> {};
struct AIXk_ContextOptions : Handle<AIXk_ContextOptions, ContextOptions> {};
struct AIXk_Context : Handle<AIXk_Context, std::shared_ptr<const Context>> {};
struct AIXk_ChainParameters : Handle<AIXk_ChainParameters, CChainParams> {};
struct AIXk_ChainstateManagerOptions : Handle<AIXk_ChainstateManagerOptions, ChainstateManagerOptions> {};
struct AIXk_ChainstateManager : Handle<AIXk_ChainstateManager, ChainMan> {};
struct AIXk_Chain : Handle<AIXk_Chain, CChain> {};
struct AIXk_BlockSpentOutputs : Handle<AIXk_BlockSpentOutputs, std::shared_ptr<CBlockUndo>> {};
struct AIXk_TransactionSpentOutputs : Handle<AIXk_TransactionSpentOutputs, CTxUndo> {};
struct AIXk_Coin : Handle<AIXk_Coin, Coin> {};
struct AIXk_BlockHash : Handle<AIXk_BlockHash, uint256> {};
struct AIXk_TransactionInput : Handle<AIXk_TransactionInput, CTxIn> {};
struct AIXk_TransactionOutPoint: Handle<AIXk_TransactionOutPoint, COutPoint> {};
struct AIXk_Txid: Handle<AIXk_Txid, Txid> {};
struct AIXk_PrecomputedTransactionData : Handle<AIXk_PrecomputedTransactionData, PrecomputedTransactionData> {};
struct AIXk_BlockHeader: Handle<AIXk_BlockHeader, CBlockHeader> {};

AIXk_Transaction* AIXk_transaction_create(const void* raw_transaction, size_t raw_transaction_len)
{
    if (raw_transaction == nullptr && raw_transaction_len != 0) {
        return nullptr;
    }
    try {
        SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_transaction), raw_transaction_len}};
        return AIXk_Transaction::create(std::make_shared<const CTransaction>(deserialize, TX_WITH_WITNESS, stream));
    } catch (...) {
        return nullptr;
    }
}

size_t AIXk_transaction_count_outputs(const AIXk_Transaction* transaction)
{
    return AIXk_Transaction::get(transaction)->vout.size();
}

const AIXk_TransactionOutput* AIXk_transaction_get_output_at(const AIXk_Transaction* transaction, size_t output_index)
{
    const CTransaction& tx = *AIXk_Transaction::get(transaction);
    assert(output_index < tx.vout.size());
    return AIXk_TransactionOutput::ref(&tx.vout[output_index]);
}

size_t AIXk_transaction_count_inputs(const AIXk_Transaction* transaction)
{
    return AIXk_Transaction::get(transaction)->vin.size();
}

const AIXk_TransactionInput* AIXk_transaction_get_input_at(const AIXk_Transaction* transaction, size_t input_index)
{
    assert(input_index < AIXk_Transaction::get(transaction)->vin.size());
    return AIXk_TransactionInput::ref(&AIXk_Transaction::get(transaction)->vin[input_index]);
}

const AIXk_Txid* AIXk_transaction_get_txid(const AIXk_Transaction* transaction)
{
    return AIXk_Txid::ref(&AIXk_Transaction::get(transaction)->GetHash());
}

AIXk_Transaction* AIXk_transaction_copy(const AIXk_Transaction* transaction)
{
    return AIXk_Transaction::copy(transaction);
}

int AIXk_transaction_to_bytes(const AIXk_Transaction* transaction, AIXk_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(AIXk_Transaction::get(transaction));
        return 0;
    } catch (...) {
        return -1;
    }
}

void AIXk_transaction_destroy(AIXk_Transaction* transaction)
{
    delete transaction;
}

AIXk_ScriptPubkey* AIXk_script_pubkey_create(const void* script_pubkey, size_t script_pubkey_len)
{
    if (script_pubkey == nullptr && script_pubkey_len != 0) {
        return nullptr;
    }
    auto data = std::span{reinterpret_cast<const uint8_t*>(script_pubkey), script_pubkey_len};
    return AIXk_ScriptPubkey::create(data.begin(), data.end());
}

int AIXk_script_pubkey_to_bytes(const AIXk_ScriptPubkey* script_pubkey_, AIXk_WriteBytes writer, void* user_data)
{
    const auto& script_pubkey{AIXk_ScriptPubkey::get(script_pubkey_)};
    return writer(script_pubkey.data(), script_pubkey.size(), user_data);
}

AIXk_ScriptPubkey* AIXk_script_pubkey_copy(const AIXk_ScriptPubkey* script_pubkey)
{
    return AIXk_ScriptPubkey::copy(script_pubkey);
}

void AIXk_script_pubkey_destroy(AIXk_ScriptPubkey* script_pubkey)
{
    delete script_pubkey;
}

AIXk_TransactionOutput* AIXk_transaction_output_create(const AIXk_ScriptPubkey* script_pubkey, int64_t amount)
{
    return AIXk_TransactionOutput::create(amount, AIXk_ScriptPubkey::get(script_pubkey));
}

AIXk_TransactionOutput* AIXk_transaction_output_copy(const AIXk_TransactionOutput* output)
{
    return AIXk_TransactionOutput::copy(output);
}

const AIXk_ScriptPubkey* AIXk_transaction_output_get_script_pubkey(const AIXk_TransactionOutput* output)
{
    return AIXk_ScriptPubkey::ref(&AIXk_TransactionOutput::get(output).scriptPubKey);
}

int64_t AIXk_transaction_output_get_amount(const AIXk_TransactionOutput* output)
{
    return AIXk_TransactionOutput::get(output).nValue;
}

void AIXk_transaction_output_destroy(AIXk_TransactionOutput* output)
{
    delete output;
}

AIXk_PrecomputedTransactionData* AIXk_precomputed_transaction_data_create(
    const AIXk_Transaction* tx_to,
    const AIXk_TransactionOutput** spent_outputs_, size_t spent_outputs_len)
{
    try {
        const CTransaction& tx{*AIXk_Transaction::get(tx_to)};
        auto txdata{AIXk_PrecomputedTransactionData::create()};
        if (spent_outputs_ != nullptr && spent_outputs_len > 0) {
            assert(spent_outputs_len == tx.vin.size());
            std::vector<CTxOut> spent_outputs;
            spent_outputs.reserve(spent_outputs_len);
            for (size_t i = 0; i < spent_outputs_len; i++) {
                const CTxOut& tx_out{AIXk_TransactionOutput::get(spent_outputs_[i])};
                spent_outputs.push_back(tx_out);
            }
            AIXk_PrecomputedTransactionData::get(txdata).Init(tx, std::move(spent_outputs));
        } else {
            AIXk_PrecomputedTransactionData::get(txdata).Init(tx, {});
        }

        return txdata;
    } catch (...) {
        return nullptr;
    }
}

AIXk_PrecomputedTransactionData* AIXk_precomputed_transaction_data_copy(const AIXk_PrecomputedTransactionData* precomputed_txdata)
{
    return AIXk_PrecomputedTransactionData::copy(precomputed_txdata);
}

void AIXk_precomputed_transaction_data_destroy(AIXk_PrecomputedTransactionData* precomputed_txdata)
{
    delete precomputed_txdata;
}

int AIXk_script_pubkey_verify(const AIXk_ScriptPubkey* script_pubkey,
                              const int64_t amount,
                              const AIXk_Transaction* tx_to,
                              const AIXk_PrecomputedTransactionData* precomputed_txdata,
                              const unsigned int input_index,
                              const AIXk_ScriptVerificationFlags flags,
                              AIXk_ScriptVerifyStatus* status)
{
    // Assert that all specified flags are part of the interface before continuing
    assert((flags & ~AIXk_ScriptVerificationFlags_ALL) == 0);

    if (!is_valid_flag_combination(script_verify_flags::from_int(flags))) {
        if (status) *status = AIXk_ScriptVerifyStatus_ERROR_INVALID_FLAGS_COMBINATION;
        return 0;
    }

    const CTransaction& tx{*AIXk_Transaction::get(tx_to)};
    assert(input_index < tx.vin.size());

    const PrecomputedTransactionData& txdata{precomputed_txdata ? AIXk_PrecomputedTransactionData::get(precomputed_txdata) : PrecomputedTransactionData(tx)};

    if (flags & AIXk_ScriptVerificationFlags_TAPROOT && txdata.m_spent_outputs.empty()) {
        if (status) *status = AIXk_ScriptVerifyStatus_ERROR_SPENT_OUTPUTS_REQUIRED;
        return 0;
    }

    if (status) *status = AIXk_ScriptVerifyStatus_OK;

    bool result = VerifyScript(tx.vin[input_index].scriptSig,
                               AIXk_ScriptPubkey::get(script_pubkey),
                               &tx.vin[input_index].scriptWitness,
                               script_verify_flags::from_int(flags),
                               TransactionSignatureChecker(&tx, input_index, amount, txdata, MissingDataBehavior::FAIL),
                               nullptr);
    return result ? 1 : 0;
}

AIXk_TransactionInput* AIXk_transaction_input_copy(const AIXk_TransactionInput* input)
{
    return AIXk_TransactionInput::copy(input);
}

const AIXk_TransactionOutPoint* AIXk_transaction_input_get_out_point(const AIXk_TransactionInput* input)
{
    return AIXk_TransactionOutPoint::ref(&AIXk_TransactionInput::get(input).prevout);
}

void AIXk_transaction_input_destroy(AIXk_TransactionInput* input)
{
    delete input;
}

AIXk_TransactionOutPoint* AIXk_transaction_out_point_copy(const AIXk_TransactionOutPoint* out_point)
{
    return AIXk_TransactionOutPoint::copy(out_point);
}

uint32_t AIXk_transaction_out_point_get_index(const AIXk_TransactionOutPoint* out_point)
{
    return AIXk_TransactionOutPoint::get(out_point).n;
}

const AIXk_Txid* AIXk_transaction_out_point_get_txid(const AIXk_TransactionOutPoint* out_point)
{
    return AIXk_Txid::ref(&AIXk_TransactionOutPoint::get(out_point).hash);
}

void AIXk_transaction_out_point_destroy(AIXk_TransactionOutPoint* out_point)
{
    delete out_point;
}

AIXk_Txid* AIXk_txid_copy(const AIXk_Txid* txid)
{
    return AIXk_Txid::copy(txid);
}

void AIXk_txid_to_bytes(const AIXk_Txid* txid, unsigned char output[32])
{
    std::memcpy(output, AIXk_Txid::get(txid).begin(), 32);
}

int AIXk_txid_equals(const AIXk_Txid* txid1, const AIXk_Txid* txid2)
{
    return AIXk_Txid::get(txid1) == AIXk_Txid::get(txid2);
}

void AIXk_txid_destroy(AIXk_Txid* txid)
{
    delete txid;
}

void AIXk_logging_set_options(const AIXk_LoggingOptions options)
{
    LOCK(cs_main);
    LogInstance().m_log_timestamps = options.log_timestamps;
    LogInstance().m_log_time_micros = options.log_time_micros;
    LogInstance().m_log_threadnames = options.log_threadnames;
    LogInstance().m_log_sourcelocations = options.log_sourcelocations;
    LogInstance().m_always_print_category_level = options.always_print_category_levels;
}

void AIXk_logging_set_level_category(AIXk_LogCategory category, AIXk_LogLevel level)
{
    LOCK(cs_main);
    if (category == AIXk_LogCategory_ALL) {
        LogInstance().SetLogLevel(get_bclog_level(level));
    }

    LogInstance().AddCategoryLogLevel(get_bclog_flag(category), get_bclog_level(level));
}

void AIXk_logging_enable_category(AIXk_LogCategory category)
{
    LogInstance().EnableCategory(get_bclog_flag(category));
}

void AIXk_logging_disable_category(AIXk_LogCategory category)
{
    LogInstance().DisableCategory(get_bclog_flag(category));
}

void AIXk_logging_disable()
{
    LogInstance().DisableLogging();
}

AIXk_LoggingConnection* AIXk_logging_connection_create(AIXk_LogCallback callback, void* user_data, AIXk_DestroyCallback user_data_destroy_callback)
{
    try {
        return AIXk_LoggingConnection::create(callback, user_data, user_data_destroy_callback);
    } catch (const std::exception&) {
        return nullptr;
    }
}

void AIXk_logging_connection_destroy(AIXk_LoggingConnection* connection)
{
    delete connection;
}

AIXk_ChainParameters* AIXk_chain_parameters_create(const AIXk_ChainType chain_type)
{
    switch (chain_type) {
    case AIXk_ChainType_MAINNET: {
        return AIXk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::Main().release()));
    }
    case AIXk_ChainType_TESTNET: {
        return AIXk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet().release()));
    }
    case AIXk_ChainType_TESTNET_4: {
        return AIXk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::TestNet4().release()));
    }
    case AIXk_ChainType_SIGNET: {
        return AIXk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::SigNet({}).release()));
    }
    case AIXk_ChainType_REGTEST: {
        return AIXk_ChainParameters::ref(const_cast<CChainParams*>(CChainParams::RegTest({}).release()));
    }
    }
    assert(false);
}

AIXk_ChainParameters* AIXk_chain_parameters_copy(const AIXk_ChainParameters* chain_parameters)
{
    return AIXk_ChainParameters::copy(chain_parameters);
}

void AIXk_chain_parameters_destroy(AIXk_ChainParameters* chain_parameters)
{
    delete chain_parameters;
}

AIXk_ContextOptions* AIXk_context_options_create()
{
    return AIXk_ContextOptions::create();
}

void AIXk_context_options_set_chainparams(AIXk_ContextOptions* options, const AIXk_ChainParameters* chain_parameters)
{
    // Copy the chainparams, so the caller can free it again
    LOCK(AIXk_ContextOptions::get(options).m_mutex);
    AIXk_ContextOptions::get(options).m_chainparams = std::make_unique<const CChainParams>(AIXk_ChainParameters::get(chain_parameters));
}

void AIXk_context_options_set_notifications(AIXk_ContextOptions* options, AIXk_NotificationInterfaceCallbacks notifications)
{
    // The KernelNotifications are copy-initialized, so the caller can free them again.
    LOCK(AIXk_ContextOptions::get(options).m_mutex);
    AIXk_ContextOptions::get(options).m_notifications = std::make_shared<KernelNotifications>(notifications);
}

void AIXk_context_options_set_validation_interface(AIXk_ContextOptions* options, AIXk_ValidationInterfaceCallbacks vi_cbs)
{
    LOCK(AIXk_ContextOptions::get(options).m_mutex);
    AIXk_ContextOptions::get(options).m_validation_interface = std::make_shared<KernelValidationInterface>(vi_cbs);
}

void AIXk_context_options_destroy(AIXk_ContextOptions* options)
{
    delete options;
}

AIXk_Context* AIXk_context_create(const AIXk_ContextOptions* options)
{
    bool sane{true};
    const ContextOptions* opts = options ? &AIXk_ContextOptions::get(options) : nullptr;
    auto context{std::make_shared<const Context>(opts, sane)};
    if (!sane) {
        LogError("Kernel context sanity check failed.");
        return nullptr;
    }
    return AIXk_Context::create(context);
}

AIXk_Context* AIXk_context_copy(const AIXk_Context* context)
{
    return AIXk_Context::copy(context);
}

int AIXk_context_interrupt(AIXk_Context* context)
{
    return (*AIXk_Context::get(context)->m_interrupt)() ? 0 : -1;
}

void AIXk_context_destroy(AIXk_Context* context)
{
    delete context;
}

const AIXk_BlockTreeEntry* AIXk_block_tree_entry_get_previous(const AIXk_BlockTreeEntry* entry)
{
    if (!AIXk_BlockTreeEntry::get(entry).pprev) {
        LogInfo("Genesis block has no previous.");
        return nullptr;
    }

    return AIXk_BlockTreeEntry::ref(AIXk_BlockTreeEntry::get(entry).pprev);
}

AIXk_BlockValidationState* AIXk_block_validation_state_create()
{
    return AIXk_BlockValidationState::create();
}

AIXk_BlockValidationState* AIXk_block_validation_state_copy(const AIXk_BlockValidationState* state)
{
    return AIXk_BlockValidationState::copy(state);
}

void AIXk_block_validation_state_destroy(AIXk_BlockValidationState* state)
{
    delete state;
}

AIXk_ValidationMode AIXk_block_validation_state_get_validation_mode(const AIXk_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = AIXk_BlockValidationState::get(block_validation_state_);
    if (block_validation_state.IsValid()) return AIXk_ValidationMode_VALID;
    if (block_validation_state.IsInvalid()) return AIXk_ValidationMode_INVALID;
    return AIXk_ValidationMode_INTERNAL_ERROR;
}

AIXk_BlockValidationResult AIXk_block_validation_state_get_block_validation_result(const AIXk_BlockValidationState* block_validation_state_)
{
    auto& block_validation_state = AIXk_BlockValidationState::get(block_validation_state_);
    switch (block_validation_state.GetResult()) {
    case BlockValidationResult::BLOCK_RESULT_UNSET:
        return AIXk_BlockValidationResult_UNSET;
    case BlockValidationResult::BLOCK_CONSENSUS:
        return AIXk_BlockValidationResult_CONSENSUS;
    case BlockValidationResult::BLOCK_CACHED_INVALID:
        return AIXk_BlockValidationResult_CACHED_INVALID;
    case BlockValidationResult::BLOCK_INVALID_HEADER:
        return AIXk_BlockValidationResult_INVALID_HEADER;
    case BlockValidationResult::BLOCK_MUTATED:
        return AIXk_BlockValidationResult_MUTATED;
    case BlockValidationResult::BLOCK_MISSING_PREV:
        return AIXk_BlockValidationResult_MISSING_PREV;
    case BlockValidationResult::BLOCK_INVALID_PREV:
        return AIXk_BlockValidationResult_INVALID_PREV;
    case BlockValidationResult::BLOCK_TIME_FUTURE:
        return AIXk_BlockValidationResult_TIME_FUTURE;
    case BlockValidationResult::BLOCK_HEADER_LOW_WORK:
        return AIXk_BlockValidationResult_HEADER_LOW_WORK;
    } // no default case, so the compiler can warn about missing cases
    assert(false);
}

AIXk_ChainstateManagerOptions* AIXk_chainstate_manager_options_create(const AIXk_Context* context, const char* data_dir, size_t data_dir_len, const char* blocks_dir, size_t blocks_dir_len)
{
    if (data_dir == nullptr || data_dir_len == 0 || blocks_dir == nullptr || blocks_dir_len == 0) {
        LogError("Failed to create chainstate manager options: dir must be non-null and non-empty");
        return nullptr;
    }
    try {
        fs::path abs_data_dir{fs::absolute(fs::PathFromString({data_dir, data_dir_len}))};
        fs::create_directories(abs_data_dir);
        fs::path abs_blocks_dir{fs::absolute(fs::PathFromString({blocks_dir, blocks_dir_len}))};
        fs::create_directories(abs_blocks_dir);
        return AIXk_ChainstateManagerOptions::create(AIXk_Context::get(context), abs_data_dir, abs_blocks_dir);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager options: %s", e.what());
        return nullptr;
    }
}

void AIXk_chainstate_manager_options_set_worker_threads_num(AIXk_ChainstateManagerOptions* opts, int worker_threads)
{
    LOCK(AIXk_ChainstateManagerOptions::get(opts).m_mutex);
    AIXk_ChainstateManagerOptions::get(opts).m_chainman_options.worker_threads_num = worker_threads;
}

void AIXk_chainstate_manager_options_destroy(AIXk_ChainstateManagerOptions* options)
{
    delete options;
}

int AIXk_chainstate_manager_options_set_wipe_dbs(AIXk_ChainstateManagerOptions* chainman_opts, int wipe_block_tree_db, int wipe_chainstate_db)
{
    if (wipe_block_tree_db == 1 && wipe_chainstate_db != 1) {
        LogError("Wiping the block tree db without also wiping the chainstate db is currently unsupported.");
        return -1;
    }
    auto& opts{AIXk_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.wipe_data = wipe_block_tree_db == 1;
    opts.m_chainstate_load_options.wipe_chainstate_db = wipe_chainstate_db == 1;
    return 0;
}

void AIXk_chainstate_manager_options_update_block_tree_db_in_memory(
    AIXk_ChainstateManagerOptions* chainman_opts,
    int block_tree_db_in_memory)
{
    auto& opts{AIXk_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_blockman_options.block_tree_db_params.memory_only = block_tree_db_in_memory == 1;
}

void AIXk_chainstate_manager_options_update_chainstate_db_in_memory(
    AIXk_ChainstateManagerOptions* chainman_opts,
    int chainstate_db_in_memory)
{
    auto& opts{AIXk_ChainstateManagerOptions::get(chainman_opts)};
    LOCK(opts.m_mutex);
    opts.m_chainstate_load_options.coins_db_in_memory = chainstate_db_in_memory == 1;
}

AIXk_ChainstateManager* AIXk_chainstate_manager_create(
    const AIXk_ChainstateManagerOptions* chainman_opts)
{
    auto& opts{AIXk_ChainstateManagerOptions::get(chainman_opts)};
    std::unique_ptr<ChainstateManager> chainman;
    try {
        LOCK(opts.m_mutex);
        chainman = std::make_unique<ChainstateManager>(*opts.m_context->m_interrupt, opts.m_chainman_options, opts.m_blockman_options);
    } catch (const std::exception& e) {
        LogError("Failed to create chainstate manager: %s", e.what());
        return nullptr;
    }

    try {
        const auto chainstate_load_opts{WITH_LOCK(opts.m_mutex, return opts.m_chainstate_load_options)};

        kernel::CacheSizes cache_sizes{DEFAULT_KERNEL_CACHE};
        auto [status, chainstate_err]{node::LoadChainstate(*chainman, cache_sizes, chainstate_load_opts)};
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to load chain state from your data directory: %s", chainstate_err.original);
            return nullptr;
        }
        std::tie(status, chainstate_err) = node::VerifyLoadedChainstate(*chainman, chainstate_load_opts);
        if (status != node::ChainstateLoadStatus::SUCCESS) {
            LogError("Failed to verify loaded chain state from your datadir: %s", chainstate_err.original);
            return nullptr;
        }
        if (auto result = chainman->ActivateBestChains(); !result) {
            LogError("%s", util::ErrorString(result).original);
            return nullptr;
        }
    } catch (const std::exception& e) {
        LogError("Failed to load chainstate: %s", e.what());
        return nullptr;
    }

    return AIXk_ChainstateManager::create(std::move(chainman), opts.m_context);
}

const AIXk_BlockTreeEntry* AIXk_chainstate_manager_get_block_tree_entry_by_hash(const AIXk_ChainstateManager* chainman, const AIXk_BlockHash* block_hash)
{
    auto block_index = WITH_LOCK(AIXk_ChainstateManager::get(chainman).m_chainman->GetMutex(),
                                 return AIXk_ChainstateManager::get(chainman).m_chainman->m_blockman.LookupBlockIndex(AIXk_BlockHash::get(block_hash)));
    if (!block_index) {
        LogDebug(BCLog::KERNEL, "A block with the given hash is not indexed.");
        return nullptr;
    }
    return AIXk_BlockTreeEntry::ref(block_index);
}

const AIXk_BlockTreeEntry* AIXk_chainstate_manager_get_best_entry(const AIXk_ChainstateManager* chainstate_manager)
{
    auto& chainman = *AIXk_ChainstateManager::get(chainstate_manager).m_chainman;
    return AIXk_BlockTreeEntry::ref(WITH_LOCK(chainman.GetMutex(), return chainman.m_best_header));
}

void AIXk_chainstate_manager_destroy(AIXk_ChainstateManager* chainman)
{
    {
        LOCK(AIXk_ChainstateManager::get(chainman).m_chainman->GetMutex());
        for (const auto& chainstate : AIXk_ChainstateManager::get(chainman).m_chainman->m_chainstates) {
            if (chainstate->CanFlushToDisk()) {
                chainstate->ForceFlushStateToDisk();
                chainstate->ResetCoinsViews();
            }
        }
    }

    delete chainman;
}

int AIXk_chainstate_manager_import_blocks(AIXk_ChainstateManager* chainman, const char** block_file_paths_data, size_t* block_file_paths_lens, size_t block_file_paths_data_len)
{
    try {
        std::vector<fs::path> import_files;
        import_files.reserve(block_file_paths_data_len);
        for (uint32_t i = 0; i < block_file_paths_data_len; i++) {
            if (block_file_paths_data[i] != nullptr) {
                import_files.emplace_back(std::string{block_file_paths_data[i], block_file_paths_lens[i]}.c_str());
            }
        }
        auto& chainman_ref{*AIXk_ChainstateManager::get(chainman).m_chainman};
        node::ImportBlocks(chainman_ref, import_files);
        WITH_LOCK(::cs_main, chainman_ref.UpdateIBDStatus());
    } catch (const std::exception& e) {
        LogError("Failed to import blocks: %s", e.what());
        return -1;
    }
    return 0;
}

AIXk_Block* AIXk_block_create(const void* raw_block, size_t raw_block_length)
{
    if (raw_block == nullptr && raw_block_length != 0) {
        return nullptr;
    }
    auto block{std::make_shared<CBlock>()};

    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block), raw_block_length}};

    try {
        stream >> TX_WITH_WITNESS(*block);
    } catch (...) {
        LogDebug(BCLog::KERNEL, "Block decode failed.");
        return nullptr;
    }

    return AIXk_Block::create(block);
}

AIXk_Block* AIXk_block_copy(const AIXk_Block* block)
{
    return AIXk_Block::copy(block);
}

size_t AIXk_block_count_transactions(const AIXk_Block* block)
{
    return AIXk_Block::get(block)->vtx.size();
}

const AIXk_Transaction* AIXk_block_get_transaction_at(const AIXk_Block* block, size_t index)
{
    assert(index < AIXk_Block::get(block)->vtx.size());
    return AIXk_Transaction::ref(&AIXk_Block::get(block)->vtx[index]);
}

AIXk_BlockHeader* AIXk_block_get_header(const AIXk_Block* block)
{
    const auto& block_ptr = AIXk_Block::get(block);
    return AIXk_BlockHeader::create(static_cast<const CBlockHeader&>(*block_ptr));
}

int AIXk_block_to_bytes(const AIXk_Block* block, AIXk_WriteBytes writer, void* user_data)
{
    try {
        WriterStream ws{writer, user_data};
        ws << TX_WITH_WITNESS(*AIXk_Block::get(block));
        return 0;
    } catch (...) {
        return -1;
    }
}

AIXk_BlockHash* AIXk_block_get_hash(const AIXk_Block* block)
{
    return AIXk_BlockHash::create(AIXk_Block::get(block)->GetHash());
}

void AIXk_block_destroy(AIXk_Block* block)
{
    delete block;
}

AIXk_Block* AIXk_block_read(const AIXk_ChainstateManager* chainman, const AIXk_BlockTreeEntry* entry)
{
    auto block{std::make_shared<CBlock>()};
    if (!AIXk_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlock(*block, AIXk_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block.");
        return nullptr;
    }
    return AIXk_Block::create(block);
}

AIXk_BlockHeader* AIXk_block_tree_entry_get_block_header(const AIXk_BlockTreeEntry* entry)
{
    return AIXk_BlockHeader::create(AIXk_BlockTreeEntry::get(entry).GetBlockHeader());
}

int32_t AIXk_block_tree_entry_get_height(const AIXk_BlockTreeEntry* entry)
{
    return AIXk_BlockTreeEntry::get(entry).nHeight;
}

const AIXk_BlockHash* AIXk_block_tree_entry_get_block_hash(const AIXk_BlockTreeEntry* entry)
{
    return AIXk_BlockHash::ref(AIXk_BlockTreeEntry::get(entry).phashBlock);
}

int AIXk_block_tree_entry_equals(const AIXk_BlockTreeEntry* entry1, const AIXk_BlockTreeEntry* entry2)
{
    return &AIXk_BlockTreeEntry::get(entry1) == &AIXk_BlockTreeEntry::get(entry2);
}

AIXk_BlockHash* AIXk_block_hash_create(const unsigned char block_hash[32])
{
    return AIXk_BlockHash::create(std::span<const unsigned char>{block_hash, 32});
}

AIXk_BlockHash* AIXk_block_hash_copy(const AIXk_BlockHash* block_hash)
{
    return AIXk_BlockHash::copy(block_hash);
}

void AIXk_block_hash_to_bytes(const AIXk_BlockHash* block_hash, unsigned char output[32])
{
    std::memcpy(output, AIXk_BlockHash::get(block_hash).begin(), 32);
}

int AIXk_block_hash_equals(const AIXk_BlockHash* hash1, const AIXk_BlockHash* hash2)
{
    return AIXk_BlockHash::get(hash1) == AIXk_BlockHash::get(hash2);
}

void AIXk_block_hash_destroy(AIXk_BlockHash* hash)
{
    delete hash;
}

AIXk_BlockSpentOutputs* AIXk_block_spent_outputs_read(const AIXk_ChainstateManager* chainman, const AIXk_BlockTreeEntry* entry)
{
    auto block_undo{std::make_shared<CBlockUndo>()};
    if (AIXk_BlockTreeEntry::get(entry).nHeight < 1) {
        LogDebug(BCLog::KERNEL, "The genesis block does not have any spent outputs.");
        return AIXk_BlockSpentOutputs::create(block_undo);
    }
    if (!AIXk_ChainstateManager::get(chainman).m_chainman->m_blockman.ReadBlockUndo(*block_undo, AIXk_BlockTreeEntry::get(entry))) {
        LogError("Failed to read block spent outputs data.");
        return nullptr;
    }
    return AIXk_BlockSpentOutputs::create(block_undo);
}

AIXk_BlockSpentOutputs* AIXk_block_spent_outputs_copy(const AIXk_BlockSpentOutputs* block_spent_outputs)
{
    return AIXk_BlockSpentOutputs::copy(block_spent_outputs);
}

size_t AIXk_block_spent_outputs_count(const AIXk_BlockSpentOutputs* block_spent_outputs)
{
    return AIXk_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size();
}

const AIXk_TransactionSpentOutputs* AIXk_block_spent_outputs_get_transaction_spent_outputs_at(const AIXk_BlockSpentOutputs* block_spent_outputs, size_t transaction_index)
{
    assert(transaction_index < AIXk_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.size());
    const auto* tx_undo{&AIXk_BlockSpentOutputs::get(block_spent_outputs)->vtxundo.at(transaction_index)};
    return AIXk_TransactionSpentOutputs::ref(tx_undo);
}

void AIXk_block_spent_outputs_destroy(AIXk_BlockSpentOutputs* block_spent_outputs)
{
    delete block_spent_outputs;
}

AIXk_TransactionSpentOutputs* AIXk_transaction_spent_outputs_copy(const AIXk_TransactionSpentOutputs* transaction_spent_outputs)
{
    return AIXk_TransactionSpentOutputs::copy(transaction_spent_outputs);
}

size_t AIXk_transaction_spent_outputs_count(const AIXk_TransactionSpentOutputs* transaction_spent_outputs)
{
    return AIXk_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size();
}

void AIXk_transaction_spent_outputs_destroy(AIXk_TransactionSpentOutputs* transaction_spent_outputs)
{
    delete transaction_spent_outputs;
}

const AIXk_Coin* AIXk_transaction_spent_outputs_get_coin_at(const AIXk_TransactionSpentOutputs* transaction_spent_outputs, size_t coin_index)
{
    assert(coin_index < AIXk_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.size());
    const Coin* coin{&AIXk_TransactionSpentOutputs::get(transaction_spent_outputs).vprevout.at(coin_index)};
    return AIXk_Coin::ref(coin);
}

AIXk_Coin* AIXk_coin_copy(const AIXk_Coin* coin)
{
    return AIXk_Coin::copy(coin);
}

uint32_t AIXk_coin_confirmation_height(const AIXk_Coin* coin)
{
    return AIXk_Coin::get(coin).nHeight;
}

int AIXk_coin_is_coinbase(const AIXk_Coin* coin)
{
    return AIXk_Coin::get(coin).IsCoinBase() ? 1 : 0;
}

const AIXk_TransactionOutput* AIXk_coin_get_output(const AIXk_Coin* coin)
{
    return AIXk_TransactionOutput::ref(&AIXk_Coin::get(coin).out);
}

void AIXk_coin_destroy(AIXk_Coin* coin)
{
    delete coin;
}

int AIXk_chainstate_manager_process_block(
    AIXk_ChainstateManager* chainman,
    const AIXk_Block* block,
    int* _new_block)
{
    bool new_block;
    auto result = AIXk_ChainstateManager::get(chainman).m_chainman->ProcessNewBlock(AIXk_Block::get(block), /*force_processing=*/true, /*min_pow_checked=*/true, /*new_block=*/&new_block);
    if (_new_block) {
        *_new_block = new_block ? 1 : 0;
    }
    return result ? 0 : -1;
}

int AIXk_chainstate_manager_process_block_header(
    AIXk_ChainstateManager* chainstate_manager,
    const AIXk_BlockHeader* header,
    AIXk_BlockValidationState* state)
{
    try {
        auto& chainman = AIXk_ChainstateManager::get(chainstate_manager).m_chainman;
        auto result = chainman->ProcessNewBlockHeaders({&AIXk_BlockHeader::get(header), 1}, /*min_pow_checked=*/true, AIXk_BlockValidationState::get(state), /*ppindex=*/nullptr);

        return result ? 0 : -1;
    } catch (const std::exception& e) {
        LogError("Failed to process block header: %s", e.what());
        return -1;
    }
}

const AIXk_Chain* AIXk_chainstate_manager_get_active_chain(const AIXk_ChainstateManager* chainman)
{
    return AIXk_Chain::ref(&WITH_LOCK(AIXk_ChainstateManager::get(chainman).m_chainman->GetMutex(), return AIXk_ChainstateManager::get(chainman).m_chainman->ActiveChain()));
}

int AIXk_chain_get_height(const AIXk_Chain* chain)
{
    LOCK(::cs_main);
    return AIXk_Chain::get(chain).Height();
}

const AIXk_BlockTreeEntry* AIXk_chain_get_by_height(const AIXk_Chain* chain, int height)
{
    LOCK(::cs_main);
    return AIXk_BlockTreeEntry::ref(AIXk_Chain::get(chain)[height]);
}

int AIXk_chain_contains(const AIXk_Chain* chain, const AIXk_BlockTreeEntry* entry)
{
    LOCK(::cs_main);
    return AIXk_Chain::get(chain).Contains(&AIXk_BlockTreeEntry::get(entry)) ? 1 : 0;
}

AIXk_BlockHeader* AIXk_block_header_create(const void* raw_block_header, size_t raw_block_header_len)
{
    if (raw_block_header == nullptr && raw_block_header_len != 0) {
        return nullptr;
    }
    auto header{std::make_unique<CBlockHeader>()};
    SpanReader stream{std::span{reinterpret_cast<const std::byte*>(raw_block_header), raw_block_header_len}};

    try {
        stream >> *header;
    } catch (...) {
        LogError("Block header decode failed.");
        return nullptr;
    }

    return AIXk_BlockHeader::ref(header.release());
}

AIXk_BlockHeader* AIXk_block_header_copy(const AIXk_BlockHeader* header)
{
    return AIXk_BlockHeader::copy(header);
}

AIXk_BlockHash* AIXk_block_header_get_hash(const AIXk_BlockHeader* header)
{
    return AIXk_BlockHash::create(AIXk_BlockHeader::get(header).GetHash());
}

const AIXk_BlockHash* AIXk_block_header_get_prev_hash(const AIXk_BlockHeader* header)
{
    return AIXk_BlockHash::ref(&AIXk_BlockHeader::get(header).hashPrevBlock);
}

uint32_t AIXk_block_header_get_timestamp(const AIXk_BlockHeader* header)
{
    return AIXk_BlockHeader::get(header).nTime;
}

uint32_t AIXk_block_header_get_bits(const AIXk_BlockHeader* header)
{
    return AIXk_BlockHeader::get(header).nBits;
}

int32_t AIXk_block_header_get_version(const AIXk_BlockHeader* header)
{
    return AIXk_BlockHeader::get(header).nVersion;
}

uint32_t AIXk_block_header_get_nonce(const AIXk_BlockHeader* header)
{
    return AIXk_BlockHeader::get(header).nNonce;
}

void AIXk_block_header_destroy(AIXk_BlockHeader* header)
{
    delete header;
}
