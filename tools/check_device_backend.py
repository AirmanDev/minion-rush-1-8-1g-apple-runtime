"""Smoke-test the frozen helper without accounts, assets, or a connected device."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile


def check(executable: Path) -> None:
    with tempfile.TemporaryDirectory(prefix="minion-rush-backend-check.") as workspace:
        process = subprocess.Popen([str(executable), "--workspace", workspace, "check"],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            stdout, stderr = process.communicate(input=b"\n", timeout=30)
            output = stdout.decode("utf-8")
            errors = stderr.decode("utf-8", errors="replace")
            if process.returncode != 0:
                raise ValueError(f"Frozen device helper failed: {output}\n{errors}")
            events = [json.loads(line) for line in output.splitlines() if line]
            requirements = Path(__file__).resolve().parents[1] / "installer/windows/requirements.txt"
            versions = dict(line.split("==", 1) for line in requirements.read_text().splitlines() if line.strip())
            if not events or events[-1].get("device_backend") != versions["pymobiledevice3"]:
                raise ValueError("Unexpected packaged device library version.")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            for stream in (process.stdin, process.stdout, process.stderr):
                if stream is not None:
                    stream.close()


if __name__ == "__main__":
    check(Path(sys.argv[1]))
    print("Frozen device backend smoke test passed")
