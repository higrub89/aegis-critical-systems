package com.aegis.gcs.service;

import com.aegis.gcs.dao.TelemetryAuditDao;
import com.aegis.gcs.dto.TelemetryFrameDto;
import com.aegis.gcs.protocol.Crc32cCodec;
import java.io.DataInputStream;
import java.io.EOFException;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.util.concurrent.Callable;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;

/**
 * Service layer orchestrating non-blocking TCP TM/TC streaming, CRC-32C validation,
 * and SQL audit persistence using Java 21 {@link ExecutorService} and {@code try-with-resources}.
 */
public final class GcsTelemetryService implements AutoCloseable {

    private final TelemetryAuditDao auditDao;
    private final ExecutorService executor;

    /**
     * Creates a GCS telemetry service backed by {@code auditDao}.
     *
     * @param auditDao persistence DAO implementation
     */
    public GcsTelemetryService(TelemetryAuditDao auditDao) {
        this.auditDao = auditDao;
        this.executor = Executors.newVirtualThreadPerTaskExecutor();
    }

    /**
     * Connects to the C17 epoll TM/TC daemon, ingests up to {@code maxFrames} 32-byte packets,
     * verifies CRC-32C, persists telemetry/DTCs, and issues {@code TC_SAFE_STATE} if a hazard appears.
     *
     * @param host      TM/TC daemon host
     * @param port      TM/TC daemon TCP port
     * @param maxFrames maximum number of TM frames to ingest
     * @return count of verified and persisted frames
     * @throws IOException if socket or persistence operations fail
     */
    // @req SRS-GCS-003
    // @fault DTC-0x403
    // @hazard MIL-882E-CAT-III
    public int ingestSession(String host, int port, int maxFrames) throws IOException {
        Callable<Integer> worker = () -> {
            int ingested = 0;
            try (Socket socket = new Socket()) {
                socket.connect(new InetSocketAddress(host, port), 2000);
                socket.setSoTimeout(2000);

                DataInputStream in = new DataInputStream(socket.getInputStream());
                OutputStream out = socket.getOutputStream();
                byte[] wireBuf = new byte[Crc32cCodec.PACKET_SIZE];

                while (ingested < maxFrames) {
                    try {
                        in.readFully(wireBuf);
                    } catch (EOFException eof) {
                        break;
                    }
                    TelemetryFrameDto frame = Crc32cCodec.decodeAndVerify(wireBuf);
                    auditDao.saveFrameAndAudit(frame);
                    ingested++;

                    if (frame.hasActiveHazard()) {
                        TelemetryFrameDto safeCmd = new TelemetryFrameDto(
                                TelemetryFrameDto.MSG_TC_SAFE_STATE,
                                frame.seq() + 1L,
                                0L,
                                0,
                                (short) 0,
                                0,
                                frame.faultMask(),
                                frame.timestampMs(),
                                0);
                        out.write(Crc32cCodec.encode(safeCmd));
                        out.flush();
                    }
                }
            }
            return ingested;
        };

        Future<Integer> future = executor.submit(worker);
        try {
            return future.get();
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            throw new IOException("GCS telemetry ingestion interrupted", e);
        } catch (ExecutionException e) {
            Throwable cause = e.getCause();
            if (cause instanceof IOException ioEx) {
                throw ioEx;
            }
            throw new IOException("GCS telemetry ingestion failed: " + cause.getMessage(), cause);
        }
    }

    @Override
    public void close() {
        executor.shutdownNow();
    }
}
