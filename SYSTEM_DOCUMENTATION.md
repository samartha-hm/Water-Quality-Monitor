# 🌊 Water Quality Monitor v2.0 — Complete System Documentation
**Industrial-Grade, 4G/LTE-Connected Multi-Parameter Pond Intelligence Platform**

---

## 📋 Executive Summary

The **Water Quality Monitor v2.0** is an industrial-grade, 4G/LTE-connected environmental monitoring system. It measures **5 core water quality parameters**:
1. **pH Level**
2. **Total Dissolved Solids (TDS)**
3. **Water Temperature**
4. **Turbidity (Clarity)**
5. **Dissolved Oxygen (DO)**

Data is gathered by an **ESP32 microcontroller** using **phase-multiplexing** to eliminate sensor interference, stored locally in **ESP32 NVS Flash memory**, transmitted via a **SIMA7670C 4G/LTE modem** using **MQTT protocol**, processed by a **FastAPI backend** running on **AWS EC2**, stored in a **SQLite database with WAL mode**, and streamed live to a **responsive glassmorphism web dashboard** via **WebSockets (`ws://`)**.

---

## 🔑 System Credentials & Network Parameters

| Parameter | Value / Setting | Description |
|-----------|-----------------|-------------|
| **AWS Server Public IP** | `<YOUR-EC2-PUBLIC-IP>` | EC2 Instance IP |
| **Dashboard URL** | `http://<YOUR-EC2-PUBLIC-IP>:8000` | Web Interface |
| **Dashboard Username** | `admin` | HTTP Basic Auth User |
| **Dashboard Password** | `waterquality` | HTTP Basic Auth Pass (configurable) |
| **MQTT Broker Host** | `<YOUR-EC2-PUBLIC-IP>` | Mosquitto Broker |
| **MQTT Broker Port** | `1883` | Unencrypted TCP |
| **Cellular APN** | `airtelgprs.com` | Airtel India 4G |
| **Device ID** | `WQM-001` | Unique Hardware ID |
| **Firmware Version** | `2.0.0` | Active Enterprise Release |
| **Baud Rate (Debug)** | `115200` | USB Serial Monitor |
| **Baud Rate (4G Modem)** | `115200` | Hardware Serial2 |

---

## ⚡ Enterprise Features & Architecture

### 1. ESP32 NVS (Non-Volatile Storage) Flash Persistence
All 9 runtime parameters (`sensor_read_interval`, `mqtt_publish_interval`, `ph_phase_duration`, `deep_sleep_enabled`, etc.) are written to internal ESP32 Flash memory (`Preferences.h`). Configuration survives power outages, manual resets, and Deep Sleep wake cycles.

### 2. RTC Boot-Loop Guard & Thermal Protection
Uses ESP32 RTC Fast Memory (`RTC_DATA_ATTR int consecutiveBootFailures`) to track failure cycles. If 4G modem setup fails 3 times in a row, the ESP32 enters a **5-minute Low-Power Deep Sleep** guard to prevent battery drain and hardware thermal stress.

### 3. FastAPI WebSockets Real-Time Stream (`ws://`)
Broadcasts live sensor readings and status updates directly to connected web clients as soon as MQTT messages arrive on AWS EC2, bypassing 3-second HTTP polling overhead.

### 4. Database Config Persistence
Stores device configurations in SQLite (`device_config` table). Settings remain saved on the server across FastAPI and AWS EC2 restarts.

### 5. Server-Side Certified Calibration Layer
Applies real-time mathematical transformation (`calibrate_sensor_payload()`) to incoming raw MQTT payloads on the server. Calibrates raw hardware readings to match certified laboratory reference test values ($pH\text{ 7.97}$, $TDS\text{ 77.0 mg/L}$, $DO\text{ 7.2 mg/L}$, $Turbidity\text{ 0.4 NTU}$, $Temp\text{ 27.4}^\circ\text{C}$) without requiring physical access to re-flash the ESP32 microcontroller.

### 6. Smart Server Downsampling & Live Point Streaming
The `/api/data/history` endpoint automatically downsamples large historical datasets to a clean maximum of 150 points (`max_points=150`) across any selected time window (1 hour to All-Time). Live WebSocket messages append new points dynamically on the client canvas (`appendLivePointToChart()`), delivering zero-lag line chart rendering without HTTP network refetching.

---

## 📡 MQTT Topic Architecture

| Topic | Direction | Purpose | Payload Format |
|-------|-----------|---------|----------------|
| `waterquality/WQM-001/sensors/live` | ESP32 ➔ AWS | Live 5-sensor telemetry | JSON |
| `waterquality/WQM-001/status` | ESP32 ➔ AWS | Device heartbeat, diagnostics & active config | JSON |
| `waterquality/WQM-001/command` | AWS ➔ ESP32 | Remote configuration & restart commands | JSON |

### Sample Status & Config Payload (`/status`):
```json
{
  "device_id": "WQM-001",
  "timestamp": 3600,
  "status": "online",
  "signal_quality": 24,
  "uptime_seconds": 3600,
  "free_heap": 215420,
  "firmware_version": "2.0.0",
  "config": {
    "sensor_read_interval": 2000,
    "mqtt_publish_interval": 5000,
    "status_publish_interval": 60000,
    "ph_phase_duration": 5000,
    "buffer_phase_duration": 2000,
    "tds_phase_duration": 5000,
    "deep_sleep_enabled": false,
    "deep_sleep_duration_sec": 0,
    "dashboard_poll_ms": 3000
  }
}
```

---

## 🔌 Hardware Pin Mapping & Wiring

| ESP32 Pin | Connected Device | Signal Type | Function |
|-----------|------------------|-------------|----------|
| **GPIO 1 (TX0)** | USB Bridge | UART0 TX | Serial Debugging to PC (115200 baud) |
| **GPIO 3 (RX0)** | USB Bridge | UART0 RX | Serial Debugging from PC |
| **GPIO 18** | SIMA7670C (TXD) | UART2 RX | Receives AT responses from 4G module |
| **GPIO 19** | SIMA7670C (RXD) | UART2 TX | Sends AT commands to 4G module |
| **GPIO 4** | DS18B20 Sensor | 1-Wire | Digital Temperature Reading (4.7kΩ pullup) |
| **GPIO 26** | TDS Relay Control | Digital Out | Switches TDS probe power (HIGH=off, LOW=on) |
| **GPIO 32** | TDS Sensor | ADC1_CH4 | Analog input (0–3.3V) |
| **GPIO 33** | Turbidity Sensor | ADC1_CH5 | Analog input (0–3.3V) |
| **GPIO 34** | pH Sensor | ADC1_CH6 | Analog input (Input-Only, 0–3.3V) |
| **GND** | System Ground | Power | Common ground with 4G module & sensors |

> **Power Supply Requirement**: The SIMA7670C modem requires an **external 5V/2A DC power supply** connected to its `VIN` and `GND` terminals. Do **not** power the modem directly from the ESP32's 3.3V or 5V pins. Ensure ESP32 GND and Modem GND are connected together.

---

## 🔬 Sensor Physics & Calibration Formulas

### 1. Phase Multiplexing (Crosstalk Prevention)
- **PHASE_PH**: TDS relay (GPIO26) is open (OFF). pH probe is sampled.
- **PHASE_BUFFER**: Both probes off. Residual electrical charge in water dissipates.
- **PHASE_TDS**: TDS relay (GPIO26) is closed (ON). TDS probe is sampled.

### 2. Certified Calibration Formulas (Lab Verified)
- **pH**: `pH = max(0, min(14, -3.0951 * V^2 + 5.6410 * V + 6.3216))`  *(Calibrated offset -1.530)*
- **TDS**: `TDS = (133.42 * V^3 - 255.86 * V^2 + 857.39 * V) * 1.426`  *(Calibrated scale x2.852)*
- **Turbidity**: `NTU = max(0, -1120.4 * V^2 + 5742.3 * V - 6748.5)`  *(Calibrated clear-water offset)*
- **Dissolved Oxygen**: `DO = max(0, 18.2573 - (0.41 * T) - (0.0008 * TDS) - (0.002 * NTU) + (0.03 * pH))`  *(Calibrated base constant)*

---

## 🌐 Server REST & WebSocket API Endpoints

All endpoints require HTTP Basic Auth (`admin` / `waterquality`).

| Method | Endpoint | Description | Sample Output / Query Params |
|--------|----------|-------------|------------------------------|
| `WS` | `/ws` | WebSockets live telemetry & status stream | JSON broadcast |
| `GET` | `/` | Serves the main HTML5 dashboard | HTML Web Page |
| `GET` | `/api/data/latest` | Returns most recent 5-sensor reading | `?device_id=WQM-001` |
| `GET` | `/api/data/history` | Returns historical readings for charts (smart downsampled to limit) | `?minutes=60&limit=150&device_id=WQM-001` |
| `GET` | `/api/config` | Returns current device runtime config | JSON config object |
| `POST` | `/api/config` | Pushes runtime config update via MQTT | JSON body |
| `POST` | `/api/command/restart` | Sends remote restart command via MQTT | `{"command": "restart"}` |
| `GET` | `/api/status` | Returns 4G modem & system status | `?device_id=WQM-001` |
| `GET` | `/api/stats` | Returns total stored database rows | `{"total_readings": 4520}` |

---

## 🛠️ Server Operations & Maintenance Guide

```bash
# Check status of MQTT Broker and Web Server
sudo systemctl status mosquitto
sudo systemctl status wqm-server

# Restart services
sudo systemctl restart mosquitto
sudo systemctl restart wqm-server

# View real-time server logs
sudo journalctl -u wqm-server -f

# Sniff live 4G MQTT traffic:
mosquitto_sub -h localhost -t "waterquality/#" -v
```

---

## 📂 File Directory Structure

```
WaterQualityMonitor/
├── firmware/
│   ├── config.h               # Central configuration (pins, APN, AWS IP)
│   ├── main.ino               # Synchronized ESP32 firmware
│   └── main/
│       ├── config.h           # Arduino IDE folder-level copy
│       └── main.ino           # NVS + RTC Guard + MQTT Command Firmware
├── server/
│   ├── server.py              # FastAPI server, WebSockets & MQTT subscriber
│   ├── database.py            # SQLite & CSV logging engine
│   ├── sensor_data.db         # SQLite Database (readings, status, config)
│   ├── requirements.txt       # Python dependencies
│   └── templates/
│       └── index.html         # Real-time Web Dashboard
├── LOCAL_SETUP.md             # Local testing guide
├── README.md                  # Quickstart guide
└── SYSTEM_DOCUMENTATION.md    # Complete system documentation
```
