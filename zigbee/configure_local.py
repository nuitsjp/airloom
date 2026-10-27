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
if not ssid or len(ssid.encode()) > 32 or len(password.encode()) > 63:
    raise SystemExit("Invalid SSID or password length.")
values = {
    "AIRLOOM_WIFI_SSID": json.dumps(ssid, ensure_ascii=False),
    "AIRLOOM_WIFI_PASSWORD": json.dumps(password, ensure_ascii=False),
}
text = config_path.read_text(encoding="utf-8")
for key, value in values.items():
    text = re.sub(rf"(?m)^(?:CONFIG_{key}=.*|# CONFIG_{key} is not set)\n?", "", text)
    text += f"CONFIG_{key}={value}\n"
config_path.write_text(text, encoding="utf-8")
print("Local sdkconfig updated. Rebuild and flash the gateway.")
