"""
server.py — FastAPI Backend + MQTT Subscriber
Water Quality Monitor — AWS Server

This server:
  1. Subscribes to Mosquitto MQTT topics for real-time sensor data
  2. Stores data in SQLite via database.py
  3. Serves a real-time dashboard at /
  4. Exposes REST API for the dashboard frontend
"""

import json
import os
import secrets
import threading
import asyncio
from typing import List
from datetime import datetime, timezone, timedelta
from contextlib import asynccontextmanager

import paho.mqtt.client as mqtt
from fastapi import FastAPI, Request, Depends, HTTPException, status, WebSocket, WebSocketDisconnect
from fastapi.responses import HTMLResponse
from fastapi.security import HTTPBasic, HTTPBasicCredentials
from fastapi.templating import Jinja2Templates

import database as db

# =============================================================
# Configuration & State Management
# =============================================================
MQTT_BROKER = os.environ.get("MQTT_BROKER", "localhost")
MQTT_PORT = int(os.environ.get("MQTT_PORT", 1883))
MQTT_TOPIC_SENSORS = "waterquality/+/sensors/live"
MQTT_TOPIC_STATUS = "waterquality/+/status"

# Dashboard authentication
AUTH_USERNAME = os.environ.get("DASH_USERNAME", "admin")
AUTH_PASSWORD = os.environ.get("DASH_PASSWORD", "waterquality")

# In-memory fast cache
latest_data = {}
device_statuses = {}
device_configs = {}
last_seen_device_time = {}
device_reported_offline = set()

# Security
security = HTTPBasic()

# =============================================================
# WebSocket Real-Time Connection Manager
# =============================================================
class ConnectionManager:
    def __init__(self):
        self.active_connections: List[WebSocket] = []
        self.loop = None

    async def connect(self, websocket: WebSocket):
        await websocket.accept()
        self.active_connections.append(websocket)
        print(f"[WS] Client connected ({len(self.active_connections)} active)")

    def disconnect(self, websocket: WebSocket):
        if websocket in self.active_connections:
            self.active_connections.remove(websocket)
            print(f"[WS] Client disconnected ({len(self.active_connections)} active)")

    def broadcast_sync(self, message: dict):
        if not self.active_connections or not self.loop:
            return
        try:
            asyncio.run_coroutine_threadsafe(self.broadcast(message), self.loop)
        except Exception as e:
            print(f"[WS] Broadcast error: {e}")

    async def broadcast(self, message: dict):
        disconnected = []
        for connection in list(self.active_connections):
            try:
                await connection.send_json(message)
            except Exception:
                disconnected.append(connection)
        for conn in disconnected:
            self.disconnect(conn)

manager = ConnectionManager()


def verify_credentials(credentials: HTTPBasicCredentials = Depends(security)):
    """Verify HTTP Basic Auth credentials for dashboard access."""
    correct_username = secrets.compare_digest(credentials.username, AUTH_USERNAME)
    correct_password = secrets.compare_digest(credentials.password, AUTH_PASSWORD)
    if not (correct_username and correct_password):
        raise HTTPException(
            status_code=status.HTTP_401_UNAUTHORIZED,
            detail="Invalid credentials",
            headers={"WWW-Authenticate": "Basic realm=\"Water Quality Monitor\""},
        )
    return credentials.username

# =============================================================
# MQTT Client
# =============================================================

def on_connect(client, userdata, flags, reason_code, properties=None):
    """Called when connected to the MQTT broker."""
    print(f"[MQTT] Connected to broker (rc={reason_code})")
    client.subscribe(MQTT_TOPIC_SENSORS, qos=1)
    client.subscribe(MQTT_TOPIC_STATUS, qos=1)
    print(f"[MQTT] Subscribed to: {MQTT_TOPIC_SENSORS}")
    print(f"[MQTT] Subscribed to: {MQTT_TOPIC_STATUS}")


def on_message(client, userdata, msg):
    """Called when a message is received from MQTT."""
    global latest_data, device_statuses, device_configs

    try:
        payload = json.loads(msg.payload.decode("utf-8"))
        topic = msg.topic

        if "/sensors/live" in topic:
            # Sensor data message
            device_id = payload.get("device_id", "WQM-001")
            now_utc = datetime.now(timezone.utc)
            last_seen_device_time[device_id] = now_utc
            if device_id in device_reported_offline:
                device_reported_offline.remove(device_id)

            latest_data[device_id] = {
                **payload,
                "received_at": now_utc.isoformat()
            }

            # Store in database
            db.store_sensor_reading(payload)

            # Broadcast real-time update via WebSockets
            manager.broadcast_sync({
                "type": "sensor_update",
                "data": latest_data[device_id]
            })

            # Log to console
            sensors = payload.get("sensors", {})
            print(f"[DATA] {device_id} | "
                  f"pH:{sensors.get('ph', {}).get('value', '?')} | "
                  f"TDS:{sensors.get('tds', {}).get('value', '?')} | "
                  f"Temp:{sensors.get('temperature', {}).get('value', '?')}°C | "
                  f"Turb:{sensors.get('turbidity', {}).get('value', '?')} NTU | "
                  f"DO:{sensors.get('dissolved_oxygen', {}).get('value', '?')} mg/L")

        elif "/status" in topic:
            # Device status heartbeat (or LWT offline message)
            device_id = payload.get("device_id", "WQM-001")
            now_utc = datetime.now(timezone.utc)
            if payload.get("status") != "offline":
                last_seen_device_time[device_id] = now_utc
                if device_id in device_reported_offline:
                    device_reported_offline.remove(device_id)

            device_statuses[device_id] = {
                **payload,
                "received_at": now_utc.isoformat()
            }
            db.update_device_status(payload)

            # Check if this is an LWT offline message from the broker
            if payload.get("status") == "offline":
                print(f"[LWT] Device {device_id} went OFFLINE (reason: {payload.get('reason', 'unknown')})")
                # Broadcast immediate offline event to all WebSocket clients
                manager.broadcast_sync({
                    "type": "device_offline",
                    "data": {
                        "device_id": device_id,
                        "status": "offline",
                        "reason": payload.get("reason", "lwt_disconnect"),
                        "timestamp": datetime.now(timezone.utc).isoformat()
                    }
                })
            else:
                # Normal status heartbeat — device is alive
                # Extract runtime config if present & save to SQLite DB
                if "config" in payload:
                    device_configs[device_id] = {
                        **payload["config"],
                        "last_reported_at": datetime.now(timezone.utc).isoformat()
                    }
                    db.save_device_config(device_id, payload["config"])

                # Broadcast real-time status update via WebSockets
                manager.broadcast_sync({
                    "type": "status_update",
                    "data": device_statuses[device_id]
                })

            print(f"[STATUS] {device_id} | Status:{payload.get('status', 'online')} | "
                  f"Signal:{payload.get('signal_quality')} | "
                  f"Uptime:{payload.get('uptime_seconds')}s | "
                  f"Heap:{payload.get('free_heap')}")

    except json.JSONDecodeError as e:
        print(f"[MQTT] JSON parse error: {e}")
    except Exception as e:
        print(f"[MQTT] Error processing message: {e}")


def on_disconnect(client, userdata, flags, reason_code, properties=None):
    """Called when disconnected from MQTT broker."""
    print(f"[MQTT] Disconnected (rc={reason_code}). Will auto-reconnect.")


def start_mqtt_client():
    """Start the MQTT client in a background thread."""
    client = mqtt.Client(
        callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
        client_id="wqm-server",
        protocol=mqtt.MQTTv311
    )
    client.on_connect = on_connect
    client.on_message = on_message
    client.on_disconnect = on_disconnect

    # Enable auto-reconnect
    client.reconnect_delay_set(min_delay=1, max_delay=30)

    try:
        client.connect(MQTT_BROKER, MQTT_PORT, keepalive=60)
        client.loop_start()  # Runs in background thread
        print(f"[MQTT] Client started, connecting to {MQTT_BROKER}:{MQTT_PORT}")
        return client
    except Exception as e:
        print(f"[MQTT] Failed to connect: {e}")
        print("[MQTT] Server will run without MQTT. Data can be checked via API.")
        return None


# =============================================================
# FastAPI App
# =============================================================

mqtt_client = None

async def device_watchdog_loop():
    """Background loop checking if any device telemetry has stopped arriving."""
    while True:
        try:
            await asyncio.sleep(2)
            now = datetime.now(timezone.utc)
            for device_id, last_time in list(last_seen_device_time.items()):
                elapsed = (now - last_time).total_seconds()
                # Telemetry publish interval is 5s (5000ms).
                # If no packets received for > 12s (missed ~2-3 packets), declare device OFFLINE!
                if elapsed > 12.0 and device_id not in device_reported_offline:
                    device_reported_offline.add(device_id)
                    print(f"[WATCHDOG] Telemetry timeout for {device_id} ({elapsed:.1f}s > 12s). Marking OFFLINE.")

                    offline_payload = {
                        "device_id": device_id,
                        "status": "offline",
                        "reason": "telemetry_timeout",
                        "received_at": now.isoformat()
                    }
                    device_statuses[device_id] = offline_payload
                    db.update_device_status(offline_payload)

                    # Broadcast immediate device_offline event over WebSockets
                    manager.broadcast_sync({
                        "type": "device_offline",
                        "data": offline_payload
                    })
        except asyncio.CancelledError:
            break
        except Exception as e:
            print(f"[WATCHDOG] Error: {e}")


@asynccontextmanager
async def lifespan(app: FastAPI):
    """Startup and shutdown events."""
    global mqtt_client

    # Startup
    print("=" * 50)
    print(" Water Quality Monitor — Server Starting")
    print("=" * 50)

    manager.loop = asyncio.get_running_loop()
    db.init_db()
    mqtt_client = start_mqtt_client()
    watchdog_task = asyncio.create_task(device_watchdog_loop())

    print("[SERVER] Dashboard: http://0.0.0.0:8000")
    print(f"[SERVER] Auth:      {AUTH_USERNAME} / {'*' * len(AUTH_PASSWORD)}")
    print("[SERVER] API Docs:  http://0.0.0.0:8000/docs")
    print("=" * 50)

    yield

    # Shutdown
    watchdog_task.cancel()
    if mqtt_client:
        mqtt_client.loop_stop()
        mqtt_client.disconnect()
        print("[MQTT] Client stopped.")


app = FastAPI(
    title="Water Quality Monitor API",
    description="Real-time water quality monitoring system",
    version="2.0.0",
    lifespan=lifespan
)

# Templates
templates_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "templates")
templates = Jinja2Templates(directory=templates_dir)


# =============================================================
# Routes — Real-Time WebSocket & Dashboard
# =============================================================

@app.websocket("/ws")
async def websocket_endpoint(websocket: WebSocket):
    """WebSocket endpoint for real-time live sensor streaming."""
    await manager.connect(websocket)
    try:
        while True:
            await websocket.receive_text()
    except WebSocketDisconnect:
        manager.disconnect(websocket)


@app.get("/", response_class=HTMLResponse)
async def dashboard(request: Request, username: str = Depends(verify_credentials)):
    """Serve the main dashboard page (requires authentication, no-cache)."""
    response = templates.TemplateResponse("index.html", {"request": request})
    response.headers["Cache-Control"] = "no-cache, no-store, must-revalidate, max-age=0"
    response.headers["Pragma"] = "no-cache"
    response.headers["Expires"] = "0"
    return response


# =============================================================
# Routes — REST API
# =============================================================

@app.get("/api/data/latest")
async def api_latest_data(device_id: str = "WQM-001"):
    """Get the latest sensor reading for a device."""
    # First check in-memory cache
    if device_id in latest_data:
        return latest_data[device_id]

    # Fallback to database
    reading = db.get_latest_reading(device_id)
    if reading:
        return {
            "device_id": device_id,
            "received_at": reading["timestamp"],
            "sensors": {
                "ph": {"value": reading["ph"], "unit": "pH"},
                "tds": {"value": reading["tds"], "unit": "ppm"},
                "temperature": {"value": reading["temperature"], "unit": "°C"},
                "turbidity": {"value": reading["turbidity"], "unit": "NTU"},
                "dissolved_oxygen": {"value": reading["dissolved_oxygen"], "unit": "mg/L"}
            },
            "metadata": {
                "signal_quality": reading.get("signal_quality", 0),
                "firmware_version": reading.get("firmware_version", "unknown"),
                "uptime_seconds": reading.get("uptime_seconds", 0)
            }
        }

    # No data at all
    return {
        "device_id": device_id,
        "sensors": {
            "ph": {"value": 0, "unit": "pH"},
            "tds": {"value": 0, "unit": "ppm"},
            "temperature": {"value": 0, "unit": "°C"},
            "turbidity": {"value": 0, "unit": "NTU"},
            "dissolved_oxygen": {"value": 0, "unit": "mg/L"}
        },
        "metadata": {"signal_quality": 0}
    }


@app.get("/api/data/history")
async def api_history(minutes: int = 10080, device_id: str = "WQM-001"):
    """Get historical sensor data for chart rendering."""
    readings = db.get_readings_history(minutes=minutes, device_id=device_id)
    return {
        "device_id": device_id,
        "minutes": minutes,
        "count": len(readings),
        "readings": readings
    }


@app.get("/api/status")
async def api_device_status(device_id: str = "WQM-001"):
    """Get current device connection status."""
    # In-memory status
    if device_id in device_statuses:
        status_data = device_statuses[device_id]
        # Check staleness
        received = datetime.fromisoformat(status_data.get("received_at", "2000-01-01"))
        if (datetime.now(timezone.utc) - received).total_seconds() > 12:
            status_data["status"] = "offline"
        return status_data

    # Database fallback
    return db.get_device_status(device_id)


@app.get("/api/stats")
async def api_stats(device_id: str = "WQM-001"):
    """Get database statistics."""
    return {
        "device_id": device_id,
        "total_readings": db.get_reading_count(device_id),
        "server_time": datetime.now().isoformat()
    }


# =============================================================
# Routes — Device Configuration
# =============================================================

# Default config values (match firmware config.h defaults)
DEFAULT_CONFIG = {
    "sensor_read_interval": 2000,
    "mqtt_publish_interval": 5000,
    "status_publish_interval": 60000,
    "ph_phase_duration": 5000,
    "buffer_phase_duration": 2000,
    "tds_phase_duration": 5000,
    "deep_sleep_enabled": False,
    "deep_sleep_duration_sec": 0,
    "dashboard_poll_ms": 3000,
}

@app.get("/api/config")
async def api_get_config(device_id: str = "WQM-001",
                        username: str = Depends(verify_credentials)):
    """Get the current device runtime configuration.
    Returns in-memory cache, SQLite DB, or defaults if never reported."""
    if device_id in device_configs:
        return {
            "device_id": device_id,
            "source": "device_reported",
            **device_configs[device_id]
        }

    # Check SQLite DB
    db_cfg = db.get_device_config(device_id)
    if db_cfg:
        return {
            "device_id": device_id,
            "source": "database_persisted",
            **db_cfg
        }

    return {
        "device_id": device_id,
        "source": "defaults",
        **DEFAULT_CONFIG
    }


@app.post("/api/config")
async def api_set_config(request: Request, device_id: str = "WQM-001",
                        username: str = Depends(verify_credentials)):
    """Push a configuration update to the device via MQTT.
    Accepts any subset of config keys — only provided keys are updated on the device."""
    global mqtt_client

    body = await request.json()

    # Build the MQTT command payload
    command_payload = {"command": "set_config"}

    valid_keys = [
        "sensor_read_interval", "mqtt_publish_interval", "status_publish_interval",
        "ph_phase_duration", "buffer_phase_duration", "tds_phase_duration",
        "deep_sleep_enabled", "deep_sleep_duration_sec", "dashboard_poll_ms"
    ]

    for key in valid_keys:
        if key in body:
            command_payload[key] = body[key]

    if len(command_payload) <= 1:
        raise HTTPException(status_code=400, detail="No valid config keys provided")

    # Save to SQLite database
    db.save_device_config(device_id, body)

    if mqtt_client and mqtt_client.is_connected():
        topic = f"waterquality/{device_id}/command"
        mqtt_client.publish(topic, json.dumps(command_payload), qos=1)
        print(f"[CONFIG] Pushed config update to {device_id}: {command_payload}")
        return {"success": True, "message": f"Config update sent to {device_id}", "payload": command_payload}

    return {"success": False, "message": "Config saved to DB (MQTT broker not connected)"}


@app.post("/api/command/restart")
async def api_restart_device(device_id: str = "WQM-001",
                            username: str = Depends(verify_credentials)):
    """Send a restart command to the device via MQTT."""
    global mqtt_client

    if mqtt_client and mqtt_client.is_connected():
        topic = f"waterquality/{device_id}/command"
        payload = json.dumps({"command": "restart", "timestamp": datetime.now().isoformat()})
        mqtt_client.publish(topic, payload, qos=1)
        print(f"[CMD] Restart command sent to {device_id}")
        return {"success": True, "message": f"Restart command sent to {device_id}"}

    return {"success": False, "message": "MQTT broker not connected"}


@app.post("/api/command/calibrate")
async def api_calibrate(device_id: str = "WQM-001",
                       username: str = Depends(verify_credentials)):
    """Publish a calibration command to the device via MQTT."""
    global mqtt_client

    if mqtt_client and mqtt_client.is_connected():
        topic = f"waterquality/{device_id}/command"
        payload = json.dumps({"command": "calibrate", "timestamp": datetime.now().isoformat()})
        mqtt_client.publish(topic, payload, qos=1)
        return {"success": True, "message": f"Calibration command sent to {device_id}"}

    return {"success": False, "message": "MQTT broker not connected"}


# =============================================================
# Run Server
# =============================================================

if __name__ == "__main__":
    import uvicorn
    uvicorn.run(
        "server:app",
        host="0.0.0.0",
        port=8000,
        reload=False,
        log_level="info"
    )

