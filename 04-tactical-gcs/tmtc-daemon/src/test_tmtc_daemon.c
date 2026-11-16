/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 04: Tactical Ground Control Station
 * File: 04-tactical-gcs/tmtc-daemon/src/test_tmtc_daemon.c
 * Description: Deterministic Verification for Hardware CRC-32C, 32B TM/TC
 *              Framing, POSIX SHM Bridge & Non-Blocking epoll TCP Loopback.
 * Standard: ISO/IEC 9899:2018 (C17) | POSIX.1-2017
 * ========================================================================== */

#define _POSIX_C_SOURCE 200809L
#include "aegis_tmtc.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static int test_crc32c_standard_vector(void)
{
    const char *vec = "123456789";
    uint32_t    crc = aegis_crc32c(vec, 9U);

    if (crc != 0xE3069283U)
    {
        (void)fprintf(stderr, "Expected 0xE3069283, got 0x%08X\n", crc);
        return (1);
    }
    return (0);
}

static int test_packet_framing_and_corruption(void)
{
    uint8_t                buf[AEGIS_TMTC_PKT_SIZE];
    t_aegis_telemetry_data tel;
    t_aegis_tmtc_packet    pkt;

    tel.pack_voltage_mv = 415200U;
    tel.pack_current_ma = -12400;
    tel.max_cell_temp_dc = 312;
    tel.contactor_state = 2U;
    tel.fault_mask = 0x01U;

    if (aegis_tmtc_encode(buf, (uint8_t)AEGIS_MSG_TM_TELEMETRY, 99U, 12345U, &tel) != 0)
        return (1);

    (void)memset(&pkt, 0, sizeof(pkt));
    if (aegis_tmtc_decode_verify(buf, &pkt) != 0)
        return (2);

    if (pkt.seq != 99U || pkt.telemetry.pack_voltage_mv != 415200U
        || pkt.telemetry.pack_current_ma != -12400 || pkt.telemetry.max_cell_temp_dc != 312)
    {
        return (3);
    }

    /* Flip 1 bit in payload -> CRC-32C must reject with EBADMSG */
    buf[14] ^= 0x08U;
    if (aegis_tmtc_decode_verify(buf, &pkt) != EBADMSG)
        return (4);

    return (0);
}

static int test_epoll_shm_tcp_loopback(void)
{
    char               shm_name[64];
    int                shm_fd;
    t_aegis_tmtc_shm  *shm_ptr;
    uint16_t           bound_port;
    int                listen_fd;
    int                epoll_fd;
    int                client_sock;
    int                client_fds[AEGIS_TMTC_MAX_CLIENTS];
    struct epoll_event ev;
    struct sockaddr_in dst;
    uint32_t           seq;
    uint32_t           tc_count;
    uint8_t            last_tc;
    uint8_t            rx_buf[AEGIS_TMTC_PKT_SIZE];
    uint8_t            tc_buf[AEGIS_TMTC_PKT_SIZE];
    t_aegis_tmtc_packet rx_pkt;
    t_aegis_telemetry_data tc_payload;
    ssize_t            n;
    size_t             i;

    (void)snprintf(shm_name, sizeof(shm_name), "/aegis_tmtc_test_%ld", (long)getpid());
    shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (shm_fd < 0 || ftruncate(shm_fd, (off_t)sizeof(t_aegis_tmtc_shm)) != 0)
        return (1);

    shm_ptr = (t_aegis_tmtc_shm *)mmap(
        NULL, sizeof(t_aegis_tmtc_shm), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    (void)close(shm_fd);
    if (shm_ptr == MAP_FAILED)
    {
        (void)shm_unlink(shm_name);
        return (2);
    }

    (void)memset(shm_ptr, 0, sizeof(t_aegis_tmtc_shm));
    shm_ptr->telemetry.pack_voltage_mv = 399500U;
    shm_ptr->telemetry.pack_current_ma = 48000;
    shm_ptr->telemetry.max_cell_temp_dc = 285;
    shm_ptr->telemetry.contactor_state = 2U;
    shm_ptr->timestamp_ns = 500000000ULL;

    bound_port = 0U;
    listen_fd = aegis_tmtc_server_open_tcp(0U, &bound_port);
    epoll_fd = epoll_create1(0);
    if (listen_fd < 0 || epoll_fd < 0)
        return (3);

    (void)memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) != 0)
        return (4);

    for (i = 0U; i < AEGIS_TMTC_MAX_CLIENTS; ++i)
        client_fds[i] = -1;

    /* Connect TCP client to loopback ephemeral port */
    client_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (client_sock < 0)
        return (5);
    (void)memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst.sin_port = htons(bound_port);
    if (connect(client_sock, (struct sockaddr *)&dst, sizeof(dst)) != 0)
        return (6);

    seq = 0U;
    tc_count = 0U;
    last_tc = 0U;

    /* Poll 1: accepts client and broadcasts TM packet from SHM */
    if (aegis_tmtc_epoll_poll_once(epoll_fd,
                                   listen_fd,
                                   client_fds,
                                   AEGIS_TMTC_MAX_CLIENTS,
                                   shm_ptr,
                                   &seq,
                                   &tc_count,
                                   &last_tc,
                                   10)
        != 0)
        return (7);

    n = recv(client_sock, rx_buf, AEGIS_TMTC_PKT_SIZE, MSG_WAITALL);
    if (n != (ssize_t)AEGIS_TMTC_PKT_SIZE)
        return (8);
    if (aegis_tmtc_decode_verify(rx_buf, &rx_pkt) != 0
        || rx_pkt.telemetry.pack_voltage_mv != 399500U)
        return (9);

    /* Send TC_SAFE_STATE from GCS client back to epoll daemon */
    (void)memset(&tc_payload, 0, sizeof(tc_payload));
    if (aegis_tmtc_encode(tc_buf, (uint8_t)AEGIS_MSG_TC_SAFE_STATE, 1U, 501U, &tc_payload) != 0)
        return (10);
    if (send(client_sock, tc_buf, AEGIS_TMTC_PKT_SIZE, 0) != (ssize_t)AEGIS_TMTC_PKT_SIZE)
        return (11);

    /* Poll 2: daemon receives and verifies CRC-32C of TC packet */
    if (aegis_tmtc_epoll_poll_once(epoll_fd,
                                   listen_fd,
                                   client_fds,
                                   AEGIS_TMTC_MAX_CLIENTS,
                                   shm_ptr,
                                   &seq,
                                   &tc_count,
                                   &last_tc,
                                   10)
        != 0)
        return (12);

    uint32_t tc_req = __atomic_load_n(&shm_ptr->tc_safe_state_req, __ATOMIC_ACQUIRE);

    (void)close(client_sock);
    for (i = 0U; i < AEGIS_TMTC_MAX_CLIENTS; ++i)
    {
        if (client_fds[i] >= 0)
            (void)close(client_fds[i]);
    }
    (void)close(epoll_fd);
    (void)close(listen_fd);
    (void)munmap(shm_ptr, sizeof(t_aegis_tmtc_shm));
    (void)shm_unlink(shm_name);

    if (tc_count != 1U || last_tc != (uint8_t)AEGIS_MSG_TC_SAFE_STATE || tc_req != 1U)
        return (13);
    return (0);
}

int main(void)
{
    int rc;

    rc = test_crc32c_standard_vector();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_crc32c_standard_vector: %d\n", rc);
        return (1);
    }
    rc = test_packet_framing_and_corruption();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_packet_framing_and_corruption: %d\n", rc);
        return (1);
    }
    rc = test_epoll_shm_tcp_loopback();
    if (rc != 0)
    {
        (void)fprintf(stderr, "[FAIL] test_epoll_shm_tcp_loopback: %d\n", rc);
        return (1);
    }
    (void)printf("[PASS] Hardware CRC-32C, 32B TM/TC Framing & epoll SHM Bridge verified.\n");
    return (0);
}
