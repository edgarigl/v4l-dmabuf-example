#include "common.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-heap.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

int xioctl(int fd, unsigned long request, void *arg)
{
    int ret;

    /* Retry interrupted ioctls so callers can treat EINTR as transparent. */
    do {
        ret = ioctl(fd, request, arg);
    } while (ret == -1 && errno == EINTR);

    return ret;
}

int open_v4l2_device(const char *path)
{
    int fd = open(path, O_RDWR | O_CLOEXEC);

    if (fd < 0) {
        fprintf(stderr, "failed to open %s: %s\n", path, strerror(errno));
    }

    return fd;
}

int alloc_dmabuf_from_heap(const char *heap_path, size_t size)
{
    struct dma_heap_allocation_data alloc = {
        .len = size,
        .fd_flags = O_RDWR | O_CLOEXEC,
        .heap_flags = 0,
    };
    int heap_fd;

    /*
     * Allocate a shareable buffer from dma-heap. In this example the buffer
     * is mmapped by user space and passed to V4L2 as V4L2_MEMORY_DMABUF.
     */
    heap_fd = open(heap_path, O_RDWR | O_CLOEXEC);
    if (heap_fd < 0) {
        fprintf(stderr, "failed to open %s: %s\n", heap_path, strerror(errno));
        return -1;
    }

    if (xioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
        fprintf(stderr, "DMA_HEAP_IOCTL_ALLOC failed on %s: %s\n",
                heap_path, strerror(errno));
        close(heap_fd);
        return -1;
    }

    close(heap_fd);
    return alloc.fd;
}

static int setup_sockaddr(struct sockaddr_in *addr, const char *ip, uint16_t port)
{
    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons(port);

    if (!ip || strcmp(ip, "0.0.0.0") == 0) {
        addr->sin_addr.s_addr = htonl(INADDR_ANY);
        return 0;
    }

    if (inet_pton(AF_INET, ip, &addr->sin_addr) != 1) {
        fprintf(stderr, "invalid IPv4 address: %s\n", ip);
        return -1;
    }

    return 0;
}

int tcp_listen_and_accept(const char *bind_ip, uint16_t port)
{
    struct sockaddr_in addr;
    int listen_fd;
    int conn_fd;
    int optval = 1;

    if (setup_sockaddr(&addr, bind_ip, port) < 0) {
        return -1;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) {
        fprintf(stderr, "setsockopt(SO_REUSEADDR) failed: %s\n", strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "bind() failed: %s\n", strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (listen(listen_fd, 1) < 0) {
        fprintf(stderr, "listen() failed: %s\n", strerror(errno));
        close(listen_fd);
        return -1;
    }

    conn_fd = accept(listen_fd, NULL, NULL);
    if (conn_fd < 0) {
        fprintf(stderr, "accept() failed: %s\n", strerror(errno));
        close(listen_fd);
        return -1;
    }

    if (fcntl(conn_fd, F_SETFD, FD_CLOEXEC) < 0) {
        fprintf(stderr, "fcntl(FD_CLOEXEC) failed: %s\n", strerror(errno));
        close(conn_fd);
        close(listen_fd);
        return -1;
    }

    close(listen_fd);
    return conn_fd;
}

int tcp_connect(const char *ip, uint16_t port)
{
    struct sockaddr_in addr;
    int fd;

    if (setup_sockaddr(&addr, ip, port) < 0) {
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "socket() failed: %s\n", strerror(errno));
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "connect() to %s:%u failed: %s\n",
                ip, port, strerror(errno));
        close(fd);
        return -1;
    }

    return fd;
}

int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    /* TCP is a stream: one send() may not write the full message. */
    while (len > 0) {
        ssize_t ret = send(fd, p, len, MSG_NOSIGNAL);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ret == 0) {
            return -1;
        }

        p += ret;
        len -= (size_t)ret;
    }

    return 0;
}

int recv_all(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;

    /* Read exactly len bytes so packet boundaries stay explicit in user space. */
    while (len > 0) {
        ssize_t ret = recv(fd, p, len, 0);

        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (ret == 0) {
            return -1;
        }

        p += ret;
        len -= (size_t)ret;
    }

    return 0;
}

uint64_t htonll_u64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(value >> 32))) |
           ((uint64_t)htonl((uint32_t)value) << 32);
#else
    return value;
#endif
}

uint64_t ntohll_u64(uint64_t value)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)ntohl((uint32_t)(value >> 32))) |
           ((uint64_t)ntohl((uint32_t)value) << 32);
#else
    return value;
#endif
}

uint32_t parse_fourcc_or_die(const char *s)
{
    if (!s || strlen(s) != 4) {
        fprintf(stderr, "pixel format must be 4 characters, got: %s\n", s ? s : "(null)");
        exit(1);
    }

    return v4l2_fourcc(s[0], s[1], s[2], s[3]);
}

void fourcc_to_text(uint32_t fourcc, char out[5])
{
    out[0] = (char)(fourcc & 0xff);
    out[1] = (char)((fourcc >> 8) & 0xff);
    out[2] = (char)((fourcc >> 16) & 0xff);
    out[3] = (char)((fourcc >> 24) & 0xff);
    out[4] = '\0';
}

void host_to_net_hello(struct stream_hello *dst, const struct stream_hello *src)
{
    /* Keep wire format endian-stable across architectures/guests. */
    dst->magic = htonl(src->magic);
    dst->version = htonl(src->version);
    dst->width = htonl(src->width);
    dst->height = htonl(src->height);
    dst->pixelformat = htonl(src->pixelformat);
    dst->sizeimage = htonl(src->sizeimage);
    dst->buffer_count = htonl(src->buffer_count);
}

void net_to_host_hello(struct stream_hello *dst, const struct stream_hello *src)
{
    dst->magic = ntohl(src->magic);
    dst->version = ntohl(src->version);
    dst->width = ntohl(src->width);
    dst->height = ntohl(src->height);
    dst->pixelformat = ntohl(src->pixelformat);
    dst->sizeimage = ntohl(src->sizeimage);
    dst->buffer_count = ntohl(src->buffer_count);
}

void host_to_net_frame(struct frame_packet *dst, const struct frame_packet *src)
{
    /* Frame headers travel over TCP before payload bytes. */
    dst->magic = htonl(src->magic);
    dst->index = htonl(src->index);
    dst->bytesused = htonl(src->bytesused);
    dst->flags = htonl(src->flags);
    dst->sequence = htonll_u64(src->sequence);
    dst->ts_sec = htonll_u64(src->ts_sec);
    dst->ts_usec = htonll_u64(src->ts_usec);
}

void net_to_host_frame(struct frame_packet *dst, const struct frame_packet *src)
{
    dst->magic = ntohl(src->magic);
    dst->index = ntohl(src->index);
    dst->bytesused = ntohl(src->bytesused);
    dst->flags = ntohl(src->flags);
    dst->sequence = ntohll_u64(src->sequence);
    dst->ts_sec = ntohll_u64(src->ts_sec);
    dst->ts_usec = ntohll_u64(src->ts_usec);
}

void host_to_net_zc_frame(struct zc_frame_packet *dst, const struct zc_frame_packet *src)
{
    dst->magic = htonl(src->magic);
    dst->buffer_index = htonl(src->buffer_index);
    dst->bytesused = htonl(src->bytesused);
    dst->flags = htonl(src->flags);
    dst->handle_id = htonll_u64(src->handle_id);
    dst->sequence = htonll_u64(src->sequence);
    dst->ts_sec = htonll_u64(src->ts_sec);
    dst->ts_usec = htonll_u64(src->ts_usec);
}

void net_to_host_zc_frame(struct zc_frame_packet *dst, const struct zc_frame_packet *src)
{
    dst->magic = ntohl(src->magic);
    dst->buffer_index = ntohl(src->buffer_index);
    dst->bytesused = ntohl(src->bytesused);
    dst->flags = ntohl(src->flags);
    dst->handle_id = ntohll_u64(src->handle_id);
    dst->sequence = ntohll_u64(src->sequence);
    dst->ts_sec = ntohll_u64(src->ts_sec);
    dst->ts_usec = ntohll_u64(src->ts_usec);
}

void host_to_net_zc_ack(struct zc_ack_packet *dst, const struct zc_ack_packet *src)
{
    dst->magic = htonl(src->magic);
    dst->status = htonl(src->status);
    dst->handle_id = htonll_u64(src->handle_id);
    dst->sequence = htonll_u64(src->sequence);
}

void net_to_host_zc_ack(struct zc_ack_packet *dst, const struct zc_ack_packet *src)
{
    dst->magic = ntohl(src->magic);
    dst->status = ntohl(src->status);
    dst->handle_id = ntohll_u64(src->handle_id);
    dst->sequence = ntohll_u64(src->sequence);
}
