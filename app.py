#!/usr/bin/env python3
"""Phone-friendly web control for the Dyson purifier's ESP32 (Dyson_UART_parser firmware).

The ESP32 has no CORS headers, so the browser can't call it directly; this serves the page
and relays a fixed set of commands to it. Standard library only.

    FAN_URL=http://10.0.44.168 PORT=8090 python3 app.py
"""
import json
import os
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

FAN_URL = os.environ.get("FAN_URL", "http://10.0.44.168").rstrip("/")
PORT = int(os.environ.get("PORT", "8090"))
PAGE = (Path(__file__).parent / "index.html").read_bytes()

# IR commands the firmware's /api/controls understands as {"attributes": <name>}.
ATTRIBUTES = {"auto_mode_on", "auto_mode_off", "night_mode", "sw_rotation", "direction"}


def fan_request(path, payload=None):
    data = None if payload is None else json.dumps(payload).encode()
    req = urllib.request.Request(FAN_URL + path, data=data, method="POST" if data else "GET",
                                 headers={"Content-Type": "application/json"})
    with urllib.request.urlopen(req, timeout=5) as resp:
        return resp.read()


def fan_state():
    return json.loads(fan_request("/api/states/sensors"))


def run_command(cmd):
    """Translate a page command into the firmware's payload; returns an error string or None."""
    action = cmd.get("action")
    if action in ("on", "off"):
        # The firmware only has an on/off toggle, so check first and never flip the wrong way.
        if fan_state()["state"] != (action == "on"):
            fan_request("/api/controls", {"attributes": "on_off"})
    elif action == "speed":
        level = cmd.get("level")
        if not isinstance(level, int) or not 1 <= level <= 10:
            return "speed must be 1-10"
        fan_request("/api/controls", {"set_fan": level})
    elif action in ATTRIBUTES:
        fan_request("/api/controls", {"attributes": action})
    else:
        return "unknown action"
    return None


class Handler(BaseHTTPRequestHandler):
    def send(self, code, body, ctype="application/json"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path == "/":
            self.send(200, PAGE, "text/html; charset=utf-8")
        elif self.path == "/api/state":
            try:
                self.send(200, json.dumps(fan_state()).encode())
            except Exception as e:
                self.send(502, json.dumps({"error": f"fan unreachable: {e}"}).encode())
        else:
            self.send(404, b'{"error":"not found"}')

    def do_POST(self):
        if self.path != "/api/cmd":
            return self.send(404, b'{"error":"not found"}')
        try:
            length = int(self.headers.get("Content-Length", 0))
            cmd = json.loads(self.rfile.read(min(length, 1024)) or b"{}")
        except ValueError:
            return self.send(400, b'{"error":"bad json"}')
        try:
            err = run_command(cmd)
        except Exception as e:
            return self.send(502, json.dumps({"error": f"fan unreachable: {e}"}).encode())
        if err:
            return self.send(400, json.dumps({"error": err}).encode())
        self.send(200, b'{"ok":true}')

    def log_message(self, fmt, *args):
        if self.command == "POST":
            super().log_message(fmt, *args)


if __name__ == "__main__":
    print(f"Dyson control on http://0.0.0.0:{PORT}/ -> {FAN_URL}")
    ThreadingHTTPServer(("0.0.0.0", PORT), Handler).serve_forever()
