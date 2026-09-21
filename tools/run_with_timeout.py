#!/usr/bin/env python3
"""Run one foreground command with a graceful external timeout."""

from __future__ import annotations

import argparse
import signal
import subprocess
import sys


def terminate(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    process.terminate()
    try:
        process.wait(timeout=2.0)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def run(command: list[str], timeout: float) -> int:
    process = subprocess.Popen(command)

    def forward(signum: int, _frame: object) -> None:
        if process.poll() is None:
            process.send_signal(signum)

    previous = {
        signum: signal.signal(signum, forward)
        for signum in (signal.SIGINT, signal.SIGTERM)
    }
    try:
        try:
            return process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            terminate(process)
            return 124
    finally:
        for signum, handler in previous.items():
            signal.signal(signum, handler)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("timeout", type=float)
    parser.add_argument("command", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if args.timeout <= 0 or not args.command:
        parser.error("a positive timeout and a command are required")
    return run(args.command, args.timeout)


if __name__ == "__main__":
    raise SystemExit(main())
