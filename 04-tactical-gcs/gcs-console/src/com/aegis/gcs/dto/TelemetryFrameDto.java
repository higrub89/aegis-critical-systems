package com.aegis.gcs.dto;

/**
 * Immutable Data Transfer Object representing a verified 32-byte AEGIS TM/TC frame.
 * Maps 1:1 to {@code t_aegis_tmtc_packet} from the C17 epoll TM/TC daemon.
 */
public record TelemetryFrameDto(
        int msgType,
        long seq,
        long packVoltageMv,
        int packCurrentMa,
        short maxCellTempDc,
        int contactorState,
        int faultMask,
        long timestampMs,
        int crc32c
) {
    /** TM Telemetry packet type ({@code AEGIS_MSG_TM_TELEMETRY}). */
    public static final int MSG_TM_TELEMETRY = 1;
    /** TC Safe-State command packet type ({@code AEGIS_MSG_TC_SAFE_STATE}). */
    public static final int MSG_TC_SAFE_STATE = 2;
    /** TC Reset command packet type ({@code AEGIS_MSG_TC_RESET}). */
    public static final int MSG_TC_RESET = 3;

    /**
     * Returns true when an active fault bit or emergency contactor opening is reported.
     *
     * @return true if fault mask is non-zero or contactor is open (state 0)
     */
    public boolean hasActiveHazard() {
        return faultMask != 0 || contactorState == 0;
    }
}
