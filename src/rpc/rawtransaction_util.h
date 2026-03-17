// Copyright (c) 2017-present The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_RAWTRANSACTION_UTIL_H
#define BITCOIN_RPC_RAWTRANSACTION_UTIL_H

#include <addresstype.h>
#include <consensus/amount.h>
#include <rpc/util.h>
#include <map>
#include <string>
#include <optional>

struct bilingual_str;
struct FlatSigningProvider;
class UniValue;
struct CMutableTransaction;
class Coin;
class COutPoint;
class SigningProvider;

/**
 * Sign a transaction with the given keystore and previous transactions
 *
 * @param  mtx           The transaction to-be-signed
 * @param  keystore      Temporary keystore containing signing keys
 * @param  coins         Map of unspent outputs
 * @param  hashType      The signature hash type
 * @param result         JSON object where signed transaction results accumulate
 */
void SignTransaction(CMutableTransaction& mtx, const SigningProvider* keystore, const std::map<COutPoint, Coin>& coins, const UniValue& hashType, UniValue& result);
void SignTransactionResultToJSON(CMutableTransaction& mtx, bool complete, const std::map<COutPoint, Coin>& coins, const std::map<int, bilingual_str>& input_errors, UniValue& result);

/**
  * Parse a prevtxs UniValue array and get the map of coins from it
  *
  * @param  prevTxsUnival Array of previous txns outputs that tx depends on but may not yet be in the block chain
  * @param  keystore      A pointer to the temporary keystore if there is one
  * @param  coins         Map of unspent outputs - coins in mempool and current chain UTXO set, may be extended by previous txns outputs after call
  */
void ParsePrevouts(const UniValue& prevTxsUnival, FlatSigningProvider* keystore, std::map<COutPoint, Coin>& coins);

/** Normalize univalue-represented inputs and add them to the transaction */
void AddInputs(CMutableTransaction& rawTx, const UniValue& inputs_in, bool rbf);

/** Normalize univalue-represented outputs */
UniValue NormalizeOutputs(const UniValue& outputs_in);

/** Parse normalized outputs into destination, amount tuples */
std::vector<std::pair<CTxDestination, CAmount>> ParseOutputs(const UniValue& outputs);

/** Normalize, parse, and add outputs to the transaction */
void AddOutputs(CMutableTransaction& rawTx, const UniValue& outputs_in);

/** Create a transaction from univalue parameters */
CMutableTransaction ConstructTransaction(const UniValue& inputs_in, const UniValue& outputs_in, const UniValue& locktime, std::optional<bool> rbf, uint32_t version);

/// Options controlling some fields in TxDoc(). Callers only need to name the ones they enable:
struct TxDocOptions {
    /// Include prevout field
    bool prevout{false};
    bool prevout_required{false};
    /// Include fee field
    bool fee{false};
    /// Include hex field
    bool hex{false};
    /// Include wallet-related fields (e.g. ischange on outputs)
    bool wallet{false};

    /// Customize a field's doc string
    std::string txid_field_doc{"The transaction id"};
    std::string vin_item_doc{"utxo being spent"};
    std::string prevout_doc{"The previous output, omitted if block undo data is not available"};
    std::optional<std::string> fee_doc{};

    /// Elide the entire tx object (top-level fields hidden after summary).
    /// When vin_inner_elision is also set, the vin array is kept visible.
    std::optional<std::string> top_level_elision{};
    /// When true, silently elide all top-level fields with no summary text.
    /// Mutually exclusive with top_level_elision.
    bool top_level_elision_silent{false};
    /// Elide vin inner fields but keep vin array with prevout expanded.
    std::optional<std::string> vin_inner_elision{};
};

/// Describe the transaction object.
/// Some fields are adjusted according to @p opts.
std::vector<RPCResult> TxDoc(const TxDocOptions& opts = {});

#endif // BITCOIN_RPC_RAWTRANSACTION_UTIL_H
