#!/usr/bin/env python3
"""Serve the WebAssembly build and the game data over the LAN.

The browser build does not bundle the archives; it fetches byte ranges out of
them as the engine reads.  http.server ignores Range headers, so the game data
requests are answered here instead.

    tools/serve-web.py [--port 8080] [--directory build/web] \\
                       [--game-data game-data]
"""

import argparse
import http.server
import re
import socket
import socketserver
from pathlib import Path

GAME_DATA_PREFIX = "/game-data/"
RANGE_PATTERN = re.compile(r"^bytes=(\d*)-(\d*)$")


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".data": "application/octet-stream",
        ".js": "text/javascript",
    }
    game_data = None

    def end_headers(self):
        # Game data never changes, so let the browser keep it across reloads.
        # The engine files must not be cached, or rebuilds go unnoticed.
        if self.path.startswith(GAME_DATA_PREFIX):
            self.send_header("Cache-Control", "public, max-age=31536000, immutable")
        else:
            self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_HEAD(self):
        if not self.serve_game_data(body=False):
            super().do_HEAD()

    def do_GET(self):
        if not self.serve_game_data(body=True):
            super().do_GET()

    def resolve_game_data(self):
        """Map a /game-data/ request onto a file, or None if it is not one."""
        if self.game_data is None or not self.path.startswith(GAME_DATA_PREFIX):
            return None
        name = self.path[len(GAME_DATA_PREFIX):].split("?", 1)[0]
        if not name or "/" in name or name.startswith("."):
            return None
        return self.game_data / name

    def serve_game_data(self, body):
        """Answer a game data request; False means "not mine, fall through"."""
        path = self.resolve_game_data()
        if path is None:
            return False
        if not path.is_file():
            self.send_error(404, "No such game data file")
            return True

        size = path.stat().st_size
        start, end = self.parse_range(size)
        if start is None:
            self.send_error(416, "Requested range not satisfiable")
            self.send_header("Content-Range", f"bytes */{size}")
            return True

        partial = self.headers.get("Range") is not None
        self.send_response(206 if partial else 200)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        if not body:
            return True
        with path.open("rb") as source:
            source.seek(start)
            remaining = end - start + 1
            while remaining > 0:
                chunk = source.read(min(remaining, 1 << 20))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)
        return True

    def parse_range(self, size):
        """Return the inclusive byte range to send, or (None, None)."""
        header = self.headers.get("Range")
        if header is None:
            return 0, max(size - 1, 0)
        match = RANGE_PATTERN.match(header.strip())
        if not match:
            return None, None
        first, last = match.groups()
        if not first:                       # bytes=-N: the final N bytes
            if not last:
                return None, None
            start, end = max(size - int(last), 0), size - 1
        else:
            start = int(first)
            end = int(last) if last else size - 1
        end = min(end, size - 1)
        if start > end or start >= size:
            return None, None
        return start, end

    def log_message(self, format, *args):
        print(f"{self.address_string()} {format % args}")


class Server(socketserver.ThreadingTCPServer):
    allow_reuse_address = True
    daemon_threads = True


def local_address():
    try:
        probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        probe.connect(("10.255.255.255", 1))
        address = probe.getsockname()[0]
        probe.close()
        return address
    except OSError:
        return socket.gethostbyname(socket.gethostname())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--directory", default="build/web")
    parser.add_argument("--game-data", default="game-data")
    parser.add_argument("--bind", default="0.0.0.0")
    arguments = parser.parse_args()

    game_data = Path(arguments.game_data).resolve()
    if not game_data.is_dir():
        parser.error(f"game data directory not found: {game_data}")

    class BoundHandler(Handler):
        pass

    BoundHandler.game_data = game_data
    directory = arguments.directory

    def factory(*args, **kwargs):
        return BoundHandler(*args, directory=directory, **kwargs)

    with Server((arguments.bind, arguments.port), factory) as server:
        print(f"Serving {directory} with game data from {game_data}")
        print(f"  http://{local_address()}:{arguments.port}/toheart2.html")
        server.serve_forever()


if __name__ == "__main__":
    main()
