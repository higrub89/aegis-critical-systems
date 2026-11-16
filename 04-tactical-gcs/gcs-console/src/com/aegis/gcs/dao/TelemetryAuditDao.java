package com.aegis.gcs.dao;

import com.aegis.gcs.dto.TelemetryFrameDto;
import java.io.IOException;
import java.util.List;

/**
 * Data Access Object contract for deterministic TM/TC telemetry and DTC hazard persistence.
 */
public interface TelemetryAuditDao extends AutoCloseable {

    /**
     * Initializes database schema tables and indexes idempotently.
     *
     * @throws IOException if storage initialization fails
     */
    void initializeSchema() throws IOException;

    /**
     * Persists a verified telemetry frame and records a DTC event if hazards are active.
     *
     * @param frame verified telemetry DTO
     * @throws IOException if persistence fails
     */
    void saveFrameAndAudit(TelemetryFrameDto frame) throws IOException;

    /**
     * Returns total persisted telemetry frame count.
     *
     * @return row count in {@code telemetry_audit}
     * @throws IOException if query fails
     */
    long countTelemetryFrames() throws IOException;

    /**
     * Returns recorded DTC hazard event descriptions ordered by sequence.
     *
     * @return list of formatted DTC audit rows
     * @throws IOException if query fails
     */
    List<String> listDtcEvents() throws IOException;

    @Override
    void close() throws IOException;
}
