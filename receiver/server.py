"""Small local HTTP receiver; stores request bodies alongside UTC receive times."""
import argparse
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
from pathlib import Path
from threading import Lock


def finite_float(text):
    value = float(text)
    if not math.isfinite(value):
        raise ValueError("invalid number")
    return value


def now_utc():
    return datetime.now(timezone.utc)


def age_at_display(value, age, elapsed, threshold):
    if value is None or not isinstance(age, (int, float)) or isinstance(age, bool) or age < 0 or elapsed is None:
        return {"value": value, "age_seconds": None, "freshness": "unknown"}
    total = age + elapsed
    return {"value": value, "age_seconds": round(total, 3), "freshness": "fresh" if total <= threshold else "stale"}


def view(record, threshold, now=None):
    received_at = datetime.fromisoformat(record["received_at"])
    elapsed = ((now or now_utc()) - received_at).total_seconds()
    # A clock moved backwards cannot be used to prove freshness.
    elapsed = elapsed if elapsed >= 0 else None
    body = record["data"]
    return {
        "received_at": record["received_at"],
        "seconds_since_received": round(elapsed, 3) if elapsed is not None else None,
        "sensor_id": body.get("sensor_id"),
        "temperature_c": age_at_display(body.get("temperature_c"), body.get("temperature_age_seconds"), elapsed, threshold),
        "humidity_percent": age_at_display(body.get("humidity_percent"), body.get("humidity_age_seconds"), elapsed, threshold),
        "data": body,
    }


def handler_for(path, stale_after):
    path = Path(path)
    lock = Lock()

    class Handler(BaseHTTPRequestHandler):
        def respond(self, status, data):
            payload = json.dumps(data, ensure_ascii=False, allow_nan=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)

        def do_POST(self):
            if self.path != "/":
                self.respond(404, {"error": "not found"})
                return
            try:
                length = int(self.headers.get("Content-Length", "0"))
                if not 0 < length <= 65536:
                    raise ValueError("invalid content length")
                body = json.loads(self.rfile.read(length), parse_float=finite_float, parse_constant=lambda _: (_ for _ in ()).throw(ValueError("invalid number")))
                if not isinstance(body, dict):
                    raise ValueError("JSON object required")
            except (ValueError, UnicodeDecodeError, json.JSONDecodeError):
                self.respond(400, {"error": "invalid JSON object or content length"})
                return
            record = {"received_at": now_utc().isoformat(), "data": body}
            with lock:
                with path.open("a", encoding="utf-8") as f:
                    f.write(json.dumps(record, ensure_ascii=False, allow_nan=False) + "\n")
            self.respond(200, {"ok": True})

        def do_GET(self):
            if self.path != "/":
                self.respond(404, {"error": "not found"})
                return
            with lock:
                try:
                    with path.open(encoding="utf-8") as f:
                        last = None
                        for line in f:
                            if line.strip():
                                last = json.loads(line)
                except FileNotFoundError:
                    last = None
            self.respond(200, view(last, stale_after) if last else {"data": None})

    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1", help="Use a LAN address or 0.0.0.0 to receive from the gateway")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--file", default="measurements.jsonl")
    parser.add_argument("--stale-after", type=float, default=172800, help="seconds; default 2 days")
    args = parser.parse_args()
    if args.stale_after < 0:
        parser.error("--stale-after must be non-negative")
    server = ThreadingHTTPServer((args.host, args.port), handler_for(args.file, args.stale_after))
    print(f"Listening on {args.host}:{args.port}, saving to {args.file}", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
