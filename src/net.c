#include "net.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

mp_status net_read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t r = recv(fd, p + done, n - done, 0);
        if (r > 0) {
            done += (size_t)r;
        } else if (r == 0) {
            return MP_ERR_CLOSED;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MP_ERR_TIMEOUT;
        } else {
            return MP_ERR_IO;
        }
    }
    return MP_OK;
}

mp_status net_write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    size_t done = 0;
    while (done < n) {
        ssize_t w = send(fd, p + done, n - done, MSG_NOSIGNAL);
        if (w >= 0) {
            done += (size_t)w;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MP_ERR_TIMEOUT;
        } else {
            return MP_ERR_IO;
        }
    }
    return MP_OK;
}

mp_status net_recv_frame(int fd, uint8_t *buf, size_t cap, size_t *out_len)
{
    uint8_t hdr[2];
    mp_status st = net_read_full(fd, hdr, sizeof hdr);
    if (st != MP_OK)
        return st;
    size_t len = ((size_t)hdr[0] << 8) | hdr[1];
    if (len == 0 || len > cap)
        return MP_ERR_OVERFLOW;
    st = net_read_full(fd, buf, len);
    if (st != MP_OK)
        return st;
    *out_len = len;
    return MP_OK;
}

mp_status net_send_frame(int fd, const uint8_t *buf, size_t len)
{
    if (len == 0 || len > 0xFFFF)
        return MP_ERR_ARG;
    uint8_t frame[2 + NET_MAX_FRAME];
    if (len > NET_MAX_FRAME)
        return MP_ERR_OVERFLOW;
    frame[0] = (uint8_t)(len >> 8);
    frame[1] = (uint8_t)(len & 0xFF);
    memcpy(frame + 2, buf, len);
    return net_write_full(fd, frame, len + 2);
}

mp_status net_set_timeouts(int fd, int seconds)
{
    struct timeval tv = {.tv_sec = seconds, .tv_usec = 0};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv) != 0 ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv) != 0)
        return MP_ERR_IO;
    return MP_OK;
}

static int make_addr(const char *host, uint16_t port, struct sockaddr_in *addr)
{
    memset(addr, 0, sizeof *addr);
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);
    return inet_pton(AF_INET, host, &addr->sin_addr) == 1 ? 0 : -1;
}

int net_listen(const char *host, uint16_t port, int backlog)
{
    struct sockaddr_in addr;
    if (make_addr(host, port, &addr) != 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    int one = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) != 0)
        goto fail;
    if (bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0)
        goto fail;
    if (listen(fd, backlog) != 0)
        goto fail;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0)
        goto fail;
    return fd;

fail:;
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

int net_connect(const char *host, uint16_t port)
{
    struct sockaddr_in addr;
    if (make_addr(host, port, &addr) != 0) {
        errno = EINVAL;
        return -1;
    }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}
