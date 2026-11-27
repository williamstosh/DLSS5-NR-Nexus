"""Synthetic update API and ZIP for the Windows WinHTTP tests."""

import argparse
import http.server
import io
import json
import pathlib
import socket
import threading
import time
import zipfile


VERSION = "65535.0.0"
NAME = f"NR-Nexus-v{VERSION}-abcdef123456-windows-x64.zip"
archive = io.BytesIO()
with zipfile.ZipFile(archive, "w") as zip_file:
    zip_file.writestr(zipfile.ZipInfo("synthetic.txt"), b"x" * (192 * 1024))
ZIP = archive.getvalue()
RACE = threading.Barrier(2, timeout=10)
LOCK = threading.Lock()


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *_args):
        pass

    def respond(self, status, body=b"", headers=()):
        self.send_response(status)
        for name, value in headers:
            self.send_header(name, value)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        if body:
            self.wfile.write(body)

    def do_GET(self):
        etag = self.headers.get("If-None-Match")
        with LOCK, self.server.log.open("a", encoding="utf-8") as log:
            log.write(json.dumps({"path": self.path, "etag": etag, "agent": self.headers.get("User-Agent"),
                                  "accept": self.headers.get("Accept"),
                                  "api_version": self.headers.get("X-GitHub-Api-Version"),
                                  "cookie": self.headers.get("Cookie")}) + "\n")
        try:
            self.route(etag)
        except (ConnectionResetError, BrokenPipeError, ConnectionAbortedError):
            # The cancelled worker deliberately closes the request mid-stream.
            pass

    def route(self, etag):
        if self.path.startswith("/check/"):
            case = self.path.removeprefix("/check/")
            if case == "stale" and etag:
                return self.respond(503)
            if case == "missing":
                return self.respond(404)
            if case == "invalid":
                return self.respond(200, b"{")
            if case == "huge":
                return self.respond(200, b" " * (1024 * 1024 + 1))
            if case == "unpaired":
                return self.respond(304)
            if case == "unrelated403":
                return self.respond(403)
            if case == "reset403":
                return self.respond(403, headers=(("X-RateLimit-Remaining", "0"),
                                                  ("X-RateLimit-Reset", str(int(time.time()) + 360))))
            if case == "retry429":
                return self.respond(429, headers=(("Retry-After", "90"),))
            if case == "api-redirect":
                return self.respond(302, headers=(("Location", "/should-not-reach"),))
            if case == "pinned":
                time.sleep(0.8)
            validators = {
                "etag-weak": ('W/"weak"',),
                "etag-wildcard": ("*",),
                "etag-list": ('"first", "second"',),
                "etag-duplicate": ('"first"', '"second"'),
            }
            revision = 0
            if case in validators:
                with LOCK:
                    revision = self.server.revisions.get(case, 0) + 1
                    self.server.revisions[case] = revision
                if case == "etag-wildcard" and revision == 1:
                    validators[case] = ('"before-wildcard"',)
                if etag and not (case == "etag-wildcard" and revision == 2):
                    return self.respond(304)
            if etag == '"release"':
                return self.respond(304)
            origin = f"http://127.0.0.1:{self.server.server_port}"
            release = {
                "tag_name": "v" + VERSION, "published_at": "2026-11-26T10:42:50Z",
                "body": f"## 标题\n- Bilingual notes ({revision})", "draft": False, "prerelease": False,
                "html_url": origin + "/release", "zipball_url": origin + "/source.zip",
                "assets": [{
                    "id": 42, "name": NAME, "state": "uploaded", "size": len(ZIP),
                    "browser_download_url": origin + "/download/" + case,
                }],
            }
            headers = tuple(("ETag", tag) for tag in validators.get(case, ('"release"',)))
            return self.respond(200, json.dumps(release, ensure_ascii=False).encode("utf-8"), headers)
        if self.path == "/download/reuse":
            return self.respond(302, headers=(("Location", "/download/redirected"),
                                              ("Set-Cookie", "fixture=secret")))
        if self.path == "/download/redirected":
            return self.respond(200, ZIP)
        if self.path == "/download/outside-host":
            return self.respond(302, headers=(("Location", f"http://localhost:{self.server.server_port}/should-not-reach"),))
        if self.path == "/download/outside-port":
            different_port = (1 if self.server.server_port != 1 else 2)
            return self.respond(307, headers=(("Location", f"http://127.0.0.1:{different_port}/should-not-reach"),))
        if self.path == "/download/redirect-loop":
            return self.respond(302, headers=(("Location", "/download/redirect-loop"),))
        if self.path in ("/download/short", "/download/denied-delete"):
            return self.respond(200, ZIP[:-1])
        if self.path == "/download/long":
            return self.respond(200, ZIP + b"x")
        if self.path == "/download/recover" and not self.server.recovery:
            self.server.recovery = True
            self.send_response(200)
            self.send_header("Content-Length", str(len(ZIP)))
            self.end_headers()
            self.wfile.write(ZIP[: 80 * 1024])
            self.connection.shutdown(socket.SHUT_WR)
            self.close_connection = True
            return None
        if self.path == "/download/race":
            RACE.wait()
        if self.path in ("/download/slow", "/download/race", "/download/conflict-race"):
            self.send_response(200)
            self.send_header("Content-Length", str(len(ZIP)))
            self.end_headers()
            self.wfile.write(ZIP[: 96 * 1024])
            self.wfile.flush()
            time.sleep(0.8)
            self.wfile.write(ZIP[96 * 1024 :])
            return None
        if self.path.startswith("/download/"):
            return self.respond(200, ZIP)
        return self.respond(500)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--ready", type=pathlib.Path, required=True)
    parser.add_argument("--log", type=pathlib.Path, required=True)
    args = parser.parse_args()
    with http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler) as server:
        server.log = args.log
        server.recovery = False
        server.revisions = {}
        ready = args.ready.with_suffix(".part")
        ready.write_text(str(server.server_port), encoding="ascii")
        ready.replace(args.ready)
        server.serve_forever()


if __name__ == "__main__":
    main()
