# ローカル HTTP 受信サーバー

Python 3 の標準ライブラリだけで動きます。作業ディレクトリに `measurements.jsonl` を追記します。

```powershell
python receiver/server.py --host 0.0.0.0 --port 8080 --file measurements.jsonl --stale-after 172800
```

既定では `127.0.0.1:8080` で待ち受けます。NanoC6 から利用するときは、NanoC6 から到達可能な PC の LAN アドレスに指定するか、上記のように `0.0.0.0` を指定します。OS のファイアウォールが接続を遮る場合は、その設定の確認が必要です。

`POST /` に JSON オブジェクトを送り、受信した UTC 時刻と一緒に JSON Lines 形式で保存します。`GET /` は最後の受信データと、温度・湿度それぞれの鮮度を JSON で返します。データには `sensor_id`, `temperature_c`, `humidity_percent`, `temperature_age_seconds`, `humidity_age_seconds` を指定します。age はゲートウェイが各値を受信してから HTTP 送信するまでの経過秒で、測定時刻ではありません。受信先は受信後の経過秒を加算して表示します。age がない値、null の値、受信後にPC時計が戻った場合は鮮度を `unknown` とし、未取得を 0 として扱いません。鮮度の閾値は `--stale-after` 秒、既定 2 日です。受信データ原本も `data` に含みます。

```powershell
python -m unittest discover -s receiver -p "test_*.py"
```

このサーバーには認証や暗号化はありません。信頼できるローカルネットワークでのみ使用してください。
