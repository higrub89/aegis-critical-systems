/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 04: Tactical Ground Control Station
 * File: 04-tactical-gcs/tmtc-daemon/src/aegis_tmtc_epoll.c
 * Description: Non-Blocking POSIX epoll TM/TC Multiplexer & Packet Framing
 * Standard: ISO/IEC 9899:2018 (C17) | POSIX.1-2017 | MISRA C:2012
 * ========================================================================== */

#define _POSIX_C_SOURCE 200809L
#include "aegis_tmtc.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static void write_u16_le(uint8_t *dst, uint16_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
}

static void write_u32_le(uint8_t *dst, uint32_t val)
{
    dst[0] = (uint8_t)(val & 0xFFU);
    dst[1] = (uint8_t)((val >> 8U) & 0xFFU);
    dst[2] = (uint8_t)((val >> 16U) & 0xFFU);
    dst[3] = (uint8_t)((val >> 24U) & 0xFFU);
}

static uint16_t read_u16_le(const uint8_t *src)
{
    return (uint16_t)(((uint16_t)src[0]) | ((uint16_t)src[1] << 8U));
}

static uint32_t read_u32_le(const uint8_t *src)
{
    return ((uint32_t)src[0]) | ((uint32_t)src[1] << 8U)
           | ((uint32_t)src[2] << 16U) | ((uint32_t)src[3] << 24U);
}

static int set_nonblocking(int fd)
{
    int flags;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return (-1);
    return (fcntl(fd, F_SETFL, flags | O_NONBLOCK));
}

int aegis_tmtc_encode(uint8_t                       out_buf[AEGIS_TMTC_PKT_SIZE],
                      uint8_t                       msg_type,
                      uint32_t                      seq,
                      uint32_t                      timestamp_ms,
                      const t_aegis_telemetry_data *tel)
{
    uint32_t crc;

    if (out_buf == NULL || tel == NULL)
        return (EINVAL);

    (void)memset(out_buf, 0, AEGIS_TMTC_PKT_SIZE);
    write_u32_le(&out_buf[0], AEGIS_TMTC_MAGIC);
    out_buf[4] = AEGIS_TMTC_VERSION;
    out_buf[5] = msg_type;
    write_u16_le(&out_buf[6], 0U);
    write_u32_le(&out_buf[8], seq);
    write_u32_le(&out_buf[12], tel->pack_voltage_mv);
    write_u32_le(&out_buf[16], (uint32_t)tel->pack_current_ma);
    write_u16_le(&out_buf[20], (uint16_t)tel->max_cell_temp_dc);
    out_buf[22] = tel->contactor_state;
    out_buf[23] = tel->fault_mask;
    write_u32_le(&out_buf[24], timestamp_ms);

    crc = aegis_crc32c(out_buf, AEGIS_TMTC_PKT_SIZE - 4U);
    write_u32_le(&out_buf[28], crc);
    return (0);
}

int aegis_tmtc_decode_verify(const uint8_t        in_buf[AEGIS_TMTC_PKT_SIZE],
                             t_aegis_tmtc_packet *out_pkt)
{
    uint32_t expected_crc;
    uint32_t actual_crc;
    uint32_t magic;

    if (in_buf == NULL || out_pkt == NULL)
        return (EINVAL);

    magic = read_u32_le(&in_buf[0]);
    if (magic != AEGIS_TMTC_MAGIC || in_buf[4] != AEGIS_TMTC_VERSION)
        return (EPROTO);

    expected_crc = aegis_crc32c(in_buf, AEGIS_TMTC_PKT_SIZE - 4U);
    actual_crc = read_u32_le(&in_buf[28]);
    if (expected_crc != actual_crc)
        return (EBADMSG);

    out_pkt->magic = magic;
    out_pkt->version = in_buf[4];
    out_pkt->msg_type = in_buf[5];
    out_pkt->reserved = read_u16_le(&in_buf[6]);
    out_pkt->seq = read_u32_le(&in_buf[8]);
    out_pkt->telemetry.pack_voltage_mv = read_u32_le(&in_buf[12]);
    out_pkt->telemetry.pack_current_ma = (int32_t)read_u32_le(&in_buf[16]);
    out_pkt->telemetry.max_cell_temp_dc = (int16_t)read_u16_le(&in_buf[20]);
    out_pkt->telemetry.contactor_state = in_buf[22];
    out_pkt->telemetry.fault_mask = in_buf[23];
    out_pkt->timestamp_ms = read_u32_le(&in_buf[24]);
    out_pkt->crc32c = actual_crc;
    return (0);
}

int aegis_tmtc_server_open_tcp(uint16_t port, uint16_t *out_bound_port)
{
    struct sockaddr_in addr;
    socklen_t          addr_len;
    int                fd;
    int                opt;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return (-errno);

    opt = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt)) < 0)
    {
        int err = errno;
        (void)close(fd);
        return (-err);
    }

    (void)memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        int err = errno;
        (void)close(fd);
        return (-err);
    }

    if (listen(fd, 8) < 0)
    {
        int err = errno;
        (void)close(fd);
        return (-err);
    }

    if (set_nonblocking(fd) < 0)
    {
        int err = errno;
        (void)close(fd);
        return (-err);
    }

    if (out_bound_port != NULL)
    {
        addr_len = sizeof(addr);
        if (getsockname(fd, (struct sockaddr *)&addr, &addr_len) < 0)
        {
            int err = errno;
            (void)close(fd);
            return (-err);
        }
        *out_bound_port = ntohs(addr.sin_port);
    }

    return (fd);
}

static void register_client_slot(int                   epoll_fd,
                                 int                   client_fd,
                                 int                  *client_fds,
                                 t_aegis_tmtc_rx_slot *rx_slots,
                                 size_t                max_clients)
{
    struct epoll_event ev;
    size_t             i;

    if (set_nonblocking(client_fd) < 0)
    {
        (void)close(client_fd);
        return;
    }
    for (i = 0U; i < max_clients; ++i)
    {
        if (client_fds[i] < 0)
        {
            (void)memset(&ev, 0, sizeof(ev));
            ev.events = EPOLLIN | EPOLLRDHUP;
            ev.data.fd = client_fd;
            if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &ev) == 0)
            {
                client_fds[i] = client_fd;
                if (rx_slots != NULL)
                    rx_slots[i].len = 0U;
                return;
            }
            break;
        }
    }
    (void)close(client_fd);
}

static void remove_client_slot(int                   epoll_fd,
                               int                   client_fd,
                               int                  *client_fds,
                               t_aegis_tmtc_rx_slot *rx_slots,
                               size_t                max_clients)
{
    size_t i;

    (void)epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client_fd, NULL);
    (void)close(client_fd);
    for (i = 0U; i < max_clients; ++i)
    {
        if (client_fds[i] == client_fd)
        {
            client_fds[i] = -1;
            if (rx_slots != NULL)
                rx_slots[i].len = 0U;
            break;
        }
    }
}

static size_t find_client_slot(const int *client_fds, size_t max_clients, int fd)
{
    size_t i;

    for (i = 0U; i < max_clients; ++i)
    {
        if (client_fds[i] == fd)
            return (i);
    }
    return (max_clients);
}

static int send_packet_nonblock(int fd, const uint8_t *buf, size_t len)
{
    size_t   sent_total;
    uint32_t retries;

    sent_total = 0U;
    retries = 0U;
    while (sent_total < len && retries < 8U)
    {
        ssize_t n = send(fd, buf + sent_total, len - sent_total, MSG_NOSIGNAL);
        if (n > 0)
        {
            sent_total += (size_t)n;
        }
        else if (n < 0 && errno == EINTR)
        {
            retries += 1U;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            if (sent_total == 0U)
                return (0);
            retries += 1U;
        }
        else
        {
            return (-1);
        }
    }
    return (sent_total == len ? 0 : -1);
}

static void process_client_rx(int                   epoll_fd,
                              int                   fd,
                              int                  *client_fds,
                              t_aegis_tmtc_rx_slot *rx_slots,
                              size_t                max_clients,
                              t_aegis_tmtc_shm     *shm,
                              uint32_t             *out_tc_received,
                              uint8_t              *out_last_tc_type,
                              uint32_t             *out_crc_errors)
{
    size_t slot_idx;

    slot_idx = find_client_slot(client_fds, max_clients, fd);
    if (slot_idx >= max_clients || rx_slots == NULL)
    {
        remove_client_slot(epoll_fd, fd, client_fds, rx_slots, max_clients);
        return;
    }

    for (;;)
    {
        size_t  need = AEGIS_TMTC_PKT_SIZE - rx_slots[slot_idx].len;
        ssize_t nread = recv(fd, rx_slots[slot_idx].buf + rx_slots[slot_idx].len, need, 0);

        if (nread == 0)
        {
            remove_client_slot(epoll_fd, fd, client_fds, rx_slots, max_clients);
            return;
        }
        if (nread < 0)
        {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return;
            remove_client_slot(epoll_fd, fd, client_fds, rx_slots, max_clients);
            return;
        }

        rx_slots[slot_idx].len += (size_t)nread;
        if (rx_slots[slot_idx].len == AEGIS_TMTC_PKT_SIZE)
        {
            t_aegis_tmtc_packet tc_pkt;
            if (aegis_tmtc_decode_verify(rx_slots[slot_idx].buf, &tc_pkt) == 0)
            {
                if (out_tc_received != NULL)
                    *out_tc_received += 1U;
                if (out_last_tc_type != NULL)
                    *out_last_tc_type = tc_pkt.msg_type;
                if (tc_pkt.msg_type == (uint8_t)AEGIS_MSG_TC_SAFE_STATE)
                    __atomic_store_n(&shm->tc_safe_state_req, 1U, __ATOMIC_RELEASE);
            }
            else if (out_crc_errors != NULL)
            {
                *out_crc_errors += 1U;
            }
            rx_slots[slot_idx].len = 0U;
        }
    }
}

int aegis_tmtc_epoll_poll_ex(int                   epoll_fd,
                             int                   listen_fd,
                             int                  *client_fds,
                             t_aegis_tmtc_rx_slot *rx_slots,
                             size_t                max_clients,
                             t_aegis_tmtc_shm     *shm,
                             uint32_t             *seq_counter,
                             uint32_t             *out_tc_received,
                             uint8_t              *out_last_tc_type,
                             uint32_t             *out_crc_errors,
                             int                   timeout_ms)
{
    struct epoll_event     events[AEGIS_TMTC_MAX_CLIENTS];
    uint8_t                pkt_buf[AEGIS_TMTC_PKT_SIZE];
    t_aegis_telemetry_data tel_snap;
    uint64_t               ts_snap;
    int                    nfds;
    int                    idx;
    size_t                 c_idx;

    if (epoll_fd < 0 || listen_fd < 0 || client_fds == NULL || rx_slots == NULL || shm == NULL
        || seq_counter == NULL || max_clients == 0U || max_clients > AEGIS_TMTC_MAX_CLIENTS)
    {
        return (EINVAL);
    }

    nfds = epoll_wait(epoll_fd, events, (int)AEGIS_TMTC_MAX_CLIENTS, timeout_ms);
    if (nfds < 0)
    {
        if (errno == EINTR)
            return (0);
        return (errno);
    }

    for (idx = 0; idx < nfds; ++idx)
    {
        int fd = events[idx].data.fd;

        if (fd == listen_fd)
        {
            for (;;)
            {
                int cfd = accept(listen_fd, NULL, NULL);
                if (cfd < 0)
                    break;
                register_client_slot(epoll_fd, cfd, client_fds, rx_slots, max_clients);
            }
        }
        else if ((events[idx].events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0U)
        {
            remove_client_slot(epoll_fd, fd, client_fds, rx_slots, max_clients);
        }
        else if ((events[idx].events & EPOLLIN) != 0U)
        {
            process_client_rx(epoll_fd,
                              fd,
                              client_fds,
                              rx_slots,
                              max_clients,
                              shm,
                              out_tc_received,
                              out_last_tc_type,
                              out_crc_errors);
        }
    }

    /* Read tear-free SHM snapshot via seqlock and broadcast to connected GCS clients */
    (void)memset(&tel_snap, 0, sizeof(tel_snap));
    ts_snap = 0ULL;
    if (aegis_shm_read_snapshot(shm, &tel_snap, NULL, NULL, &ts_snap) != 0)
        return (0);

    *seq_counter += 1U;
    if (aegis_tmtc_encode(pkt_buf,
                          (uint8_t)AEGIS_MSG_TM_TELEMETRY,
                          *seq_counter,
                          (uint32_t)(ts_snap / 1000000ULL),
                          &tel_snap)
        == 0)
    {
        for (c_idx = 0U; c_idx < max_clients; ++c_idx)
        {
            if (client_fds[c_idx] >= 0)
            {
                if (send_packet_nonblock(client_fds[c_idx], pkt_buf, AEGIS_TMTC_PKT_SIZE) != 0)
                {
                    remove_client_slot(
                        epoll_fd, client_fds[c_idx], client_fds, rx_slots, max_clients);
                }
            }
        }
    }

    return (0);
}

/*
 * @req SRS-GCS-002
 * @fault DTC-0x402
 * @hazard MIL-882E-CAT-III
 */
int aegis_tmtc_epoll_poll_once(int               epoll_fd,
                               int               listen_fd,
                               int              *client_fds,
                               size_t            max_clients,
                               t_aegis_tmtc_shm *shm,
                               uint32_t         *seq_counter,
                               uint32_t         *out_tc_received,
                               uint8_t          *out_last_tc_type,
                               int               timeout_ms)
{
    t_aegis_tmtc_rx_slot local_slots[AEGIS_TMTC_MAX_CLIENTS];

    (void)memset(local_slots, 0, sizeof(local_slots));
    return (aegis_tmtc_epoll_poll_ex(epoll_fd,
                                     listen_fd,
                                     client_fds,
                                     local_slots,
                                     max_clients,
                                     shm,
                                     seq_counter,
                                     out_tc_received,
                                     out_last_tc_type,
                                     NULL,
                                     timeout_ms));
}
