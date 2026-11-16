/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 04: Tactical Ground Control Station
 * File: 04-tactical-gcs/tmtc-daemon/include/aegis_tmtc.h
 * Description: Binary 32-Byte TM/TC Framing, Hardware CRC-32C & POSIX epoll
 * Standard: ISO/IEC 9899:2018 (C17) | POSIX.1-2017 | MISRA C:2012
 * ========================================================================== */

#ifndef AEGIS_TMTC_H
#define AEGIS_TMTC_H

#include "aegis_frame_abi.h"
#include "aegis_shm_bus.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AEGIS_TMTC_MAGIC        0xAE615001U
#define AEGIS_TMTC_VERSION      1U
#define AEGIS_TMTC_PKT_SIZE     32U
#define AEGIS_TMTC_MAX_CLIENTS  16U
#define AEGIS_DTC_TMTC_CRC      0x0401U
#define AEGIS_DTC_TMTC_SHM_FAIL 0x0402U

typedef enum e_aegis_tmtc_msg_type
{
    AEGIS_MSG_TM_TELEMETRY = 1U,
    AEGIS_MSG_TC_SAFE_STATE = 2U,
    AEGIS_MSG_TC_RESET = 3U,
    AEGIS_MSG_TM_ACK = 4U
} t_aegis_tmtc_msg_type;

typedef struct s_aegis_tmtc_packet
{
    uint32_t               magic;
    uint8_t                version;
    uint8_t                msg_type;
    uint16_t               reserved;
    uint32_t               seq;
    t_aegis_telemetry_data telemetry;
    uint32_t               timestamp_ms;
    uint32_t               crc32c;
} t_aegis_tmtc_packet;

AEGIS_STATIC_ASSERT(sizeof(t_aegis_tmtc_packet) == AEGIS_TMTC_PKT_SIZE,
                    "TM/TC wire packet must be exactly 32 bytes");

uint32_t aegis_crc32c(const void *data, size_t len);
int      aegis_tmtc_encode(uint8_t                       out_buf[AEGIS_TMTC_PKT_SIZE],
                           uint8_t                       msg_type,
                           uint32_t                      seq,
                           uint32_t                      timestamp_ms,
                           const t_aegis_telemetry_data *tel);
int      aegis_tmtc_decode_verify(const uint8_t        in_buf[AEGIS_TMTC_PKT_SIZE],
                                  t_aegis_tmtc_packet *out_pkt);
int      aegis_tmtc_server_open_tcp(uint16_t port, uint16_t *out_bound_port);
int      aegis_tmtc_epoll_poll_once(int               epoll_fd,
                                    int               listen_fd,
                                    int              *client_fds,
                                    size_t            max_clients,
                                    t_aegis_tmtc_shm *shm,
                                    uint32_t         *seq_counter,
                                    uint32_t         *out_tc_received,
                                    uint8_t          *out_last_tc_type,
                                    int               timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* AEGIS_TMTC_H */
