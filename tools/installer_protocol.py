"""Shared JSON Lines events, complete operation logs, and workspace locking."""

from __future__ import annotations

import contextlib
import io
import json
import os
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path
from typing import Iterator

PROTOCOL = 1
PROTOCOL_OUTPUT = sys.stdout
OPERATION_LOG: io.TextIOBase | None = None


def emit(kind: str, **fields: object) -> None:
    if OPERATION_LOG is not None:
        message = fields.get("message")
        if isinstance(message, str):
            OPERATION_LOG.write((message if kind == "log" else f"[{kind}] {message}") + "\n")
        elif kind == "result":
            OPERATION_LOG.write("Operation completed successfully.\n")
    print(json.dumps({"protocol": PROTOCOL, "type": kind, **fields}),
          file=PROTOCOL_OUTPUT, flush=True)


@contextlib.contextmanager
def operation_log(workspace: Path) -> Iterator[None]:
    global OPERATION_LOG
    if workspace.is_symlink():
        raise ValueError("The installer workspace must not be a symbolic link.")
    directory = workspace / "logs"
    if directory.is_symlink():
        raise ValueError("The log directory must not be a symbolic link.")
    directory.mkdir(parents=True, exist_ok=True, mode=0o700)
    timestamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    path = directory / f"{timestamp}-{uuid.uuid4().hex}.log"
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    previous = OPERATION_LOG
    with os.fdopen(descriptor, "w", encoding="utf-8", buffering=1) as output:
        OPERATION_LOG = output
        try:
            emit("session", log_path=str(path))
            yield
        finally:
            OPERATION_LOG = previous


class LogStream(io.TextIOBase):
    def write(self, value: str) -> int:
        for line in value.splitlines():
            if line:
                emit("log", message=line)
        return len(value)


@contextlib.contextmanager
def workspace_lock(workspace: Path) -> Iterator[None]:
    if workspace.is_symlink():
        raise ValueError("The installer workspace must not be a symbolic link.")
    workspace.mkdir(parents=True, exist_ok=True)
    with (workspace / ".lock").open("a+") as lock:
        try:
            if os.name == "nt":
                import msvcrt
                lock.seek(0)
                if lock.read(1) == "":
                    lock.write(" ")
                    lock.flush()
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise ValueError("Another installer operation is running.") from exc
        yield
