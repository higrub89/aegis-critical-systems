package com.aegis.gcs;

import com.aegis.gcs.dao.SqliteCliAuditDao;
import com.aegis.gcs.dao.TelemetryAuditDao;
import com.aegis.gcs.dto.TelemetryFrameDto;
import com.aegis.gcs.protocol.Crc32cCodec;
import com.aegis.gcs.service.GcsTelemetryService;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.List;
import java.util.concurrent.atomic.AtomicReference;

/**
 * Deterministic verification suite for the Java 21 Tactical GCS Console.
 */
public final class TacticalGcsTest {

    private TacticalGcsTest() {
    }

    private static void verifyCrc32cAndFraming() {
        byte[] vec = "123456789".getBytes(StandardCharsets.US_ASCII);
        int crc = Crc32cCodec.crc32c(vec, vec.length);
        if (crc != 0xE3069283) {
            throw new AssertionError(String.format("Expected CRC-32C 0xE3069283, got 0x%08X", crc));
        }

        TelemetryFrameDto dto = new TelemetryFrameDto(
                TelemetryFrameDto.MSG_TM_TELEMETRY, 11L, 405000L, -9200, (short) 310, 2, 0, 1000L, 0);
        byte[] wire = Crc32cCodec.encode(dto);
        TelemetryFrameDto decoded = Crc32cCodec.decodeAndVerify(wire);
        if (decoded.packVoltageMv() != 405000L || decoded.packCurrentMa() != -9200) {
            throw new AssertionError("Decoded telemetry mismatch");
        }

        wire[14] ^= 0x04;
        boolean rejected = false;
        try {
            Crc32cCodec.decodeAndVerify(wire);
        } catch (IllegalArgumentException expected) {
            rejected = true;
        }
        if (!rejected) {
            throw new AssertionError("Corrupted CRC-32C frame was not rejected");
        }
    }

    private static void verifyEndToEndGcsAndSqlitePersistence() throws IOException {
        Path dbPath = Path.of("build", "aegis_gcs_test.db");
        Path schemaPath = Path.of("04-tactical-gcs", "gcs-console", "sql", "schema.sql");
        Files.deleteIfExists(dbPath);

        AtomicReference<TelemetryFrameDto> receivedTc = new AtomicReference<>();

        try (ServerSocket server = new ServerSocket(0, 4, InetAddress.getLoopbackAddress());
                TelemetryAuditDao dao = new SqliteCliAuditDao(dbPath, schemaPath);
                GcsTelemetryService service = new GcsTelemetryService(dao)) {
            dao.initializeSchema();
            int port = server.getLocalPort();

            Thread simDaemon = Thread.ofVirtual().start(() -> {
                try (Socket client = server.accept();
                        OutputStream out = client.getOutputStream();
                        DataInputStream in = new DataInputStream(client.getInputStream())) {
                    TelemetryFrameDto nom = new TelemetryFrameDto(
                            TelemetryFrameDto.MSG_TM_TELEMETRY, 1L, 400000L, 45000, (short) 280, 2, 0, 10L, 0);
                    TelemetryFrameDto fault = new TelemetryFrameDto(
                            TelemetryFrameDto.MSG_TM_TELEMETRY, 2L, 398000L, 0, (short) 625, 0, 0x01, 20L, 0);
                    out.write(Crc32cCodec.encode(nom));
                    out.write(Crc32cCodec.encode(fault));
                    out.flush();

                    byte[] tcRaw = new byte[Crc32cCodec.PACKET_SIZE];
                    in.readFully(tcRaw);
                    receivedTc.set(Crc32cCodec.decodeAndVerify(tcRaw));
                } catch (IOException e) {
                    throw new RuntimeException(e);
                }
            });

            int ingested = service.ingestSession("127.0.0.1", port, 2);
            simDaemon.join();

            if (ingested != 2) {
                throw new AssertionError("Expected 2 ingested frames, got " + ingested);
            }
            if (dao.countTelemetryFrames() != 2L) {
                throw new AssertionError("Expected 2 rows in telemetry_audit");
            }
            List<String> dtcs = dao.listDtcEvents();
            if (dtcs.size() != 1 || !dtcs.get(0).contains("DTC-0x202")) {
                throw new AssertionError("Expected 1 DTC-0x202 audit event, got " + dtcs);
            }
            TelemetryFrameDto tc = receivedTc.get();
            if (tc == null || tc.msgType() != TelemetryFrameDto.MSG_TC_SAFE_STATE) {
                throw new AssertionError("Expected TC_SAFE_STATE command dispatch to daemon");
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("Test interrupted", e);
        } finally {
            Files.deleteIfExists(dbPath);
        }
    }

    /**
     * Runs all Java 21 Tactical GCS verification checks.
     *
     * @param args unused
     * @throws IOException if persistence or socket setup fails
     */
    public static void main(String[] args) throws IOException {
        verifyCrc32cAndFraming();
        verifyEndToEndGcsAndSqlitePersistence();
        System.out.println("[PASS] Java 21 Tactical GCS (DAO/DTO, CRC-32C, ExecutorService & SQLite) verified.");
    }
}
