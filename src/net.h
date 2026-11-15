#ifndef MP_NET_H
#define MP_NET_H

#include <stddef.h>
#include <stdint.h>

#include "common.h"

#define NET_MAX_FRAME 4096

mp_status net_read_full(int fd, void *buf, size_t n);
mp_status net_write_full(int fd, const void *buf, size_t n);

mp_status net_recv_frame(int fd, uint8_t *buf, size_t cap, size_t *out_len);
mp_status net_send_frame(int fd, const uint8_t *buf, size_t len);

mp_status net_set_timeouts(int fd, int seconds);

int net_listen(const char *host, uint16_t port, int backlog);
int net_connect(const char *host, uint16_t port);

#endif
