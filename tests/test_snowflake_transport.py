#!/usr/bin/env python3
"""Exercise the pinned ODBC driver and C++ connector against an owned TLS fixture.

This checks client behavior against a simulated Snowflake HTTP endpoint. It does
not validate a real account, warehouse, permissions or backend transaction engine.
"""
import argparse
import gzip
import http.server
import json
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import threading
import time


class SnowflakeTransportTest:
    def __init__(self, probe, driver):
        self.probe = str(Path(probe).resolve(strict=True))
        self.driver = str(Path(driver).resolve(strict=True))
        self.temporary = tempfile.TemporaryDirectory(prefix="otterstax-snowflake-transport-")
        self.root = Path(self.temporary.name)
        self.requests = []
        self.errors = []
        self.scenario = None
        self.server = None

    def setup(self):
        """Create a CA and a separate localhost server certificate."""
        self.ca = self.root / "ca.pem"
        key, cert = self.root / "server.key", self.root / "server.pem"
        ca_key, csr, extensions = [self.root / name for name in ("ca.key", "server.csr", "extensions")]
        extensions.write_text("subjectAltName=DNS:localhost\nbasicConstraints=CA:FALSE\n"
                              "keyUsage=digitalSignature,keyEncipherment\nextendedKeyUsage=serverAuth\n")
        commands = [
            ["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1", "-keyout", str(ca_key), "-out", str(self.ca), "-subj", "/CN=Fixture CA"],
            ["openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-keyout", str(key), "-out", str(csr), "-subj", "/CN=localhost"],
            ["openssl", "x509", "-req", "-in", str(csr), "-CA", str(self.ca), "-CAkey", str(ca_key), "-CAcreateserial", "-out", str(cert), "-days", "1", "-extfile", str(extensions)],
        ]
        for command in commands:
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                try:
                    owner.respond(self)
                except (BrokenPipeError, ConnectionResetError, ssl.SSLError):
                    if owner.scenario != "timeout":
                        owner.errors.append("unexpected client disconnect")
                except Exception as error:
                    owner.errors.append(str(error))
                    self.close_connection = True

        self.server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        tls = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        tls.load_cert_chain(cert, key)
        self.server.socket = tls.wrap_socket(self.server.socket, server_side=True)
        self.thread = threading.Thread(target=self.server.serve_forever)
        self.thread.start()

    def respond(self, handler):
        raw = handler.rfile.read(int(handler.headers.get("Content-Length", 0)))
        if handler.headers.get("Content-Encoding") == "gzip":
            raw = gzip.decompress(raw)
        data = json.loads(raw) if raw else {}
        path = handler.path.split("?")[0]
        sql = data.get("sqlText", "")
        self.requests.append((path, sql))
        if path == "/session/v1/login-request":
            assert data["data"]["AUTHENTICATOR"] == "PROGRAMMATIC_ACCESS_TOKEN"
            reply = {"success": True, "data": {"token": "fixture-session", "masterToken": "fixture-master", "sessionId": 12345,
                     "validityInSeconds": 3600, "masterValidityInSeconds": 3600, "parameters": []}}
        elif path == "/queries/v1/query-request":
            assert "fixture-session" in handler.headers.get("Authorization", "")
            statement = sql.upper().strip()
            if not statement.startswith("ALTER SESSION"):
                assert data.get("parameters", {}).get("MULTI_STATEMENT_COUNT") == 1, "single-statement limit missing"
            if self.scenario == "lost_commit" and statement == "COMMIT":
                handler.connection.shutdown(socket.SHUT_RDWR)
                handler.connection.close()
                return
            if self.scenario == "timeout" and statement.startswith("SELECT"):
                time.sleep(4)
            result = {"queryId": "00000000-0000-0000-0000-000000000001", "queryResultFormat": "json",
                      "rowtype": [{"name": "status", "type": "text", "length": 100, "nullable": False}],
                      "rowset": [["Statement executed successfully."]], "total": 1, "statementTypeId": 0x5000}
            if statement.startswith("SELECT"):
                result.update({"statementTypeId": 0x1000, "total": 2, "rowtype": [
                    {"name": "NUMBER", "type": "fixed", "precision": 38, "scale": 8, "nullable": True},
                    {"name": "TEXT", "type": "text", "length": 50000, "nullable": True},
                    {"name": "BYTES", "type": "binary", "length": 16, "byteLength": 16, "nullable": True}],
                    "rowset": [["123456789012345678901234567890.12345678", "x" * 20000, "0000ff41"], [None, "", None]]})
            elif statement.startswith("INSERT"):
                result.update({"statementTypeId": 0x3100, "total": 1, "rowtype": [
                    {"name": "number of rows inserted", "type": "fixed", "precision": 38, "scale": 0, "nullable": False}], "rowset": [["7"]]})
            elif statement.startswith("ALTER SESSION"):
                result["statementTypeId"] = 0x4100
            reply = {"success": True, "data": result}
        elif path == "/session" or path == "/telemetry/send" or path.endswith("/abort-request"):
            reply = {"success": True, "data": {}}
        else:
            raise AssertionError(f"unexpected driver request {path}")
        body = json.dumps(reply).encode()
        handler.send_response(200)
        handler.send_header("Content-Type", "application/json")
        handler.send_header("Content-Length", str(len(body)))
        handler.end_headers()
        handler.wfile.write(body)

    def run_probe(self, scenario):
        self.scenario = scenario
        self.requests.clear()
        self.errors.clear()
        connection = {"alias": "warehouse", "driver": self.driver, "host": "127.0.0.1" if scenario == "wrong_hostname" else "localhost",
                      "port": self.server.server_port, "account": "fixture", "username": "fixture", "database": "DB", "schema": "PUBLIC",
                      "warehouse": "WH", "role": "ROLE", "token_env": "OTTERSTAX_SNOWFLAKE_FIXTURE_TOKEN",
                      "connect_timeout_seconds": 2, "query_timeout_seconds": 1 if scenario == "timeout" else 3,
                      "max_rows": 1 if scenario == "row_limit" else 10}
        if scenario != "untrusted_ca":
            connection["ca_file"] = str(self.ca)
        config = self.root / "config.json"
        config.write_text(json.dumps({"connections": {"snowflake": [connection]}}))
        environment = os.environ.copy()
        environment["OTTERSTAX_SNOWFLAKE_FIXTURE_TOKEN"] = "fixture-token"
        start = time.monotonic()
        completed = subprocess.run([self.probe, str(config), scenario], env=environment, text=True,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=12)
        assert completed.returncode == 0, (completed.stdout + completed.stderr, self.errors, self.requests)
        assert not self.errors, self.errors
        print(completed.stdout.strip(), flush=True)
        return time.monotonic() - start

    def test_rollback_and_lossless_fetch(self):
        """The real driver preserves values and sends rollback on the same session."""
        self.run_probe("rollback")
        assert sum(path.endswith("login-request") for path, _ in self.requests) == 1
        assert [sql for _, sql in self.requests if sql in ("BEGIN TRANSACTION", "ROLLBACK")] == ["BEGIN TRANSACTION", "ROLLBACK"]

    def test_commit(self):
        """A successful commit closes the dedicated connection."""
        self.run_probe("commit")
        assert sum(sql == "COMMIT" for _, sql in self.requests) == 1
        assert any(path == "/session" for path, _ in self.requests)

    def test_lost_commit_is_not_replayed(self):
        """A closed HTTP response produces one commit attempt and no new login."""
        self.run_probe("lost_commit")
        assert sum(sql == "COMMIT" for _, sql in self.requests) == 1
        assert sum(path.endswith("login-request") for path, _ in self.requests) == 1

    def test_untrusted_certificate(self):
        """No credentials reach a server with an untrusted certificate."""
        self.run_probe("untrusted_ca")
        assert self.requests == []

    def test_hostname_mismatch(self):
        """Trusting the CA does not permit a mismatched server hostname."""
        self.run_probe("wrong_hostname")
        assert self.requests == []

    def test_row_limit(self):
        """The real driver result cannot bypass the connector's row bound."""
        self.run_probe("row_limit")

    def test_timeout(self):
        """A query that stalls returns a failure within the configured deadline."""
        elapsed = self.run_probe("timeout")
        assert elapsed < 8, elapsed
        assert sum(path == "/queries/v1/query-request" and sql.startswith("SELECT")
                   for path, sql in self.requests) == 1, self.requests

    def cleanup(self):
        if self.server:
            self.server.shutdown()
            self.server.server_close()
            self.thread.join()
        self.temporary.cleanup()

    def run_all_tests(self):
        try:
            self.setup()
            self.test_rollback_and_lossless_fetch()
            self.test_commit()
            self.test_lost_commit_is_not_replayed()
            self.test_untrusted_certificate()
            self.test_hostname_mismatch()
            self.test_row_limit()
            self.test_timeout()
        finally:
            self.cleanup()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--probe", required=True)
    parser.add_argument("--driver", required=True)
    args = parser.parse_args()
    SnowflakeTransportTest(args.probe, args.driver).run_all_tests()
