#!/usr/bin/env python3
"""The documented local launcher must restart without replacing identity/state."""
import argparse
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser()
parser.add_argument("--coordinator", required=True)
parser.add_argument("--worker", required=True)
parser.add_argument("--apache-root", required=True)
args = parser.parse_args()
repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="kh-launch-") as directory:
    root = Path(directory) / "environment"
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    command = [sys.executable, str(repo / "tools/coordinator_local.py"), "--directory", str(root),
               "--coordinator", args.coordinator, "--worker", args.worker,
               "--apache-root", args.apache_root, "--port", str(port), "--check"]
    manifests = []
    for _ in range(2):
        result = subprocess.run(command, capture_output=True, text=True, timeout=60)
        assert result.returncode == 0, (result.stdout, result.stderr)
        value = json.loads(result.stdout)
        assert value["ready"]
        manifests.append(json.loads((root / "local.json").read_text()))
        assert not (root / "server/api.sock").exists(), "check left a listener running"
        for name in ("alice", "bob"):
            assert (root / (name + "-worker/progress.sqlite")).is_file()
            assert (root / (name + "-worker.json")).stat().st_mode & 0o777 == 0o600
            assert (root / ("server/" + name + ".key")).stat().st_mode & 0o777 == 0o600
    assert manifests[0] == manifests[1], "restart replaced clients or job identity"
    result = subprocess.run(command[:command.index("--directory")+1] + [str(repo)] + command[command.index("--directory")+2:],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode != 0 and "external private directory" in result.stderr
print("Private localhost setup, authenticated readiness and identity-preserving restart passed")
