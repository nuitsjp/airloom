"""Configure the gateway locally without printing Wi-Fi credentials."""
from getpass import getpass
import json
from pathlib import Path
import re

config_path = Path(__file__).resolve().parent / "sdkconfig"
if not config_path.exists():
    raise SystemExit("Run idf.py build in zigbee/ first to create sdkconfig.")
ssid = input("Wi-Fi SSID: ")
password = getpass("Wi-Fi password (input hidden): ")
url = input("HTTP receiver URL (for example http://192.168.0.20:8080/): ").strip()
interval = input("Upload interval in seconds [30 for testing]: ").strip() or "30"
if not ssid or len(ssid.encode()) > 32 or len(password.encode()) > 63 or not url.startswith("http://"):
    raise SystemExit("Invalid SSID, password length, or HTTP URL.")
if not interval.isdecimal() or not 10 <= int(interval) <= 604800:
    raise SystemExit("Interval must be between 10 and 604800 seconds.")
values = {
    "AIRLOOM_WIFI_SSID": json.dumps(ssid, ensure_ascii=False),
    "AIRLOOM_WIFI_PASSWORD": json.dumps(password, ensure_ascii=False),
    "AIRLOOM_HTTP_URL": json.dumps(url, ensure_ascii=False),
    "AIRLOOM_UPLOAD_INTERVAL_SECONDS": str(int(interval)),
}
text = config_path.read_text(encoding="utf-8")
for key, value in values.items():
    text = re.sub(rf"(?m)^(?:CONFIG_{key}=.*|# CONFIG_{key} is not set)\n?", "", text)
    text += f"CONFIG_{key}={value}\n"
config_path.write_text(text, encoding="utf-8")
print("Local sdkconfig updated. Rebuild and flash the gateway.")
