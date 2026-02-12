#ifndef V4L_DMABUF_COMMON_H
#define V4L_DMABUF_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <linux/videodev2.h>

#define STREAM_MAGIC 0x56344c42u /* 'V4LB' */
#define FRAME_MAGIC 0x5652464du  /* 'VFRM' */
#define PROTO_VERSION 1u

struct stream_hello {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t sizeimage;
    uint32_t buffer_count;
};

struct frame_packet {
    uint32_t magic;
    uint32_t index;
    uint32_t bytesused;
    uint32_t flags;
    uint64_t sequence;
    uint64_t ts_sec;
    uint64_t ts_usec;
};

int xioctl(int fd, unsigned long request, void *arg);
int open_v4l2_device(const char *path);

int alloc_dmabuf_from_heap(const char *heap_path, size_t size);

int tcp_listen_and_accept(const char *bind_ip, uint16_t port);
int tcp_connect(const char *ip, uint16_t port);

int send_all(int fd, const void *buf, size_t len);
int recv_all(int fd, void *buf, size_t len);

uint64_t htonll_u64(uint64_t value);
uint64_t ntohll_u64(uint64_t value);

uint32_t parse_fourcc_or_die(const char *s);
void fourcc_to_text(uint32_t fourcc, char out[5]);

void host_to_net_hello(struct stream_hello *dst, const struct stream_hello *src);
void net_to_host_hello(struct stream_hello *dst, const struct stream_hello *src);
void host_to_net_frame(struct frame_packet *dst, const struct frame_packet *src);
void net_to_host_frame(struct frame_packet *dst, const struct frame_packet *src);

#endif
