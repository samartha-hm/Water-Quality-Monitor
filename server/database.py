"""
database.py — SQLite Data Layer
Water Quality Monitor — AWS Server

Handles all data persistence:
  - Stores incoming sensor readings in a SQLite database
  - Provides time-range queries for the dashboard charts
  - Auto-creates tables on first run
  - Exports daily CSV logs alongside the database
"""

import sqlite3
import os
import csv
from datetime import datetime, timedelta
from typing import Optional
from contextlib import contextmanager

# Database file path (relative to server.py location)
DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sensor_data.db")
CSV_LOG_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "logs")


@contextmanager
def get_db():
    """Context manager for database connections with WAL mode for concurrent reads."""
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    conn.execute("PRAGMA journal_mode=WAL")
    conn.execute("PRAGMA foreign_keys=ON")
    try:
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def init_db():
    """Create tables if they don't exist. Called once at server startup."""
    with get_db() as conn:
        conn.execute("""
            CREATE TABLE IF NOT EXISTS sensor_readings (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                timestamp TEXT NOT NULL,
                device_id TEXT NOT NULL DEFAULT 'WQM-001',
                ph REAL,
                tds REAL,
                temperature REAL,
                turbidity REAL,
                dissolved_oxygen REAL,
                signal_quality INTEGER,
                firmware_version TEXT,
                uptime_seconds INTEGER
            )
        """)

        conn.execute("""
            CREATE TABLE IF NOT EXISTS device_status (
                device_id TEXT PRIMARY KEY,
                last_seen TEXT,
                status TEXT DEFAULT 'offline',
                signal_quality INTEGER DEFAULT 0,
                uptime_seconds INTEGER DEFAULT 0,
                firmware_version TEXT,
                free_heap INTEGER
            )
        """)

        conn.execute("""
            CREATE TABLE IF NOT EXISTS device_config (
                device_id TEXT PRIMARY KEY,
                updated_at TEXT,
                sensor_read_interval INTEGER,
                mqtt_publish_interval INTEGER,
                status_publish_interval INTEGER,
                ph_phase_duration INTEGER,
                buffer_phase_duration INTEGER,
                tds_phase_duration INTEGER,
                deep_sleep_enabled INTEGER,
                deep_sleep_duration_sec INTEGER,
                dashboard_poll_ms INTEGER
            )
        """)

        # Index for time-range queries
        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_readings_timestamp
            ON sensor_readings (timestamp DESC)
        """)

        conn.execute("""
            CREATE INDEX IF NOT EXISTS idx_readings_device
            ON sensor_readings (device_id, timestamp DESC)
        """)

    # Create logs directory
    os.makedirs(CSV_LOG_DIR, exist_ok=True)
    print(f"[DB] Database initialized at {DB_PATH}")


def save_device_config(device_id: str, cfg: dict):
    """Save or update device runtime config in database."""
    now = datetime.now().isoformat()
    with get_db() as conn:
        conn.execute("""
            INSERT INTO device_config (
                device_id, updated_at, sensor_read_interval, mqtt_publish_interval,
                status_publish_interval, ph_phase_duration, buffer_phase_duration,
                tds_phase_duration, deep_sleep_enabled, deep_sleep_duration_sec, dashboard_poll_ms
            ) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                updated_at = excluded.updated_at,
                sensor_read_interval = COALESCE(excluded.sensor_read_interval, sensor_read_interval),
                mqtt_publish_interval = COALESCE(excluded.mqtt_publish_interval, mqtt_publish_interval),
                status_publish_interval = COALESCE(excluded.status_publish_interval, status_publish_interval),
                ph_phase_duration = COALESCE(excluded.ph_phase_duration, ph_phase_duration),
                buffer_phase_duration = COALESCE(excluded.buffer_phase_duration, buffer_phase_duration),
                tds_phase_duration = COALESCE(excluded.tds_phase_duration, tds_phase_duration),
                deep_sleep_enabled = COALESCE(excluded.deep_sleep_enabled, deep_sleep_enabled),
                deep_sleep_duration_sec = COALESCE(excluded.deep_sleep_duration_sec, deep_sleep_duration_sec),
                dashboard_poll_ms = COALESCE(excluded.dashboard_poll_ms, dashboard_poll_ms)
        """, (
            device_id, now,
            cfg.get("sensor_read_interval"), cfg.get("mqtt_publish_interval"),
            cfg.get("status_publish_interval"), cfg.get("ph_phase_duration"),
            cfg.get("buffer_phase_duration"), cfg.get("tds_phase_duration"),
            1 if cfg.get("deep_sleep_enabled") else 0 if "deep_sleep_enabled" in cfg else None,
            cfg.get("deep_sleep_duration_sec"), cfg.get("dashboard_poll_ms")
        ))


def get_device_config(device_id: str = "WQM-001") -> Optional[dict]:
    """Retrieve saved device runtime config from database."""
    with get_db() as conn:
        row = conn.execute("""
            SELECT * FROM device_config WHERE device_id = ?
        """, (device_id,)).fetchone()
        if row:
            res = dict(row)
            res["deep_sleep_enabled"] = bool(res.get("deep_sleep_enabled"))
            return res
    return None


def store_sensor_reading(data: dict):
    """
    Store a single sensor reading from MQTT payload.
    
    Expected data format (from ESP32 JSON):
    {
        "device_id": "WQM-001",
        "timestamp": 3600,
        "sensors": {
            "ph": {"value": 7.23, "unit": "pH"},
            "tds": {"value": 285, "unit": "ppm"},
            "temperature": {"value": 28.5, "unit": "°C"},
            "turbidity": {"value": 2150.3, "unit": "NTU"},
            "dissolved_oxygen": {"value": 6.8, "unit": "mg/L"}
        },
        "metadata": {
            "signal_quality": 18,
            "firmware_version": "2.0.0",
            "uptime_seconds": 3600
        }
    }
    """
    now = datetime.now().isoformat()
    device_id = data.get("device_id", "WQM-001")
    sensors = data.get("sensors", {})
    metadata = data.get("metadata", {})

    ph = sensors.get("ph", {}).get("value")
    tds = sensors.get("tds", {}).get("value")
    temperature = sensors.get("temperature", {}).get("value")
    turbidity = sensors.get("turbidity", {}).get("value")
    dissolved_oxygen = sensors.get("dissolved_oxygen", {}).get("value")
    signal_quality = metadata.get("signal_quality")
    firmware_version = metadata.get("firmware_version")
    uptime_seconds = metadata.get("uptime_seconds")

    with get_db() as conn:
        conn.execute("""
            INSERT INTO sensor_readings
                (timestamp, device_id, ph, tds, temperature, turbidity,
                 dissolved_oxygen, signal_quality, firmware_version, uptime_seconds)
            VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)
        """, (now, device_id, ph, tds, temperature, turbidity,
              dissolved_oxygen, signal_quality, firmware_version, uptime_seconds))

    # Also append to daily CSV log
    _append_csv_log(now, device_id, ph, tds, temperature, turbidity,
                    dissolved_oxygen, signal_quality)


def update_device_status(data: dict):
    """Update the device status from a status heartbeat message."""
    device_id = data.get("device_id", "WQM-001")
    now = datetime.now().isoformat()

    with get_db() as conn:
        conn.execute("""
            INSERT INTO device_status (device_id, last_seen, status, signal_quality,
                                       uptime_seconds, firmware_version, free_heap)
            VALUES (?, ?, ?, ?, ?, ?, ?)
            ON CONFLICT(device_id) DO UPDATE SET
                last_seen = excluded.last_seen,
                status = excluded.status,
                signal_quality = excluded.signal_quality,
                uptime_seconds = excluded.uptime_seconds,
                firmware_version = excluded.firmware_version,
                free_heap = excluded.free_heap
        """, (device_id, now, data.get("status", "online"),
              data.get("signal_quality", 0), data.get("uptime_seconds", 0),
              data.get("firmware_version"), data.get("free_heap")))


def get_latest_reading(device_id: str = "WQM-001") -> Optional[dict]:
    """Get the most recent sensor reading for a device."""
    with get_db() as conn:
        row = conn.execute("""
            SELECT * FROM sensor_readings
            WHERE device_id = ?
            ORDER BY timestamp DESC
            LIMIT 1
        """, (device_id,)).fetchone()

        if row:
            return dict(row)
    return None


def get_readings_history(minutes: int = 60, device_id: str = "WQM-001", max_points: int = 150) -> list:
    """Get sensor readings for a device with smart even downsampling to max_points.
    If minutes >= 10080 or <= 0, returns historical readings from database up to max_points."""
    with get_db() as conn:
        if minutes >= 10080 or minutes <= 0:
            rows = conn.execute("""
                SELECT timestamp, ph, tds, temperature, turbidity, dissolved_oxygen
                FROM sensor_readings
                WHERE device_id = ?
                ORDER BY timestamp ASC
            """, (device_id,)).fetchall()
        else:
            cutoff = (datetime.now(timezone.utc) - timedelta(minutes=minutes)).isoformat()
            rows = conn.execute("""
                SELECT timestamp, ph, tds, temperature, turbidity, dissolved_oxygen
                FROM sensor_readings
                WHERE device_id = ? AND timestamp >= ?
                ORDER BY timestamp ASC
            """, (device_id, cutoff)).fetchall()

            if not rows:
                # Fallback if cutoff filtered out all historical data
                rows = conn.execute("""
                    SELECT timestamp, ph, tds, temperature, turbidity, dissolved_oxygen
                    FROM sensor_readings
                    WHERE device_id = ?
                    ORDER BY timestamp ASC
                """, (device_id,)).fetchall()

        all_readings = [dict(row) for row in rows]
        total = len(all_readings)

        # Smart Downsampling: step-sample if dataset size exceeds max_points
        if total > max_points and max_points > 0:
            step = total / float(max_points)
            downsampled = []
            for i in range(max_points):
                idx = min(total - 1, int(i * step))
                downsampled.append(all_readings[idx])
            # Ensure latest reading is always included
            if downsampled and downsampled[-1] != all_readings[-1]:
                downsampled[-1] = all_readings[-1]
            return downsampled

        return all_readings


def get_device_status(device_id: str = "WQM-001") -> Optional[dict]:
    """Get the current device status."""
    with get_db() as conn:
        row = conn.execute("""
            SELECT * FROM device_status WHERE device_id = ?
        """, (device_id,)).fetchone()

        if row:
            result = dict(row)
            # Check if device is stale (no update in 2+ minutes)
            if result.get("last_seen"):
                last_seen = datetime.fromisoformat(result["last_seen"])
                if datetime.now() - last_seen > timedelta(minutes=2):
                    result["status"] = "offline"
            return result
    return {"device_id": device_id, "status": "never_connected"}


def get_reading_count(device_id: str = "WQM-001") -> int:
    """Get total number of readings stored for a device."""
    with get_db() as conn:
        row = conn.execute("""
            SELECT COUNT(*) as count FROM sensor_readings WHERE device_id = ?
        """, (device_id,)).fetchone()
        return row["count"] if row else 0


def cleanup_old_data(retention_days: int = 30):
    """Delete sensor readings older than the retention period."""
    cutoff = (datetime.now() - timedelta(days=retention_days)).isoformat()
    with get_db() as conn:
        result = conn.execute("""
            DELETE FROM sensor_readings WHERE timestamp < ?
        """, (cutoff,))
        if result.rowcount > 0:
            print(f"[DB] Cleaned up {result.rowcount} old readings (>{retention_days} days)")


def _append_csv_log(timestamp, device_id, ph, tds, temperature, turbidity,
                    dissolved_oxygen, signal_quality):
    """Append a reading to the daily CSV log file."""
    date_str = datetime.now().strftime("%Y-%m-%d")
    filepath = os.path.join(CSV_LOG_DIR, f"sensor_data_{date_str}.csv")

    file_exists = os.path.exists(filepath)

    try:
        with open(filepath, "a", newline="", encoding="utf-8") as f:
            writer = csv.writer(f)
            if not file_exists:
                writer.writerow(["timestamp", "device_id", "ph", "tds",
                                 "temperature", "turbidity", "dissolved_oxygen",
                                 "signal_quality"])
            writer.writerow([timestamp, device_id, ph, tds, temperature,
                             turbidity, dissolved_oxygen, signal_quality])
    except Exception as e:
        print(f"[DB] CSV log error: {e}")
