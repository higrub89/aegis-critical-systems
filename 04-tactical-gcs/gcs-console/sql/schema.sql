-- AEGIS Critical Systems - Tactical GCS Audit & Telemetry SQL Schema (SQLite)
-- Subsystem: 04-tactical-gcs (Java 21 LTS)

PRAGMA journal_mode = WAL;
PRAGMA foreign_keys = ON;

CREATE TABLE IF NOT EXISTS telemetry_audit (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    seq             INTEGER NOT NULL,
    msg_type        INTEGER NOT NULL,
    pack_voltage_mv INTEGER NOT NULL,
    pack_current_ma INTEGER NOT NULL,
    cell_temp_dc    INTEGER NOT NULL,
    contactor_state INTEGER NOT NULL,
    fault_mask      INTEGER NOT NULL,
    timestamp_ms    INTEGER NOT NULL,
    crc32c_hex      TEXT    NOT NULL,
    recorded_at     TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE IF NOT EXISTS dtc_events (
    id              INTEGER PRIMARY KEY AUTOINCREMENT,
    telemetry_seq   INTEGER NOT NULL,
    dtc_code        TEXT    NOT NULL,
    severity        TEXT    NOT NULL,
    fault_mask      INTEGER NOT NULL,
    contactor_state INTEGER NOT NULL,
    recorded_at     TEXT    NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE INDEX IF NOT EXISTS idx_telemetry_seq ON telemetry_audit(seq);
CREATE INDEX IF NOT EXISTS idx_dtc_code ON dtc_events(dtc_code);
