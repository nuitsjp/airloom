import json
from datetime import datetime, timedelta, timezone
from http.server import ThreadingHTTPServer
from pathlib import Path
from tempfile import TemporaryDirectory
from threading import Thread
from urllib.request import Request, urlopen
from urllib.error import HTTPError
import unittest

from server import handler_for, view


class ReceiverTest(unittest.TestCase):
    def test_age_fresh_stale_and_unknown(self):
        t = datetime(2026, 1, 1, tzinfo=timezone.utc)
        record = {"received_at": t.isoformat(), "data": {"sensor_id": "one", "temperature_c": 22.5, "temperature_age_seconds": 5, "humidity_percent": 60, "humidity_age_seconds": 20}}
        result = view(record, 30, t + timedelta(seconds=15))
        self.assertEqual(result["temperature_c"], {"value": 22.5, "age_seconds": 20, "freshness": "fresh"})
        self.assertEqual(result["humidity_percent"]["freshness"], "stale")
        record["data"]["temperature_c"] = None
        self.assertEqual(view(record, 30, t)["temperature_c"]["freshness"], "unknown")
        self.assertEqual(view(record, 30, t - timedelta(seconds=1))["humidity_percent"]["freshness"], "unknown")

    def test_http_and_jsonl(self):
        with TemporaryDirectory() as tmp:
            path = Path(tmp) / "measurements.jsonl"
            server = ThreadingHTTPServer(("127.0.0.1", 0), handler_for(path, 10))
            thread = Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                url = f"http://127.0.0.1:{server.server_port}/"
                self.assertIsNone(json.load(urlopen(url))["data"])
                payload = {"sensor_id": "test", "temperature_c": 21.5, "temperature_age_seconds": 20, "humidity_percent": None}
                request = Request(url, data=json.dumps(payload).encode(), headers={"Content-Type": "application/json"})
                self.assertTrue(json.load(urlopen(request))["ok"])
                result = json.load(urlopen(url))
                self.assertEqual(result["temperature_c"]["freshness"], "stale")
                self.assertEqual(result["humidity_percent"]["freshness"], "unknown")
                stored = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(stored["data"], payload)
                self.assertTrue(stored["received_at"].endswith("+00:00"))
                invalid = Request(url, data=b'{"temperature_c":1e999}')
                with self.assertRaises(HTTPError) as caught:
                    urlopen(invalid)
                self.assertEqual(caught.exception.code, 400)
            finally:
                server.shutdown()
                thread.join()
                server.server_close()


if __name__ == "__main__":
    unittest.main()
