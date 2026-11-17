/* ========================================================================== *
 * AEGIS Critical Systems - Subsystem 04: Tactical Ground Control Station
 * File: 04-tactical-gcs/tmtc-daemon/src/main_tmtc_daemon.c
 * Description: Standalone C17 epoll TM/TC Daemon bridging POSIX SHM to GCS
 * Standard: ISO/IEC 9899:2018 (C17) | POSIX.1-2017
 * ========================================================================== */

#define _POSIX_C_SOURCE 200809L
#include "aegis_tmtc.h"
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static volatile sig_atomic_t g_tmtc_stop = 0;

static void handle_sig(int signum)
{
    (void)signum;
    g_tmtc_stop = 1;
}

static int parse_u16_port(const char *arg, uint16_t *out)
{
    char         *endptr;
    unsigned long val;

    if (arg == NULL || out == NULL || arg[0] == '\0')
        return (-1);
    val = strtoul(arg, &endptr, 10);
    if (*endptr != '\0' || val > 65535UL)
        return (-1);
    *out = (uint16_t)val;
    return (0);
}

int main(int argc, char **argv)
{
    const char          *shm_name;
    uint16_t             port;
    int                  shm_fd;
    int                  owns_shm;
    t_aegis_tmtc_shm    *shm_ptr;
    int                  listen_fd;
    int                  epoll_fd;
    struct epoll_event   ev;
    int                  client_fds[AEGIS_TMTC_MAX_CLIENTS];
    t_aegis_tmtc_rx_slot rx_slots[AEGIS_TMTC_MAX_CLIENTS];
    uint32_t             seq_counter;
    uint32_t             tc_count;
    uint32_t             crc_errors;
    uint8_t              last_tc;
    struct sigaction     sa;
    size_t               i;
    int                  arg_i;

    shm_name = AEGIS_DEFAULT_SHM_NAME;
    port = 9100U;
    for (arg_i = 1; arg_i < argc; ++arg_i)
    {
        if (strcmp(argv[arg_i], "--port") == 0 && (arg_i + 1) < argc)
        {
            if (parse_u16_port(argv[++arg_i], &port) != 0)
            {
                (void)fprintf(stderr, "[AEGIS-TMTC] Invalid --port value\n");
                return (1);
            }
        }
        else if (strcmp(argv[arg_i], "--shm") == 0 && (arg_i + 1) < argc)
        {
            shm_name = argv[++arg_i];
        }
        else if (strcmp(argv[arg_i], "--help") == 0 || strcmp(argv[arg_i], "-h") == 0)
        {
            (void)printf("Usage: %s [--port PORT] [--shm NAME]\n", argv[0]);
            return (0);
        }
        else
        {
            (void)fprintf(stderr, "[AEGIS-TMTC] Unknown or incomplete argument: %s\n", argv[arg_i]);
            return (1);
        }
    }

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    owns_shm = 1;
    shm_fd = shm_open(shm_name, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR);
    if (shm_fd < 0)
    {
        owns_shm = 0;
        shm_fd = shm_open(shm_name, O_RDWR, S_IRUSR | S_IWUSR);
    }
    if (shm_fd < 0 || (owns_shm != 0 && ftruncate(shm_fd, (off_t)sizeof(t_aegis_tmtc_shm)) != 0))
    {
        (void)fprintf(stderr, "[AEGIS-TMTC] Failed to initialize SHM '%s'\n", shm_name);
        if (shm_fd >= 0)
            (void)close(shm_fd);
        if (owns_shm != 0)
            (void)shm_unlink(shm_name);
        return (1);
    }

    shm_ptr = (t_aegis_tmtc_shm *)mmap(
        NULL, sizeof(t_aegis_tmtc_shm), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    (void)close(shm_fd);
    if (shm_ptr == MAP_FAILED)
    {
        if (owns_shm != 0)
            (void)shm_unlink(shm_name);
        return (1);
    }
    if (owns_shm != 0)
        (void)memset(shm_ptr, 0, sizeof(t_aegis_tmtc_shm));

    listen_fd = aegis_tmtc_server_open_tcp(port, &port);
    epoll_fd = epoll_create1(0);
    if (listen_fd < 0 || epoll_fd < 0)
    {
        if (listen_fd >= 0)
            (void)close(listen_fd);
        if (epoll_fd >= 0)
            (void)close(epoll_fd);
        (void)munmap(shm_ptr, sizeof(t_aegis_tmtc_shm));
        if (owns_shm != 0)
            (void)shm_unlink(shm_name);
        return (1);
    }

    (void)memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    (void)epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    (void)memset(rx_slots, 0, sizeof(rx_slots));
    for (i = 0U; i < AEGIS_TMTC_MAX_CLIENTS; ++i)
        client_fds[i] = -1;

    seq_counter = 0U;
    tc_count = 0U;
    crc_errors = 0U;
    last_tc = 0U;
    (void)printf("[AEGIS-TMTC] epoll TCP server listening on 127.0.0.1:%u (SHM=%s, owner=%d)\n",
                 (unsigned)port,
                 shm_name,
                 owns_shm);
    (void)fflush(stdout);

    while (g_tmtc_stop == 0)
    {
        (void)aegis_tmtc_epoll_poll_ex(epoll_fd,
                                       listen_fd,
                                       client_fds,
                                       rx_slots,
                                       AEGIS_TMTC_MAX_CLIENTS,
                                       shm_ptr,
                                       &seq_counter,
                                       &tc_count,
                                       &last_tc,
                                       &crc_errors,
                                       50);
    }

    for (i = 0U; i < AEGIS_TMTC_MAX_CLIENTS; ++i)
    {
        if (client_fds[i] >= 0)
            (void)close(client_fds[i]);
    }
    (void)close(epoll_fd);
    (void)close(listen_fd);
    (void)munmap(shm_ptr, sizeof(t_aegis_tmtc_shm));
    if (owns_shm != 0)
        (void)shm_unlink(shm_name);
    (void)printf("[AEGIS-TMTC] Shutdown clean (tx=%u, tc=%u, crc_err=%u)\n",
                 seq_counter,
                 tc_count,
                 crc_errors);
    return (0);
}
