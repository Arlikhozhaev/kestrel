#!/usr/bin/env python3
"""Build one chat-completions body. bench/lua/chat_completion.lua matches this byte for byte."""

import argparse
import sys


def content(prompt_id: int, nbytes: int) -> str:
    prefix = f"k{prompt_id:08d}"
    if nbytes < len(prefix):
        raise SystemExit(f"payload {nbytes} is shorter than the {len(prefix)}-byte id prefix")
    return prefix + ("x" * (nbytes - len(prefix)))


def body(prompt_id: int, nbytes: int) -> str:
    return (
        '{"model":"m","messages":[{"role":"user","content":"'
        + content(prompt_id, nbytes)
        + '"}]}'
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="print one POST body")
    parser.add_argument("--id", type=int, required=True)
    parser.add_argument("--bytes", type=int, required=True)
    args = parser.parse_args()
    if args.id < 0 or args.bytes < 9 or args.bytes > 8192:
        print("id must be >= 0 and bytes must be 9..8192", file=sys.stderr)
        return 2
    sys.stdout.write(body(args.id, args.bytes))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
