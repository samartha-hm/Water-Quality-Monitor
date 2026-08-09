import database as db
print("COUNT:", db.get_reading_count("WQM-001"))
print("LATEST:", db.get_latest_reading("WQM-001"))
print("HISTORY (5m):", len(db.get_readings_history(5)))
print("HISTORY (1440m):", len(db.get_readings_history(1440)))
