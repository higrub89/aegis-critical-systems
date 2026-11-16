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

int main(int argc, char **argv)
{
    const char        *shm_name;
    uint16_t           port;
    int                shm_fd;
    t_aegis_tmtc_shm  *shm_ptr;
    int                listen_fd;
    int                epoll_fd;
    struct epoll_event ev;
    int                client_fds[AEGIS_TMTC_MAX_CLIENTS];
    uint32_t           seq_counter;
    uint32_t           tc_count;
    uint8_t            last_tc;
    struct sigaction   sa;
    size_t             i;
    int                arg_i;

    shm_name = "/aegis_tmtc_shm";
    port = 9100U;
    for (arg_i = 1; arg_i < argc; ++arg_i)
    {
        if (strcmp(argv[arg_i], "--port") == 0 && (arg_i + 1) < argc)
            port = (uint16_t)strtoul(argv[++arg_i], NULL, 10);
        else if (strcmp(argv[arg_i], "--shm") == 0 && (arg_i + 1) < argc)
            shm_name = argv[++arg_i];
    }

    (void)memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sig;
    (void)sigaction(SIGINT, &sa, NULL);
    (void)sigaction(SIGTERM, &sa, NULL);

    shm_fd = shm_open(shm_name, O_CREAT | O_RDWR, S_IRUSR | S_IWUSR);
    if (shm_fd < 0 || ftruncate(shm_fd, (off_t)sizeof(t_aegis_tmtc_shm)) != 0)
    {
        (void)fprintf(stderr, "[AEGIS-TMTC] Failed to initialize SHM '%s'\n", shm_name);
        if (shm_fd >= 0)
            (void)close(shm_fd);
        return (1);
    }

    shm_ptr = (t_aegis_tmtc_shm *)mmap(
        NULL, sizeof(t_aegis_tmtc_shm), PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    (void)close(shm_fd);
    if (shm_ptr == MAP_FAILED)
    {
        (void)shm_unlink(shm_name);
        return (1);
    }

    listen_fd = aegis_tmtc_server_open_tcp(port, &port);
    epoll_fd = epoll_create1(0);
    if (listen_fd < 0 || epoll_fd < 0)
    {
        if (listen_fd >= 0)
            (void)close(listen_fd);
        if (epoll_fd >= 0)
            (void)close(epoll_fd);
        (void)munmap(shm_ptr, sizeof(t_aegis_tmtc_shm));
        (void)shm_unlink(shm_name);
        return (1);
    }

    (void)memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    (void)epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    for (i = 0U; i < AEGIS_TMTC_MAX_CLIENTS; ++i)
        client_fds[i] = -1;

    seq_counter = 0U;
    tc_count = 0U;
    last_tc = 0U;
    (void)printf("[AEGIS-TMTC] epoll TCP server listening on 127.0.0.1:%u (SHM=%s)\n",
                 (unsigned)port,
                 shm_name);

    while (g_tmtc_stop == 0)
    {
        (void)aegis_tmtc_epoll_poll_once(epoll_fd,
                                         listen_fd,
                                         client_fds,
                                         AEGIS_TMTC_MAX_CLIENTS,
                                         shm_ptr,
                                         &seq_counter,
                                         &tc_count,
                                         &last_tc,
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
    (void)shm_unlink(shm_name);
    (void)printf("[AEGIS-TMTC] Shutdown clean (tx=%u, tc=%u)\n", seq_counter, tc_count);
    return (0);
}
