#!/usr/bin/env python3
"""Run CTest directly as the current user."""
import argparse
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--test-dir", required=True)
parser.add_argument("--launcher-timeout", type=float, default=600)
parser.add_argument("arguments", nargs=argparse.REMAINDER)
args = parser.parse_args()
arguments = args.arguments
if arguments[:1] == ["--"]:
    arguments = arguments[1:]
try:
    raise SystemExit(subprocess.run(["ctest", "--test-dir", args.test_dir, *arguments], timeout=args.launcher_timeout).returncode)
except subprocess.TimeoutExpired:
    raise SystemExit(124)
