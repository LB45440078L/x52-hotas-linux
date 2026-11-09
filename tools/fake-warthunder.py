#!/usr/bin/env python3
"""Fake War Thunder local API, for testing the telemetry pipeline."""
import json
import sys
from http.server import BaseHTTPRequestHandler, HTTPServer

STATE = {
    "valid": True,
    "IAS, km/h": 642,
    "TAS, km/h": 1108,
    "H, m": 8240,
    "M": 1.16,
    "Ny": 7.4,
    "AoA, deg": 2.3,
    "Vy, m/s": -12.4,
    "Mfuel, kg": 3100,
    "Mfuel0, kg": 5000,
    "throttle 1, %": 100,
    "RPM 1": 9700,
    "flaps, %": 0,
}
INDICATORS = {
    "gears": 0.0,
    "compass": 271.4,
    "airbrake_indicator": 0.0,
    "vario": -12.4,
    "weapon2": 0.0,
    "weapon3": 0.0,
    "g_meter": 7.4,
}


class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        body = None
        if self.path.startswith("/state"):
            body = json.dumps(STATE)
        elif self.path.startswith("/indicators"):
            body = json.dumps(INDICATORS)
        if body is None:
            self.send_response(404)
            self.end_headers()
            return
        data = body.encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *args):
        pass


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8111
    HTTPServer(("127.0.0.1", port), Handler).serve_forever()
