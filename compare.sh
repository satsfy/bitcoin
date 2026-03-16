#!/usr/bin/env bash
set -euo pipefail

LOCAL_CLI="./build-ninja/bin/bitcoin-cli"
LOCAL_DAEMON="./build-ninja/bin/bitcoind"
SYSTEM_CLI="bitcoin-cli"

DATADIR="${COMPARE_DATADIR:-/tmp/btc-compare-regtest}"
RPC_PORT="${COMPARE_RPC_PORT:-19453}"
P2P_PORT="${COMPARE_P2P_PORT:-19454}"
WALLET_NAME="${COMPARE_WALLET_NAME:-w1}"
STARTED_DAEMON=0

local_cli() {
    "${LOCAL_CLI}" -regtest -datadir="${DATADIR}" -rpcport="${RPC_PORT}" "$@"
}

system_cli() {
    "${SYSTEM_CLI}" -regtest -datadir="${DATADIR}" -rpcport="${RPC_PORT}" "$@"
}

cleanup() {
    if [[ "${STARTED_DAEMON}" -eq 1 ]]; then
        local_cli stop >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

require_binaries() {
    if [[ ! -x "${LOCAL_CLI}" || ! -x "${LOCAL_DAEMON}" ]]; then
        echo "Missing build binaries in ./build-ninja/bin."
        echo "Build first, e.g.: cmake --build build-ninja --target bitcoind bitcoin-cli"
        exit 1
    fi
    if ! command -v "${SYSTEM_CLI}" >/dev/null 2>&1; then
        echo "System bitcoin-cli not found in PATH."
        exit 1
    fi
}

start_daemon_if_needed() {
    mkdir -p "${DATADIR}"
    if local_cli getblockchaininfo >/dev/null 2>&1; then
        return
    fi

    "${LOCAL_DAEMON}" -regtest -daemon \
        -datadir="${DATADIR}" \
        -rpcport="${RPC_PORT}" \
        -port="${P2P_PORT}" \
        -server=1 \
        -fallbackfee=0.0002 \
        -noconnect

    local_cli -rpcwait getblockchaininfo >/dev/null
    STARTED_DAEMON=1
}

ensure_wallet_loaded() {
    if local_cli -rpcwallet="${WALLET_NAME}" getwalletinfo >/dev/null 2>&1; then
        return
    fi

    local_cli createwallet "${WALLET_NAME}" >/dev/null 2>&1 || true
    local_cli loadwallet "${WALLET_NAME}" >/dev/null 2>&1 || true
    local_cli -rpcwallet="${WALLET_NAME}" getwalletinfo >/dev/null
}

ensure_chain_has_blocks() {
    local height addr blocks_needed
    height="$(local_cli getblockcount)"
    if [[ "${height}" -ge 110 ]]; then
        return
    fi

    blocks_needed="$((110 - height))"
    addr="$(local_cli -rpcwallet="${WALLET_NAME}" getnewaddress)"
    local_cli generatetoaddress "${blocks_needed}" "${addr}" >/dev/null
}

get_transaction_sample() {
    local addr txid
    addr="$(local_cli -rpcwallet="${WALLET_NAME}" getnewaddress)"
    txid="$(local_cli -rpcwallet="${WALLET_NAME}" sendtoaddress "${addr}" 1)"
    local_cli -rpcwallet="${WALLET_NAME}" generatetoaddress 1 "${addr}" >/dev/null
    local_cli -rpcwallet="${WALLET_NAME}" gettransaction "${txid}"
    system_cli -rpcwallet="${WALLET_NAME}" gettransaction "${txid}"
    echo "txid: ${txid}"
}

get_raw_transaction_sample() {
    local addr txid blockhash
    addr="$(local_cli -rpcwallet="${WALLET_NAME}" getnewaddress)"
    txid="$(local_cli -rpcwallet="${WALLET_NAME}" sendtoaddress "${addr}" 1)"
    local_cli -rpcwallet="${WALLET_NAME}" generatetoaddress 1 "${addr}" >/dev/null
    blockhash="$(local_cli -rpcwallet="${WALLET_NAME}" gettransaction "${txid}" | sed -n 's/.*"blockhash"[[:space:]]*:[[:space:]]*"\([^"]*\)".*/\1/p')"
    if [[ -n "${blockhash}" ]]; then
        local_cli -rpcwallet="${WALLET_NAME}" getrawtransaction "${txid}" 1 "${blockhash}"
        system_cli -rpcwallet="${WALLET_NAME}" getrawtransaction "${txid}" 1 "${blockhash}"
    else
        local_cli -rpcwallet="${WALLET_NAME}" getrawtransaction "${txid}" 1 || true
        system_cli -rpcwallet="${WALLET_NAME}" getrawtransaction "${txid}" 1 || true
    fi
    echo "blockhash: ${blockhash}"
    echo "txid: ${txid}"
}

decoderawtransaction_sample() {
    local addr txid rawhex
    addr="$(local_cli -rpcwallet="${WALLET_NAME}" getnewaddress)"
    txid="$(local_cli -rpcwallet="${WALLET_NAME}" sendtoaddress "${addr}" 1)"
    local_cli -rpcwallet="${WALLET_NAME}" generatetoaddress 1 "${addr}" >/dev/null
    rawhex="$(local_cli -rpcwallet="${WALLET_NAME}" getrawtransaction "${txid}" 0 || true)"
    if [[ -n "${rawhex}" ]]; then
        local_cli -rpcwallet="${WALLET_NAME}" decoderawtransaction "${rawhex}"
        system_cli -rpcwallet="${WALLET_NAME}" decoderawtransaction "${rawhex}"
    else
        echo "failed to obtain raw transaction hex for txid: ${txid}"
    fi
    echo "txid: ${txid}"
}

get_block() {
    local block blockhash
    block="$(local_cli getblockcount)"
    blockhash="$(local_cli getblockhash "${block}")"
    echo "${LOCAL_CLI} -regtest -datadir=${DATADIR} -rpcport=${RPC_PORT} -rpcwallet=${WALLET_NAME} getblock \"${blockhash}\""
    system_cli -rpcwallet="${WALLET_NAME}" getblock "${blockhash}" 0 > getblock_verbosity_0.json
    system_cli -rpcwallet="${WALLET_NAME}" getblock "${blockhash}" 1 > getblock_verbosity_1.json
    system_cli -rpcwallet="${WALLET_NAME}" getblock "${blockhash}" 2 > getblock_verbosity_2.json
    system_cli -rpcwallet="${WALLET_NAME}" getblock "${blockhash}" 3 > getblock_verbosity_3.json
    echo "blockhash: ${blockhash}"
}

compare() {
    local method outfile_local outfile_system
    method="$1"
    shift
    outfile_local="local_cli_${method}.json"
    outfile_system="system_cli_${method}.json"

    local_cli -rpcwallet="${WALLET_NAME}" "${method}" "$@" &> "${outfile_local}"
    system_cli -rpcwallet="${WALLET_NAME}" "${method}" "$@" &> "${outfile_system}"
    echo "Comparing outputs... ${outfile_local} ${outfile_system}"
    echo "============================="
    diff "${outfile_local}" "${outfile_system}"
}

main() {
    local output blockhash
    require_binaries
    start_daemon_if_needed
    ensure_wallet_loaded
    ensure_chain_has_blocks

    output="$(get_block)"
    blockhash="$(echo "${output}" | sed -n 's/.*blockhash: \([^[:space:]]*\).*/\1/p')"
    compare "getblock" "${blockhash}"
}

main "$@"
