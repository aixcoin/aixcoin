#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Check that doc/openrpc.json is in sync with RPC metadata."""

import json
import sys
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework


class OpenRPCDocTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()
        self.skip_if_no_bitcoind_zmq()
        self.skip_if_no_external_signer()

    def run_test(self):
        srcdir = Path(self.config["environment"]["SRCDIR"])
        sys.path.insert(0, str(srcdir / "contrib" / "devtools"))
        from openrpc import build_openrpc  # ty: ignore[unresolved-import]

        self.log.info("Collecting structured RPC command descriptions")
        descriptions = self.nodes[0].help("dump_all_command_descriptions")
        generated = build_openrpc(descriptions)

        openrpc_path = srcdir / "doc" / "openrpc.json"
        committed = json.loads(openrpc_path.read_text(encoding="utf-8"))

        if generated != committed:
            self.log.info("Generated doc/openrpc.json:\n%s",
                          json.dumps(generated, indent=2, sort_keys=True))
            raise AssertionError(
                f"{openrpc_path} is out of date.\n"
                "To regenerate, see contrib/devtools/README.md#gen-openrpcpy"
            )


if __name__ == "__main__":
    OpenRPCDocTest(__file__).main()
