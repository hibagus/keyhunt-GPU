#!/usr/bin/env python3
"""Isolated Apache/mTLS fixture; all runtime state stays in a private directory.

This module also supports the integration suite. It never edits DNS, /etc/hosts,
system services or trusted CAs. Its test CA must not be used on public ingress.
"""
import http.client
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import time

HOST = "dbkeyprogress.rumahsimanis.bagus.my.id"
REPO = Path(__file__).resolve().parents[1]


class LocalHTTPS(http.client.HTTPSConnection):
    """Connect to loopback while retaining the real Host, SNI and CA checks."""
    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()

    def connect(self):
        raw = socket.create_connection(("127.0.0.1", self.port), self.timeout)
        self.sock = self._context.wrap_socket(raw, server_hostname=self.host)


def admin(path, body):
    connection = http.client.HTTPConnection("local", timeout=20)
    connection.sock = socket.socket(socket.AF_UNIX)
    connection.sock.settimeout(20)
    connection.sock.connect(str(path))
    try:
        connection.request("POST", "/admin", json.dumps(body), {"Content-Type": "application/json"})
        response = connection.getresponse()
        value = json.loads(response.read())
        if response.status != 200:
            raise RuntimeError(value)
        return value["value"]
    finally:
        connection.close()


class Environment:
    def __init__(self, directory, executable, apache_root="/"):
        self.directory = Path(directory).resolve()
        self.executable = str(Path(executable).resolve())
        self.apache_root = Path(apache_root)
        self.processes = []
        self.logs = []
        self.port = 0

    def command(self, *args):
        subprocess.run(args, cwd=self.directory, check=True, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE)

    def certificate(self, name, ca, server=False):
        key, csr, cert = (self.directory / (name + suffix) for suffix in (".key", ".csr", ".pem"))
        self.command("openssl", "req", "-new", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                     "-nodes", "-subj", "/CN=" + (HOST if server else "same-untrusted-name"),
                     "-keyout", str(key), "-out", str(csr))
        os.chmod(key, 0o600)
        extensions = self.directory / (name + ".ext")
        extensions.write_text("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature\n"
                              + "extendedKeyUsage=" + ("serverAuth" if server else "clientAuth") + "\n"
                              + ("subjectAltName=DNS:" + HOST + "\n" if server else ""))
        self.command("openssl", "x509", "-req", "-in", str(csr), "-CA", str(self.directory / (ca + ".pem")),
                     "-CAkey", str(self.directory / (ca + ".key")), "-CAcreateserial", "-days", "60",
                     "-extfile", str(extensions), "-out", str(cert))

    def initialize(self):
        self.directory.mkdir(mode=0o700, parents=True, exist_ok=True)
        os.chmod(self.directory, 0o700)
        for ca in ("server-ca", "client-ca", "wrong-ca"):
            self.command("openssl", "req", "-x509", "-newkey", "ec", "-pkeyopt", "ec_paramgen_curve:P-256",
                         "-nodes", "-days", "60", "-subj", "/CN=isolated-" + ca,
                         "-addext", "basicConstraints=critical,CA:TRUE",
                         "-addext", "keyUsage=critical,keyCertSign,cRLSign",
                         "-keyout", str(self.directory / (ca + ".key")), "-out", str(self.directory / (ca + ".pem")))
            os.chmod(self.directory / (ca + ".key"), 0o600)
        self.certificate("server", "server-ca", server=True)
        for name in ("alice", "bob", "rotation", "unknown"):
            self.certificate(name, "client-ca")
        self.certificate("wrong", "wrong-ca")

    def launch(self, name, args, env=None):
        log = open(self.directory / (name + ".log"), "ab", buffering=0)
        self.logs.append(log)
        process = subprocess.Popen(args, cwd=self.directory, stdout=log, stderr=log, env=env)
        self.processes.append(process)
        return process

    def start(self, proxy_uid=None):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            self.port = probe.getsockname()[1]
        self.authority = f"{HOST}:{self.port}"
        self.api = self.directory / "api.sock"
        self.admin_socket = self.directory / "admin.sock"
        service = self.launch("coordinator", [self.executable, "serve", "--state-dir", str(self.directory / "state"),
                              "--api-socket", str(self.api), "--admin-socket", str(self.admin_socket),
                              "--proxy-uid", str(os.getuid() if proxy_uid is None else proxy_uid),
                              "--authority", self.authority])
        for _ in range(100):
            if self.admin_socket.exists():
                break
            if service.poll() is not None:
                raise RuntimeError((self.directory / "coordinator.log").read_text())
            time.sleep(.05)
        else:
            raise RuntimeError("coordinator did not start")
        module_dir = self.apache_root / "usr/lib/apache2/modules"
        modules = ["mpm_event", "authz_core", "ssl", "socache_shmcb", "proxy", "proxy_http", "headers", "reqtimeout"]
        config = f'ServerRoot "{self.directory}"\nDefaultRuntimeDir "{self.directory}"\n'
        config += f'PidFile "{self.directory}/apache.pid"\nErrorLog "{self.directory}/apache-error.log"\n'
        config += f"ServerName {HOST}\nUser #{os.getuid()}\nGroup #{os.getgid()}\nKeepAlive On\n"
        config += "".join(f'LoadModule {module}_module "{module_dir}/mod_{module}.so"\n' for module in modules)
        template = (REPO / "deploy/apache/keyhunt.conf.in").read_text()
        substitutions = dict(LISTEN_ADDRESS="127.0.0.1", PORT=self.port, HOST=HOST, AUTHORITY=self.authority,
                             SERVER_CERT=self.directory / "server.pem", SERVER_KEY=self.directory / "server.key",
                             CLIENT_CA=self.directory / "client-ca.pem", API_SOCKET=self.api)
        for key, value in substitutions.items():
            template = template.replace("@" + key + "@", str(value))
        conf = self.directory / "apache.conf"
        conf.write_text(config + template)
        env = os.environ.copy()
        env["LD_LIBRARY_PATH"] = str(self.apache_root / "usr/lib/x86_64-linux-gnu") + ":" + env.get("LD_LIBRARY_PATH", "")
        apache = self.launch("apache", [str(self.apache_root / "usr/sbin/apache2"), "-f", str(conf), "-DFOREGROUND"], env)
        for _ in range(100):
            if apache.poll() is not None:
                raise RuntimeError((self.directory / "apache.log").read_text())
            try:
                with socket.create_connection(("127.0.0.1", self.port), timeout=.1):
                    return
            except OSError:
                time.sleep(.05)
        raise RuntimeError("Apache did not start")

    def client(self, name=None, host=HOST):
        context = ssl.create_default_context(cafile=str(self.directory / "server-ca.pem"))
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        if name:
            context.load_cert_chain(str(self.directory / (name + ".pem")), str(self.directory / (name + ".key")))
        return LocalHTTPS(host, self.port, context=context, timeout=20)

    def admin(self, operation, **kwargs):
        return admin(self.admin_socket, dict(operation=operation, **kwargs))

    def stop(self):
        for process in reversed(self.processes):
            process.terminate()
            try:
                process.wait(timeout=20)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        self.processes.clear()
        for log in self.logs:
            log.close()
        self.logs.clear()
