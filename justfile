build_dir := "build-ninja"

setup:
  sudo apt update
  sudo apt install -y build-essential ccache pkg-config cmake ninja-build
  just build

configure:
  cmake -S . -B "{{build_dir}}" -G Ninja \
    -DBUILD_DAEMON=ON \
    -DBUILD_CLI=ON \
    -DBUILD_TESTS=OFF \
    -DENABLE_WALLET=ON \
    -DENABLE_IPC=OFF

build: configure
  cmake --build "{{build_dir}}" -j "$(nproc)" --target bitcoind bitcoin-cli

getopenrpc: build
  mkdir -p "{{build_dir}}/regtest-data"

  # stop regtest if running
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 stop 2>/dev/null || true
  sleep 0.5

  # start daemon
  "./{{build_dir}}/bin/bitcoind" -datadir="{{build_dir}}/regtest-data" -regtest -port=19444 -rpcport=19443 -daemon
  sleep 0.5

  # wait for RPC ready
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 -rpcwait getblockchaininfo >/dev/null
  sleep 0.5

  name="openrpc_$(date +%s).json" ;\
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 getopenrpcinfo > "$name" ;\
  echo "Wrote API spec to $name"

thing:
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 decodepsbt cHNidP8BAAoCAAAAAAAAAAAAAA==

thing2:
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 getrawtransaction

estimaterawfee:
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 estimaterawfee

listsinceblock:
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 help listsinceblock

decodepsbt:
  "./{{build_dir}}/bin/bitcoin-cli" -datadir="{{build_dir}}/regtest-data" -regtest -rpcport=19443 help decodepsbt
