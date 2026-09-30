#!/usr/bin/env python3
"""Exercise the shipped Apache template with real TLS, not mocked headers."""
import argparse
import base64
import http.client
import json
import os
from pathlib import Path
import socket
import ssl
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from coordinator_local import Environment, HOST


def request(connection, path="/api/v1/projects", headers=None, body=None):
    values = dict(headers or {})
    if body is not None:
        values["Content-Type"] = "application/json"
    connection.request("GET" if body is None else "POST", path, body=body, headers=values)
    response = connection.getresponse()
    payload = response.read()
    return response.status, payload


def raw(path, message):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(20)
        sock.connect(str(path))
        sock.sendall(message.encode())
        return sock.recv(32768).split(b" ")[1]


def test(env):
    a = env.admin("bootstrap", name="owner", certificate=(env.directory / "alice.pem").read_text())
    b = env.admin("client-add", name="other", certificate=(env.directory / "bob.pem").read_text())
    p = env.admin("project-create", name="private", owner=a["client"])["project"]
    env.admin("project-create", name="other", owner=b["client"])
    with env.client("alice") as client:
        status, data = request(client)
        assert status == 200, (status, data)
        assert [row["project"] for row in json.loads(data)["value"]] == [p]
        retained = client.sock
        assert retained is not None, "frontend connection was not persistent"
        env.admin("credential-set", fingerprint=a["fingerprint"], enabled=False)
        assert request(client)[0] == 401
        assert client.sock is retained, "revocation test silently opened a new TLS connection"
    env.admin("credential-add", client=a["client"], certificate=(env.directory / "rotation.pem").read_text())
    with env.client("rotation") as client:
        assert request(client)[0] == 200
    for name in (None, "wrong"):
        with env.client(name) as client:
            try:
                assert request(client)[0] >= 400
            except (ssl.SSLError, ConnectionError, http.client.RemoteDisconnected):
                pass
    with env.client("unknown") as client:
        assert request(client)[0] == 401
    with env.client("bob") as client:
        # Apache must replace every supplied copy, even duplicate names. Bob's
        # verified connection must not become Alice's enrolled identity.
        client.putrequest("GET", "/api/v1/projects")
        for _ in range(2):
            client.putheader("X-Keyhunt-Cert", base64.b64encode((env.directory / "rotation.pem").read_bytes()).decode())
            client.putheader("X-Keyhunt-TLS-Verify", "SUCCESS")
            client.putheader("X-Keyhunt-TLS-SNI", HOST)
        client.endheaders()
        response = client.getresponse()
        payload = response.read()
        assert response.status == 200, payload
        assert p not in [row["project"] for row in json.loads(payload)["value"]]
        assert request(client, headers={"Host": "wrong.invalid"})[0] in (403, 421)
        assert request(client, "/admin", body='{"operation":"clients"}')[0] >= 400
        assert request(client, headers={"Early-Data": "1"})[0] == 425
        assert request(client, "/api/v1/projects/" + p + "/jobs", body="{}")[0] == 404
        assert request(client, "/api/v1/projects/" + p + "/jobs", body='{"a":1,"a":2}')[0] == 400
    # Mismatched SNI still supplies the correct Host; disable only the test's
    # server-hostname check so it reaches Apache's explicit SNI policy.
    with env.client("bob", host="wrong.invalid") as client:
        client._context.check_hostname = False
        assert request(client, headers={"Host": env.authority})[0] in (403, 421)
    assert raw(env.api, "GET /api/v1/projects HTTP/1.1\r\nHost: " + env.authority + "\r\n\r\n") == b"401"
    assert raw(env.api, "GET /api/v1/projects HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n") == b"400"
    assert raw(env.api, "POST /api/v1/sync HTTP/1.1\r\nHost: a\r\nContent-Length: 8388609\r\n\r\n") == b"413"
    assert (env.api.stat().st_mode & 0o777) == 0o660
    assert (env.admin_socket.stat().st_mode & 0o777) == 0o600
    env.admin("check")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--coordinator", required=True)
    parser.add_argument("--apache-root", default="/")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="kh-tls-") as directory:
        env = Environment(directory, args.coordinator, args.apache_root)
        try:
            env.initialize()
            env.start()
            test(env)
            env.stop()
            env.start(proxy_uid=os.getuid() + 1)
            assert raw(env.api, "GET /api/v1/projects HTTP/1.1\r\nHost: local\r\n\r\n") == b"403"
            env.admin("check")
        except BaseException:
            for file in Path(directory).glob("*.log"):
                print(file.name, file.read_text()[-8000:], file=sys.stderr)
            raise
        finally:
            env.stop()
    print("Apache mTLS, header replacement, socket peer policy and persistent-connection revocation passed")


if __name__ == "__main__":
    main()
