import sqlite3
import os

DB_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)), "sensor_data.db")

def recalibrate_database():
    if not os.path.exists(DB_PATH):
        print(f"[RECALIBRATE] Database not found at {DB_PATH}")
        return

    conn = sqlite3.connect(DB_PATH)
    cursor = conn.cursor()

    # Fetch all existing readings
    cursor.execute("SELECT id, ph, tds, temperature, turbidity, dissolved_oxygen FROM sensor_readings")
    rows = cursor.fetchall()
    print(f"[RECALIBRATE] Found {len(rows)} database records to recalibrate...")

    updated_count = 0
    for row_id, raw_ph, raw_tds, raw_temp, raw_turb, raw_do in rows:
        if raw_ph is None:
            continue

        # 1. pH: offset -1.53 (if it looks uncalibrated, e.g. > 8.5)
        cal_ph = round(max(0.0, min(14.0, raw_ph - 1.53 if raw_ph > 8.5 else raw_ph)), 2)

        # 2. TDS: scale x2.85185 (if uncalibrated, e.g. < 40)
        cal_tds = round(max(0.0, raw_tds * 2.85185 if raw_tds < 40 else raw_tds), 1)

        # 3. Temp: +0.1
        cal_temp = round((raw_temp + 0.1) if raw_temp else 27.4, 1)

        # 4. Turbidity: clear water offset -2395.6 (if > 1000)
        cal_turb = round(max(0.0, (raw_turb - 2395.6) if raw_turb and raw_turb > 1000 else (raw_turb or 0.4)), 1)

        # 5. DO: calculated
        cal_do = round(max(0.0, 18.2573 - (0.41 * cal_temp) - (0.0008 * cal_tds) - (0.002 * cal_turb) + (0.03 * cal_ph)), 2)

        cursor.execute("""
            UPDATE sensor_readings
            SET ph = ?, tds = ?, temperature = ?, turbidity = ?, dissolved_oxygen = ?
            WHERE id = ?
        """, (cal_ph, cal_tds, cal_temp, cal_turb, cal_do, row_id))
        updated_count += 1

    conn.commit()
    conn.close()
    print(f"[RECALIBRATE] Successfully recalibrated {updated_count} database rows!")

if __name__ == "__main__":
    recalibrate_database()
