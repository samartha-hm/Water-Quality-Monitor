# 💻 Local Setup & Testing Guide (Before AWS)

Follow these steps to set up the **Mosquitto MQTT Broker** and **Web Dashboard** directly on your Windows PC, so you can test data flow before deploying to AWS.

---

## Architecture for Local Testing

```
  [ ESP32 Device ]
  (WiFi or Cellular*)
         │
         ▼  MQTT (port 1883)
  ┌──────────────────────────────────────────────┐
  │              YOUR WINDOWS PC                 │
  │                                              │
  │  Mosquitto Broker (localhost:1883)          │
  │         ▲                                    │
  │         │ MQTT Subscribe                     │
  │  FastAPI Server (server.py)                  │
  │         │ Serves                             │
  │         ▼                                    │
  │  Dashboard (http://localhost:8000)           │
  └──────────────────────────────────────────────┘
```

---

## Step 1: Install Mosquitto MQTT Broker on Windows

### Method A: Using Command Prompt / PowerShell (Recommended)
Open Command Prompt or PowerShell as Administrator and run:

```cmd
winget install Eclipse.Mosquitto
```

### Method B: Manual Download
1. Download installer from: [https://mosquitto.org/download/](https://mosquitto.org/download/)
2. Run `mosquitto-2.x.x-install-x64.exe` with default options.

---

## Step 2: Configure & Start Mosquitto on Windows

1. Create or edit `C:\Program Files\mosquitto\mosquitto.conf` (or create a local file `local_mosquitto.conf` in your project folder).
2. Add these two lines to allow incoming connections:
   ```ini
   listener 1883 0.0.0.0
   allow_anonymous true
   ```
3. Start Mosquitto from Command Prompt:
   ```cmd
   "C:\Program Files\mosquitto\mosquitto.exe" -c "C:\Program Files\mosquitto\mosquitto.conf" -v
   ```

---

## Step 3: Run the Local Python Server & Dashboard

1. Open Command Prompt / Terminal in your project directory `E:\experimindlabs\WaterQualityMonitor`.
2. Install Python dependencies:
   ```cmd
   cd server
   pip install -r requirements.txt
   ```
3. Run the server:
   ```cmd
   python server.py
   ```
4. Open your web browser and go to:
   - **URL**: `http://localhost:8000`
   - **Username**: `admin`
   - **Password**: `waterquality`

---

## Step 4: Test MQTT Data Flow Locally

### Test 1: Send Fake Sensor Data (Simulate ESP32)
Open a new terminal window and run:
```cmd
"C:\Program Files\mosquitto\mosquitto_pub.exe" -h localhost -t "waterquality/WQM-001/sensors/live" -m "{\"device_id\":\"WQM-001\",\"sensors\":{\"ph\":{\"value\":7.4,\"unit\":\"pH\"},\"tds\":{\"value\":180,\"unit\":\"ppm\"},\"temperature\":{\"value\":27.5,\"unit\":\"°C\"},\"turbidity\":{\"value\":1200,\"unit\":\"NTU\"},\"dissolved_oxygen\":{\"value\":6.9,\"unit\":\"mg/L\"}},\"metadata\":{\"signal_quality\":24,\"firmware_version\":\"2.0.0\",\"uptime_seconds\":120}}"
```
👉 Refresh your browser at `http://localhost:8000` — you should immediately see the gauges update!

---

## Step 5: Connect ESP32 to Local Mosquitto

### Finding Your PC's Local IP
Open Command Prompt and run:
```cmd
ipconfig
```
Look for **IPv4 Address** (e.g., `192.168.1.15` or `10.0.0.5`).

### Update `firmware/config.h`
```cpp
// Change line 43 in config.h to your PC's IP address:
#define MQTT_BROKER         "192.168.x.x"   // ← Your PC's local IP
#define MQTT_PORT           1883
```

> [!NOTE]
> **Important Note about 4G Cellular vs Local PC:**
> - If ESP32 is on **WiFi**, it can connect directly to your PC's local IP (`192.168.x.x`).
> - If ESP32 uses the **SIMA7670C 4G SIM**, cellular networks cannot see private local IPs (`192.168.x.x`). To test over 4G locally on your PC before AWS, use **ngrok**:
>   ```cmd
>   ngrok tcp 1883
>   ```
>   `ngrok` provides a public host and port (e.g. `4.tcp.ngrok.io:12345`) which you put into `config.h`.

---

## Summary Checklist for Local Testing

- [ ] Install Mosquitto on Windows via `winget install Eclipse.Mosquitto`
- [ ] Start Mosquitto with `allow_anonymous true` and `listener 1883`
- [ ] Run `python server/server.py` and open `http://localhost:8000`
- [ ] Publish a test message using `mosquitto_pub` to verify the dashboard
- [ ] Update `config.h` with local IP or ngrok address and flash ESP32
