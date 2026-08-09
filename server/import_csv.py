import os
import csv
import glob
import database as db

db.init_db()

csv_files = glob.glob(os.path.join(db.CSV_LOG_DIR, "sensor_data_*.csv"))
total_imported = 0

for filepath in csv_files:
    print(f"[IMPORT] Processing {filepath}...")
    with open(filepath, "r", encoding="utf-8") as f:
        reader = csv.DictReader(f)
        with db.get_db() as conn:
            for row in reader:
                try:
                    conn.execute("""
                        INSERT INTO sensor_readings
                            (timestamp, device_id, ph, tds, temperature, turbidity,
                             dissolved_oxygen, signal_quality)
                        VALUES (?, ?, ?, ?, ?, ?, ?, ?)
                    """, (
                        row["timestamp"],
                        row.get("device_id", "WQM-001"),
                        float(row["ph"]) if row.get("ph") else None,
                        float(row["tds"]) if row.get("tds") else None,
                        float(row["temperature"]) if row.get("temperature") else None,
                        float(row["turbidity"]) if row.get("turbidity") else None,
                        float(row["dissolved_oxygen"]) if row.get("dissolved_oxygen") else None,
                        int(row["signal_quality"]) if row.get("signal_quality") else 0
                    ))
                    total_imported += 1
                except Exception as e:
                    pass

print(f"[IMPORT] Successfully imported {total_imported} rows into SQLite database!")
