#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check that getopenrpcinfo RPC is callable and serializable as valid json."""

import json

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


def find_method(openrpc, name):
    return next(method for method in openrpc["methods"] if method["name"] == name)


def find_param(method, name):
    return next(param for param in method["params"] if param["name"] == name)


class OpenRPCDocTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        self.log.info("Calling getopenrpcinfo")
        openrpc = self.nodes[0].getopenrpcinfo()
        json.dumps(openrpc)
        assert_equal(type(openrpc["openrpc"]).__name__, "str")
        assert_equal(type(openrpc["info"]).__name__, "dict")
        assert_equal(type(openrpc["methods"]).__name__, "list")

        stop = find_method(openrpc, "stop")
        assert "wait" not in [param["name"] for param in stop["params"]]

        self.log.info("Calling getopenrpcinfo with hidden commands")
        openrpc_with_hidden = self.nodes[0].getopenrpcinfo(True)
        stop_with_hidden = find_method(openrpc_with_hidden, "stop")
        wait = find_param(stop_with_hidden, "wait")
        assert_equal(wait["schema"]["type"], "number")

        self.log.info("Checking type_str override schemas")
        getblockstats = find_method(openrpc, "getblockstats")
        hash_or_height = find_param(getblockstats, "hash_or_height")
        assert_equal(hash_or_height["schema"], {"oneOf": [{"type": "number"}, {"type": "string"}]})

        self.log.info("Checking argument fallback annotations")
        getblock = find_method(openrpc, "getblock")
        verbosity = find_param(getblock, "verbosity")
        assert_equal(verbosity["schema"]["default"], 1)
        stats = find_param(getblockstats, "stats")
        assert_equal(stats["schema"]["x-bitcoin-default-hint"], "all values")

        self.log.info("Checking string amount result annotations")
        analyzepsbt = find_method(openrpc, "analyzepsbt")
        result_schema = analyzepsbt["result"]["schema"]
        estimated_feerate = result_schema["properties"]["estimated_feerate"]
        assert_equal(estimated_feerate["type"], "string")
        assert_equal(estimated_feerate["x-bitcoin-unit"], "amount")


if __name__ == "__main__":
    OpenRPCDocTest(__file__).main()
