package com.aegis.gcs.dao;

import com.aegis.gcs.dto.TelemetryFrameDto;
import java.io.BufferedReader;
import java.io.BufferedWriter;
import java.io.IOException;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Pattern;

/**
 * SQLite-backed {@link TelemetryAuditDao} using strict {@code sqlite3} parameterized statements
 * and deterministic {@code try-with-resources} process lifecycle.
 */
public final class SqliteCliAuditDao implements TelemetryAuditDao {

    private static final Pattern SAFE_TOKEN = Pattern.compile("^[0-9A-Za-z_-]{1,32}$");

    private final Path dbPath;
    private final Path schemaPath;

    /**
     * Constructs a DAO targeting {@code dbPath} initialized from {@code schemaPath}.
     *
     * @param dbPath     SQLite database file path
     * @param schemaPath SQL DDL schema file path
     */
    public SqliteCliAuditDao(Path dbPath, Path schemaPath) {
        this.dbPath = dbPath;
        this.schemaPath = schemaPath;
    }

    @Override
    public void initializeSchema() throws IOException {
        Path parent = dbPath.getParent();
        if (parent != null) {
            Files.createDirectories(parent);
        }
        String ddl = Files.readString(schemaPath, StandardCharsets.UTF_8);
        executeSql(ddl);
    }

    @Override
    public synchronized void saveFrameAndAudit(TelemetryFrameDto frame) throws IOException {
        String crcHex = String.format("%08X", frame.crc32c());
        validateToken(crcHex);

        StringBuilder sql = new StringBuilder(512);
        sql.append("BEGIN IMMEDIATE;\n");
        sql.append(".parameter init\n");
        sql.append(".parameter set :seq ").append(frame.seq()).append('\n');
        sql.append(".parameter set :msg_type ").append(frame.msgType()).append('\n');
        sql.append(".parameter set :volt ").append(frame.packVoltageMv()).append('\n');
        sql.append(".parameter set :curr ").append(frame.packCurrentMa()).append('\n');
        sql.append(".parameter set :temp ").append(frame.maxCellTempDc()).append('\n');
        sql.append(".parameter set :cont ").append(frame.contactorState()).append('\n');
        sql.append(".parameter set :fmask ").append(frame.faultMask()).append('\n');
        sql.append(".parameter set :ts ").append(frame.timestampMs()).append('\n');
        sql.append(".parameter set :crc \"'").append(crcHex).append("'\"\n");
        sql.append(
                "INSERT INTO telemetry_audit "
                        + "(seq, msg_type, pack_voltage_mv, pack_current_ma, cell_temp_dc, "
                        + "contactor_state, fault_mask, timestamp_ms, crc32c_hex) "
                        + "VALUES (:seq, :msg_type, :volt, :curr, :temp, :cont, :fmask, :ts, :crc);\n");

        if (frame.hasActiveHazard()) {
            String dtcCode = resolveDtcCode(frame);
            String severity = (frame.contactorState() == 0) ? "MIL-882E-CAT-I" : "MIL-882E-CAT-II";
            validateToken(dtcCode);
            validateToken(severity);
            sql.append(".parameter set :dtc \"'").append(dtcCode).append("'\"\n");
            sql.append(".parameter set :sev \"'").append(severity).append("'\"\n");
            sql.append(
                    "INSERT INTO dtc_events "
                            + "(telemetry_seq, dtc_code, severity, fault_mask, contactor_state) "
                            + "VALUES (:seq, :dtc, :sev, :fmask, :cont);\n");
        }

        sql.append("COMMIT;\n");
        executeSql(sql.toString());
    }

    @Override
    public synchronized long countTelemetryFrames() throws IOException {
        List<String> rows = executeSql("SELECT COUNT(*) FROM telemetry_audit;\n");
        if (rows.isEmpty()) {
            return 0L;
        }
        return Long.parseLong(rows.get(0).trim());
    }

    @Override
    public synchronized List<String> listDtcEvents() throws IOException {
        return executeSql(
                "SELECT telemetry_seq || '|' || dtc_code || '|' || severity "
                        + "FROM dtc_events ORDER BY id ASC;\n");
    }

    @Override
    public void close() {
        // Each SQL batch closes its sqlite3 process deterministically via try-with-resources.
    }

    private static String resolveDtcCode(TelemetryFrameDto frame) {
        if ((frame.faultMask() & 0x01) != 0) {
            return "DTC-0x202";
        }
        if ((frame.faultMask() & 0x02) != 0) {
            return "DTC-0x201";
        }
        return (frame.contactorState() == 0) ? "DTC-0x104" : "DTC-0x401";
    }

    private static void validateToken(String token) throws IOException {
        if (token == null || !SAFE_TOKEN.matcher(token).matches()) {
            throw new IOException("Rejected invalid SQL token");
        }
    }

    private List<String> executeSql(String sqlScript) throws IOException {
        ProcessBuilder pb = new ProcessBuilder("sqlite3", "-batch", dbPath.toString());
        pb.redirectErrorStream(false);
        Process proc = pb.start();

        List<String> outputLines = new ArrayList<>();
        String errorOutput;

        try (BufferedWriter writer = new BufferedWriter(
                new OutputStreamWriter(proc.getOutputStream(), StandardCharsets.UTF_8))) {
            writer.write(sqlScript);
            writer.flush();
        }

        try (BufferedReader stdout = new BufferedReader(
                        new InputStreamReader(proc.getInputStream(), StandardCharsets.UTF_8));
                BufferedReader stderr = new BufferedReader(
                        new InputStreamReader(proc.getErrorStream(), StandardCharsets.UTF_8))) {
            String line;
            while ((line = stdout.readLine()) != null) {
                if (!line.isEmpty()) {
                    outputLines.add(line);
                }
            }

            StringBuilder errBuf = new StringBuilder();
            while ((line = stderr.readLine()) != null) {
                errBuf.append(line).append('\n');
            }
            errorOutput = errBuf.toString();
        }

        try {
            int exitCode = proc.waitFor();
            if (exitCode != 0) {
                throw new IOException("sqlite3 exited with code " + exitCode + ": " + errorOutput);
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("Interrupted waiting for sqlite3", e);
        }

        return outputLines;
    }
}
