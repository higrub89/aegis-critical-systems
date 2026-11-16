package com.aegis.gcs.protocol;

import com.aegis.gcs.dto.TelemetryFrameDto;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

/**
 * Binary 32-byte TM/TC codec and Castagnoli CRC-32C verifier matching {@code aegis_tmtc.h}.
 */
public final class Crc32cCodec {

    /** Expected 32-bit magic header ({@code 0xAE615001}). */
    public static final int MAGIC = 0xAE615001;
    /** Protocol version byte. */
    public static final byte VERSION = 1;
    /** Fixed wire packet size in bytes. */
    public static final int PACKET_SIZE = 32;
    private static final int CRC32C_POLY_REFLECTED = 0x82F63B78;

    private Crc32cCodec() {
    }

    /**
     * Computes deterministic Castagnoli CRC-32C over {@code data[0..len)}.
     *
     * @param data input byte array
     * @param len  number of bytes to checksum
     * @return 32-bit CRC-32C value
     */
    public static int crc32c(byte[] data, int len) {
        if (data == null || len <= 0 || len > data.length) {
            return 0;
        }
        int crc = 0xFFFFFFFF;
        for (int i = 0; i < len; i++) {
            crc ^= (data[i] & 0xFF);
            for (int bit = 0; bit < 8; bit++) {
                if ((crc & 1) != 0) {
                    crc = (crc >>> 1) ^ CRC32C_POLY_REFLECTED;
                } else {
                    crc >>>= 1;
                }
            }
        }
        return ~crc;
    }

    /**
     * Encodes a {@link TelemetryFrameDto} into a 32-byte little-endian TM/TC wire frame.
     *
     * @param dto frame payload to encode
     * @return 32-byte wire frame with appended CRC-32C
     */
    public static byte[] encode(TelemetryFrameDto dto) {
        ByteBuffer buf = ByteBuffer.allocate(PACKET_SIZE).order(ByteOrder.LITTLE_ENDIAN);
        buf.putInt(MAGIC);
        buf.put(VERSION);
        buf.put((byte) dto.msgType());
        buf.putShort((short) 0);
        buf.putInt((int) dto.seq());
        buf.putInt((int) dto.packVoltageMv());
        buf.putInt(dto.packCurrentMa());
        buf.putShort(dto.maxCellTempDc());
        buf.put((byte) dto.contactorState());
        buf.put((byte) dto.faultMask());
        buf.putInt((int) dto.timestampMs());

        byte[] raw = buf.array();
        int crc = crc32c(raw, PACKET_SIZE - 4);
        buf.putInt(28, crc);
        return raw;
    }

    /**
     * Decodes and verifies a 32-byte wire frame against magic, version, and Castagnoli CRC-32C.
     *
     * @param raw 32-byte packet buffer
     * @return verified {@link TelemetryFrameDto}
     * @throws IllegalArgumentException if framing or CRC-32C validation fails
     */
    public static TelemetryFrameDto decodeAndVerify(byte[] raw) {
        if (raw == null || raw.length != PACKET_SIZE) {
            throw new IllegalArgumentException("Invalid TM/TC packet length");
        }
        ByteBuffer buf = ByteBuffer.wrap(raw).order(ByteOrder.LITTLE_ENDIAN);
        int magic = buf.getInt(0);
        byte version = buf.get(4);
        if (magic != MAGIC || version != VERSION) {
            throw new IllegalArgumentException("Invalid TM/TC magic or version");
        }
        int expectedCrc = crc32c(raw, PACKET_SIZE - 4);
        int actualCrc = buf.getInt(28);
        if (expectedCrc != actualCrc) {
            throw new IllegalArgumentException(
                    String.format("CRC-32C mismatch: expected=0x%08X actual=0x%08X", expectedCrc, actualCrc));
        }

        int msgType = buf.get(5) & 0xFF;
        long seq = Integer.toUnsignedLong(buf.getInt(8));
        long voltageMv = Integer.toUnsignedLong(buf.getInt(12));
        int currentMa = buf.getInt(16);
        short tempDc = buf.getShort(20);
        int contactor = buf.get(22) & 0xFF;
        int faultMask = buf.get(23) & 0xFF;
        long timestampMs = Integer.toUnsignedLong(buf.getInt(24));

        return new TelemetryFrameDto(
                msgType, seq, voltageMv, currentMa, tempDc, contactor, faultMask, timestampMs, actualCrc);
    }
}
