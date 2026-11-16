package com.aegis.gcs;

import com.aegis.gcs.dao.SqliteCliAuditDao;
import com.aegis.gcs.dao.TelemetryAuditDao;
import com.aegis.gcs.service.GcsTelemetryService;
import java.io.IOException;
import java.nio.file.Path;
import java.util.List;

/**
 * CLI Entry point for the AEGIS Tactical Ground Control Station (Java 21 LTS).
 */
public final class TacticalGcsApp {

    private TacticalGcsApp() {
    }

    /**
     * Launches the GCS console client against a running C17 {@code aegis_tmtc_daemon}.
     *
     * @param args optional flags: {@code --host}, {@code --port}, {@code --db}, {@code --count}
     */
    public static void main(String[] args) {
        String host = "127.0.0.1";
        int port = 9100;
        Path dbPath = Path.of("reports", "aegis_gcs_audit.db");
        Path schemaPath = Path.of("04-tactical-gcs", "gcs-console", "sql", "schema.sql");
        int count = 10;

        for (int i = 0; i < args.length; i++) {
            if ("--host".equals(args[i]) && i + 1 < args.length) {
                host = args[++i];
            } else if ("--port".equals(args[i]) && i + 1 < args.length) {
                port = Integer.parseInt(args[++i]);
            } else if ("--db".equals(args[i]) && i + 1 < args.length) {
                dbPath = Path.of(args[++i]);
            } else if ("--count".equals(args[i]) && i + 1 < args.length) {
                count = Integer.parseInt(args[++i]);
            }
        }

        try (TelemetryAuditDao dao = new SqliteCliAuditDao(dbPath, schemaPath);
                GcsTelemetryService service = new GcsTelemetryService(dao)) {
            dao.initializeSchema();
            int ingested = service.ingestSession(host, port, count);
            List<String> dtcs = dao.listDtcEvents();
            System.out.printf(
                    "[AEGIS-GCS] Ingested=%d frames | TotalDB=%d | DTCs=%d%n",
                    ingested, dao.countTelemetryFrames(), dtcs.size());
        } catch (IOException e) {
            System.err.println("[AEGIS-GCS] Operational error: " + e.getMessage());
            System.exit(1);
        }
    }
}
