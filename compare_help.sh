#!/usr/bin/env bash
#
# compare_help.sh — Compare bitcoin-cli help output (your branch vs master).
#
# Uses git-worktree (non-destructive, doesn't touch your working directory)
# to build master in /tmp and then diffs every RPC help string.
#
set -euo pipefail

REPO_DIR="$(cd "$(dirname "$0")" && pwd)"
BRANCH_BUILD="${REPO_DIR}/build-ninja"

MASTER_WORKTREE="/tmp/btc-master-worktree"
MASTER_BUILD="${MASTER_WORKTREE}/build-ninja"

# --- ports (all different from justfile's 19443/19444) ---
BRANCH_RPC=19551
BRANCH_P2P=19552
MASTER_RPC=19561
MASTER_P2P=19562

BRANCH_DATADIR="/tmp/btc-help-branch"
MASTER_DATADIR="/tmp/btc-help-master"

OUTPUT_DIR="${REPO_DIR}/help-comparison"

# ────────────────────────────────────────────────────────
cleanup() {
  echo ""
  echo "==> Cleaning up..."
  # stop daemons
  "${BRANCH_BUILD}/bin/bitcoin-cli" -regtest -rpcport="${BRANCH_RPC}" \
    -datadir="${BRANCH_DATADIR}" stop 2>/dev/null || true
  "${MASTER_BUILD}/bin/bitcoin-cli" -regtest -rpcport="${MASTER_RPC}" \
    -datadir="${MASTER_DATADIR}" stop 2>/dev/null || true
  sleep 1
  # remove temp datadirs
  rm -rf "${BRANCH_DATADIR}" "${MASTER_DATADIR}"
  echo "==> Done. Worktree kept at ${MASTER_WORKTREE} (remove with: git worktree remove ${MASTER_WORKTREE})"
}
trap cleanup EXIT

# ────────────────────────────────────────────────────────
echo "============================================"
echo " Step 1: Ensure your branch is built"
echo "============================================"
if [[ ! -x "${BRANCH_BUILD}/bin/bitcoind" || ! -x "${BRANCH_BUILD}/bin/bitcoin-cli" ]]; then
  echo "Building your branch..."
  cmake -S "${REPO_DIR}" -B "${BRANCH_BUILD}" -G Ninja \
    -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DBUILD_TESTS=OFF \
    -DENABLE_WALLET=ON -DENABLE_IPC=OFF
  cmake --build "${BRANCH_BUILD}" -j "$(nproc)" --target bitcoind bitcoin-cli
else
  echo "Branch binaries found, skipping build."
fi

echo ""
echo "============================================"
echo " Step 2: Create worktree for master (non-destructive)"
echo "============================================"
if [[ -d "${MASTER_WORKTREE}" ]]; then
  echo "Worktree already exists at ${MASTER_WORKTREE}, reusing it."
else
  if git -C "${REPO_DIR}" worktree list --porcelain | grep -Fxq "worktree ${MASTER_WORKTREE}"; then
    echo "Found stale registration for ${MASTER_WORKTREE}; pruning worktree metadata..."
    git -C "${REPO_DIR}" worktree prune
  fi
  echo "Creating detached worktree at ${MASTER_WORKTREE} from local 'master'..."
  git -C "${REPO_DIR}" worktree add --detach -f "${MASTER_WORKTREE}" master
fi

echo ""
echo "============================================"
echo " Step 3: Build master"
echo "============================================"
if [[ -x "${MASTER_BUILD}/bin/bitcoind" && -x "${MASTER_BUILD}/bin/bitcoin-cli" ]]; then
  echo "Master binaries found, skipping build."
else
  echo "Building master (this may take a while)..."
  cmake -S "${MASTER_WORKTREE}" -B "${MASTER_BUILD}" -G Ninja \
    -DBUILD_DAEMON=ON -DBUILD_CLI=ON -DBUILD_TESTS=OFF \
    -DENABLE_WALLET=ON -DENABLE_IPC=OFF
  cmake --build "${MASTER_BUILD}" -j "$(nproc)" --target bitcoind bitcoin-cli
fi

echo ""
echo "============================================"
echo " Step 4: Start both daemons"
echo "============================================"
rm -rf "${BRANCH_DATADIR}" "${MASTER_DATADIR}"
mkdir -p "${BRANCH_DATADIR}" "${MASTER_DATADIR}"

echo "Starting branch daemon (rpc=${BRANCH_RPC}, p2p=${BRANCH_P2P})..."
"${BRANCH_BUILD}/bin/bitcoind" -regtest \
  -datadir="${BRANCH_DATADIR}" \
  -rpcport="${BRANCH_RPC}" -port="${BRANCH_P2P}" \
  -daemon -noconnect
sleep 2

echo "Starting master daemon (rpc=${MASTER_RPC}, p2p=${MASTER_P2P})..."
"${MASTER_BUILD}/bin/bitcoind" -regtest \
  -datadir="${MASTER_DATADIR}" \
  -rpcport="${MASTER_RPC}" -port="${MASTER_P2P}" \
  -daemon -noconnect
sleep 2

# Wait for both to be ready
echo "Waiting for branch RPC..."
"${BRANCH_BUILD}/bin/bitcoin-cli" -regtest -rpcport="${BRANCH_RPC}" \
  -datadir="${BRANCH_DATADIR}" -rpcwait getblockchaininfo >/dev/null
echo "Waiting for master RPC..."
"${MASTER_BUILD}/bin/bitcoin-cli" -regtest -rpcport="${MASTER_RPC}" \
  -datadir="${MASTER_DATADIR}" -rpcwait getblockchaininfo >/dev/null

echo "Both daemons ready."

echo ""
echo "============================================"
echo " Step 5: Collect help for every command"
echo "============================================"
mkdir -p "${OUTPUT_DIR}/branch" "${OUTPUT_DIR}/master"

# Get the command list from both
BRANCH_CLI="${BRANCH_BUILD}/bin/bitcoin-cli -regtest -rpcport=${BRANCH_RPC} -datadir=${BRANCH_DATADIR}"
MASTER_CLI="${MASTER_BUILD}/bin/bitcoin-cli -regtest -rpcport=${MASTER_RPC} -datadir=${MASTER_DATADIR}"

echo "Fetching command list from branch..."
$BRANCH_CLI help | grep -v '^==' | grep -v '^$' | awk '{print $1}' | sort > "${OUTPUT_DIR}/branch_commands.txt"

echo "Fetching command list from master..."
$MASTER_CLI help | grep -v '^==' | grep -v '^$' | awk '{print $1}' | sort > "${OUTPUT_DIR}/master_commands.txt"

# Merge into a unified list
sort -u "${OUTPUT_DIR}/branch_commands.txt" "${OUTPUT_DIR}/master_commands.txt" > "${OUTPUT_DIR}/all_commands.txt"
total=$(wc -l < "${OUTPUT_DIR}/all_commands.txt")
echo "Total unique commands: ${total}"

echo ""
echo "Dumping help for each command..."
i=0
while IFS= read -r cmd; do
  i=$((i + 1))
  printf "\r  [%d/%d] %s                " "$i" "$total" "$cmd"
  # Branch help (may fail if command doesn't exist in this build)
  $BRANCH_CLI help "$cmd" > "${OUTPUT_DIR}/branch/${cmd}.txt" 2>&1 || true
  # Master help
  $MASTER_CLI help "$cmd" > "${OUTPUT_DIR}/master/${cmd}.txt" 2>&1 || true
done < "${OUTPUT_DIR}/all_commands.txt"
echo ""

echo ""
echo "============================================"
echo " Step 6: Diff results"
echo "============================================"

diff_count=0
only_branch=0
only_master=0
identical=0
report="${OUTPUT_DIR}/report.txt"
> "$report"

while IFS= read -r cmd; do
  bf="${OUTPUT_DIR}/branch/${cmd}.txt"
  mf="${OUTPUT_DIR}/master/${cmd}.txt"

  # Check if command is missing from one side
  if ! grep -qx "$cmd" "${OUTPUT_DIR}/branch_commands.txt"; then
    echo "[ONLY IN MASTER] ${cmd}" >> "$report"
    only_master=$((only_master + 1))
    continue
  fi
  if ! grep -qx "$cmd" "${OUTPUT_DIR}/master_commands.txt"; then
    echo "[ONLY IN BRANCH] ${cmd}" >> "$report"
    only_branch=$((only_branch + 1))
    continue
  fi

  if ! diff -q "$bf" "$mf" >/dev/null 2>&1; then
    echo "" >> "$report"
    echo "========== DIFF: ${cmd} ==========" >> "$report"
    diff -u "$mf" "$bf" >> "$report" 2>&1 || true
    diff_count=$((diff_count + 1))
  else
    identical=$((identical + 1))
  fi
done < "${OUTPUT_DIR}/all_commands.txt"

echo ""
echo "============================================"
echo " SUMMARY"
echo "============================================"
echo "  Identical:        ${identical}"
echo "  Different:        ${diff_count}"
echo "  Only in branch:   ${only_branch}"
echo "  Only in master:   ${only_master}"
echo ""
echo "Full report:        ${OUTPUT_DIR}/report.txt"
echo "Branch help files:  ${OUTPUT_DIR}/branch/"
echo "Master help files:  ${OUTPUT_DIR}/master/"
echo ""

if [[ ${diff_count} -gt 0 || ${only_branch} -gt 0 || ${only_master} -gt 0 ]]; then
  echo "⚠  Differences found! Check the report."
  exit 1
else
  echo "✓  All help strings are identical."
  exit 0
fi
