#!/usr/bin/env python3
# Copyright (c) 2026 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://opensource.org/license/mit/.

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

from openrpc import build_openrpc


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Generate an OpenRPC document from Bitcoin Core RPC metadata.",
        epilog="You may need to start with double-dash (--) when providing bitcoin-cli arguments.",
    )
    parser.add_argument(
        "-c", "--cmd", default="bitcoin-cli", help="bitcoin-cli command to use"
    )
    parser.add_argument("--output", default="-", help="Output path (default: stdout)")
    parser.add_argument(
        "--input",
        help="Optional input file containing command descriptions JSON. If omitted, query bitcoin-cli help dump_all_command_descriptions.",
    )
    parser.add_argument("bitcoin_cli_args", nargs="*", help="Arguments to pass on to bitcoin-cli")
    args = parser.parse_args()

    if args.input:
        command_descriptions = json.loads(Path(args.input).read_text(encoding="utf-8"))
    else:
        cmd = [args.cmd, *args.bitcoin_cli_args, "help", "dump_all_command_descriptions"]
        try:
            output = subprocess.check_output(cmd, text=True)
        except subprocess.CalledProcessError as e:
            print(f"Failed to get command descriptions: {e}", file=sys.stderr)
            return 1
        command_descriptions = json.loads(output)

    openrpc = build_openrpc(command_descriptions)
    rendered = json.dumps(openrpc, indent=2, sort_keys=True) + "\n"
    if args.output == "-":
        sys.stdout.write(rendered)
    else:
        Path(args.output).write_text(rendered, encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
