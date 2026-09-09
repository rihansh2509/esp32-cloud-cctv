# ESP32-S3 Cloud CCTV — two cameras, one permanent URL

```
 ESP32 cam1 ──┐  home Wi-Fi   ┌──────────────────────┐   WebSocket   ┌───────────┐
              ├──── HTTPS ───►│  Render (free tier)  │──────────────►│  any      │
 ESP32 cam2 ──┘  POST frames  │ esp32-cloud-cctv     │               │  browser  │
                              └──────────────────────┘               └───────────┘
                          https://esp32-cloud-cctv.onrender.com
```

- `server/` — Node relay. Cameras POST JPEG frames, browsers watch over WebSocket.
- `firmware/CloudCam_1`, `firmware/CloudCam_2` — identical sketches, only the default camera id differs.
- `render.yaml` — one-click Render Blueprint.

Nothing is hard-coded in the firmware. Wi-Fi, server URL and key are typed into the Serial Monitor once and saved to flash.

## 1. Deploy the server on Render (about 5 minutes)

1. Push this folder to a GitHub repo (public or private).
2. On https://dashboard.render.com click **New + → Blueprint**, pick the repo, click **Apply**.
3. Render asks for two values:
   - `CAM_KEY` — any long random string. The cameras must send it. Example: `k9F2-super-secret-7Qx`
   - `DASH_PASS` — the password the web page will ask you for.
4. Wait for the deploy to finish. Your permanent URL is shown at the top, like
   `https://esp32-cloud-cctv.onrender.com` (Render adds a random suffix if the name is taken).

Free tier notes: the service sleeps after 15 min without traffic, but the cameras ping it every 5 s so it stays awake. First load after a cold start can take ~30 s. Free outbound bandwidth is 100 GB/month, which is why cameras only stream at full speed while someone is watching.

## 2. Configure each camera (Serial Monitor, 115200, line ending = New Line)

Type these lines, one at a time, into the Serial Monitor of each board:

```
ssid=YourHomeWiFi
pass=YourWifiPassword
host=https://esp32-cloud-cctv.onrender.com
key=k9F2-super-secret-7Qx
show
```

Board 1 is `cam1`, board 2 is `cam2` by default. Change with `id=frontdoor` if you like.
Within a few seconds you should see:

```
[WIFI] connected, IP 192.168.1.42
[HTTP] connecting to https://esp32-cloud-cctv.onrender.com/api/frame/cam1
[STAT] idle | viewers 0 | 0.2 fps | 5 KB/s | total 2 frames
```

Open the URL in any browser, enter `DASH_PASS`, and both cameras appear. The `[STAT]` line switches to `LIVE` with 5–10 fps while the page is open.

## 3. Flashing (already done for both boards, but for reference)

Arduino IDE → Tools: ESP32S3 Dev Module, USB CDC On Boot = Enabled, PSRAM = OPI PSRAM,
Flash Size = 16MB, Partition = 16M Flash (3MB APP/9.9MB FATFS). Board package esp32 3.x.

## Troubleshooting

| Symptom | Fix |
|---|---|
| `[HTTP] server said 401` | `key=` on the camera does not match `CAM_KEY` on Render |
| `[HTTP] failed: connection refused/lost` | Wrong `host=`, or Render is still deploying. Check the URL in a browser first |
| `[WIFI] connecting ...` forever | Wrong ssid/pass, or 5 GHz-only network (ESP32 needs 2.4 GHz) |
| `[CAM] init FAILED` | Wrong pin model — change the `#define CAMERA_MODEL_...` line |
| Page says OFFLINE | Camera stopped posting; look at its Serial Monitor |
| Picture upside down | `s->set_vflip(s, 1)` in `initCamera()` |
| Choppy | Slow upload. Use `FRAMESIZE_HVGA` or `jpeg_quality = 18` |

## Local test without Render

```bash
cd server && npm install && npm start
```
Then on the camera: `host=http://<your-pc-ip>:3000` (plain HTTP works too).
