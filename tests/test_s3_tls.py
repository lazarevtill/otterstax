"""Exercise S3 TLS through SQL against an owned HTTPS object fixture and server.

Run with --server /path/to/server. Requires psycopg and openssl. Credentials are
generated in memory and supplied through environment references in configuration.
"""

import argparse
import contextlib
import http.server
import json
import os
from pathlib import Path
import secrets
import socket
import ssl
import subprocess
import tempfile
import threading
import time

import psycopg


def free_port():
    with socket.socket() as listener:
        listener.bind(("127.0.0.1", 0))
        return listener.getsockname()[1]


def certificate(root, name, hostname):
    cert, key = root / f"{name}.crt", root / f"{name}.key"
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-keyout", str(key), "-out", str(cert), "-subj", f"/CN={hostname}",
                    "-addext", f"subjectAltName=DNS:{hostname}"], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return cert, key


def run(server):
    with tempfile.TemporaryDirectory(prefix="otterstax-s3-tls-") as directory:
        root = Path(directory)
        cert, key = certificate(root, "storage", "localhost")
        wrong_ca, _ = certificate(root, "untrusted", "untrusted.example")
        objects = {"/bucket/people.csv": b"id,name\n1,Alice\n2,Bob\n"}
        requests = []

        class Storage(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *_):
                pass

            def reply(self, send_body):
                requests.append((self.command, self.path))
                data = objects.get(self.path)
                self.send_response(200 if data is not None else 404)
                self.send_header("Content-Length", str(len(data or b"")))
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("ETag", '"fixture-etag"')
                self.send_header("Last-Modified", "Mon, 12 Oct 2026 00:00:00 GMT")
                self.send_header("x-amz-bucket-region", "us-east-1")
                self.end_headers()
                if send_body and data:
                    self.wfile.write(data)

            def do_HEAD(self):
                self.reply(False)

            def do_GET(self):
                self.reply(True)

        storage = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Storage)
        storage.daemon_threads = True
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.minimum_version = ssl.TLSVersion.TLSv1_2
        tls.load_cert_chain(cert, key)
        storage.socket = tls.wrap_socket(storage.socket, server_side=True)
        thread = threading.Thread(target=storage.serve_forever, daemon=True)
        thread.start()
        ports = {name: free_port() for name in ("mysql", "postgres", "flight_sql", "spark_connect")}
        assert len(set(ports.values())) == 4
        aliases = []
        for name, host, ca in (("valid", "localhost", cert), ("untrusted", "localhost", wrong_ca),
                               ("wronghost", "127.0.0.1", cert)):
            aliases.append({"alias": name, "access_key_env": "OTX_TEST_S3_ACCESS_KEY",
                            "secret_key_env": "OTX_TEST_S3_SECRET_KEY", "region": "us-east-1",
                            "endpoint": f"{host}:{storage.server_port}", "ca_file": str(ca)})
        # No scheme override: custom endpoints must retain the HTTPS default.
        config = {"service": {name: {"host": "127.0.0.1", "port": port} for name, port in ports.items()},
                  "connections": {"s3": aliases}}
        config["service"]["security"] = {"mode": "trusted_private"}
        config_file = root / "config.json"
        config_file.write_text(json.dumps(config))
        environment = dict(os.environ, OTX_TEST_S3_ACCESS_KEY=secrets.token_hex(10),
                           OTX_TEST_S3_SECRET_KEY=secrets.token_urlsafe(32))
        log_file = root / "server.log"
        process = None
        try:
            with log_file.open("w") as log:
                process = subprocess.Popen([str(server), "--config", str(config_file), "--data-dir", str(root / "data")],
                                           env=environment, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 60
            while True:
                if process.poll() is not None:
                    raise AssertionError("Server exited: " + log_file.read_text()[-2000:])
                try:
                    connection = psycopg.connect(host="127.0.0.1", port=ports["postgres"], user="test", dbname="test",
                                                 autocommit=True, connect_timeout=2, sslmode="disable")
                    break
                except psycopg.OperationalError:
                    if time.monotonic() > deadline:
                        raise
                    time.sleep(0.1)
            with connection:
                connection.execute("CREATE EXTERNAL TABLE tls_test.people WITH (s3_alias = 'valid', location = 's3://bucket/people.csv', format = 'csv');")
                assert connection.execute("SELECT * FROM tls_test.people ORDER BY id;").fetchall() == [(1, "Alice"), (2, "Bob")]
                assert any(method == "GET" for method, _ in requests)
                print("PASS custom S3 endpoint defaults to HTTPS and accepts its configured CA", flush=True)
                for alias in ("untrusted", "wronghost"):
                    before = len(requests)
                    try:
                        connection.execute(f"CREATE EXTERNAL TABLE tls_test.{alias} WITH (s3_alias = '{alias}', location = 's3://bucket/people.csv', format = 'csv');")
                    except psycopg.Error:
                        pass
                    else:
                        raise AssertionError(f"S3 TLS accepted {alias}")
                    assert len(requests) == before, "A request reached the fixture despite failed TLS validation"
                    print(f"PASS S3 TLS rejects {alias} without an HTTP request", flush=True)
                assert not list((root / "data" / "s3-staging").iterdir()), "S3 staging leaked files"
                print("PASS S3 private staging is cleaned after ingestion and failures", flush=True)
        finally:
            if process is not None:
                process.terminate()
                try:
                    process.wait(timeout=20)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=10)
            storage.shutdown()
            storage.server_close()
            thread.join(timeout=5)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", type=Path, required=True)
    run(parser.parse_args().server.resolve())
