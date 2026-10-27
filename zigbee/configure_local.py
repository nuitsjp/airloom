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
update_password = getpass("Firmware update password (12 to 64 printable ASCII characters): ")
if not ssid or len(ssid.encode()) > 32 or len(password.encode()) > 63:
    raise SystemExit("Invalid SSID or password length.")
if not 12 <= len(update_password) <= 64 or any(not 33 <= ord(c) <= 126 for c in update_password):
    raise SystemExit("Update password must be 12 to 64 non-space printable ASCII characters.")
values = {
    "AIRLOOM_WIFI_SSID": json.dumps(ssid, ensure_ascii=False),
    "AIRLOOM_WIFI_PASSWORD": json.dumps(password, ensure_ascii=False),
    "AIRLOOM_UPDATE_PASSWORD": json.dumps(update_password),
}
text = config_path.read_text(encoding="utf-8")
for key, value in values.items():
    text = re.sub(rf"(?m)^(?:CONFIG_{key}=.*|# CONFIG_{key} is not set)\n?", "", text)
    text += f"CONFIG_{key}={value}\n"
config_path.write_text(text, encoding="utf-8")
(config_path.parent / "ota-password.txt").write_text(update_password + "\n", encoding="utf-8")
print("Local sdkconfig and Git-ignored ota-password.txt updated. Rebuild the gateway.")
