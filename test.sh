# # 0) start clean
# rm -rf /tmp/erf-regtest
# bitcoind -regtest -daemon -datadir=/tmp/erf-regtest \
#   -fallbackfee=0.0002 \
#   -server=1

# # 1) create/load wallet and get mature coins
# bitcoin-cli -regtest -datadir=/tmp/erf-regtest createwallet "w"
# ADDR=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcwallet=w getnewaddress)
# bitcoin-cli -regtest -datadir=/tmp/erf-regtest generatetoaddress 110 "$ADDR"

# # 2) build tx history at multiple feerates
# #    For each round:
# #      - create 3 txs with different fee rates
# #      - mine a block
# #    Repeat enough times so the estimator has data.

# for i in $(seq 1 40); do
#   A1=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcwallet=w getnewaddress)
#   A2=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcwallet=w getnewaddress)
#   A3=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcwallet=w getnewaddress)

#   bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcwallet=w -named sendtoaddress address="$A1" amount=0.01 fee_rate=1
#   bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcwallet=w -named sendtoaddress address="$A2" amount=0.01 fee_rate=5
#   bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcwallet=w -named sendtoaddress address="$A3" amount=0.01 fee_rate=20

#   # sometimes leave one tx unconfirmed for a round to create fail-side data
#   if [ $((i % 5)) -ne 0 ]; then
#     bitcoin-cli -regtest -datadir=/tmp/erf-regtest generatetoaddress 1 "$ADDR"
#   fi
# done

# # 3) age the estimator a bit more
# bitcoin-cli -regtest -datadir=/tmp/erf-regtest generatetoaddress 25 "$ADDR"

# # 4) inspect raw output
# bitcoin-cli -regtest -datadir=/tmp/erf-regtest estimaterawfee 6 0.95
# bitcoin-cli -regtest -datadir=/tmp/erf-regtest estimaterawfee 12 0.95
# bitcoin-cli -regtest -datadir=/tmp/erf-regtest estimaterawfee 24 0.95

mkdir -p /tmp/erf-regtest

bitcoind -regtest -daemon -datadir=/tmp/erf-regtest \
  -fallbackfee=0.0002 \
  -server=1 \
  -rpcport=19453 \
  -port=19454

bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwait createwallet "w"
ADDR=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwallet=w getnewaddress)
bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 generatetoaddress 110 "$ADDR"

for i in $(seq 1 40); do
  A1=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwallet=w getnewaddress)
  A2=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwallet=w getnewaddress)
  A3=$(bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwallet=w getnewaddress)

  bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwallet=w -named sendtoaddress address="$A1" amount=0.01 fee_rate=1
  bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwallet=w -named sendtoaddress address="$A2" amount=0.01 fee_rate=5
  bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 -rpcwallet=w -named sendtoaddress address="$A3" amount=0.01 fee_rate=20

  if [ $((i % 5)) -ne 0 ]; then
    bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 generatetoaddress 1 "$ADDR"
  fi
done

bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 generatetoaddress 25 "$ADDR"

bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 estimaterawfee 6 0.95
bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 estimaterawfee 12 0.95
bitcoin-cli -regtest -datadir=/tmp/erf-regtest -rpcport=19453 estimaterawfee 24 0.95
