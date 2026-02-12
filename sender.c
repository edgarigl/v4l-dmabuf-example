#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct buffer_ctx {
    int fd;
    void *addr;
    size_t len;
};

struct sender_cfg {
    const char *device;
    const char *peer_ip;
    const char *heap;
    uint16_t port;
    uint32_t width;
    uint32_t height;
    uint32_t pixelformat;
    uint32_t buffers;
    int frame_limit;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -a <peer-ip> -p <port> [options]\n"
            "Options:\n"
            "  -d <dev>      V4L2 device (default /dev/video0)\n"
            "  -a <ip>       Receiver IPv4 address (required)\n"
            "  -p <port>     Receiver port (required)\n"
            "  -W <width>    Capture width (default 640)\n"
            "  -H <height>   Capture height (default 480)\n"
            "  -f <fourcc>   Pixel format (default YUYV)\n"
            "  -b <count>    Buffer count (default 4)\n"
            "  -n <frames>   Stop after N frames (default: unlimited)\n"
            "  -e <heap>     dma-heap device (default /dev/dma_heap/system)\n",
            prog);
}

static int parse_args(int argc, char **argv, struct sender_cfg *cfg)
{
    int c;

    *cfg = (struct sender_cfg){
        .device = "/dev/video0",
        .peer_ip = NULL,
        .heap = "/dev/dma_heap/system",
        .port = 0,
        .width = 640,
        .height = 480,
        .pixelformat = v4l2_fourcc('Y', 'U', 'Y', 'V'),
        .buffers = 4,
        .frame_limit = -1,
    };

    while ((c = getopt(argc, argv, "d:a:p:W:H:f:b:n:e:h")) != -1) {
        switch (c) {
        case 'd':
            cfg->device = optarg;
            break;
        case 'a':
            cfg->peer_ip = optarg;
            break;
        case 'p':
            cfg->port = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'W':
            cfg->width = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'H':
            cfg->height = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'f':
            cfg->pixelformat = parse_fourcc_or_die(optarg);
            break;
        case 'b':
            cfg->buffers = (uint32_t)strtoul(optarg, NULL, 10);
            break;
        case 'n':
            cfg->frame_limit = (int)strtol(optarg, NULL, 10);
            break;
        case 'e':
            cfg->heap = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return -1;
        }
    }

    if (!cfg->peer_ip || cfg->port == 0 || cfg->buffers == 0) {
        usage(argv[0]);
        return -1;
    }

    return 0;
}

static int setup_capture_format(int vfd, struct sender_cfg *cfg,
                                struct v4l2_format *fmt)
{
    /* Ask the driver for our preferred format, then keep negotiated values. */
    memset(fmt, 0, sizeof(*fmt));
    fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt->fmt.pix.width = cfg->width;
    fmt->fmt.pix.height = cfg->height;
    fmt->fmt.pix.pixelformat = cfg->pixelformat;
    fmt->fmt.pix.field = V4L2_FIELD_NONE;

    if (xioctl(vfd, VIDIOC_S_FMT, fmt) < 0) {
        fprintf(stderr, "VIDIOC_S_FMT failed: %s\n", strerror(errno));
        return -1;
    }

    cfg->width = fmt->fmt.pix.width;
    cfg->height = fmt->fmt.pix.height;
    cfg->pixelformat = fmt->fmt.pix.pixelformat;

    return 0;
}

static int request_dmabuf_capture(int vfd, uint32_t count)
{
    struct v4l2_requestbuffers req = {
        .count = count,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_DMABUF,
    };

    /*
     * Enable DMABUF queueing mode. The application owns the backing memory
     * and only passes fds to the capture driver.
     */
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS(DMABUF) failed: %s\n", strerror(errno));
        return -1;
    }

    if (req.count < count) {
        fprintf(stderr, "requested %u buffers, got %u\n", count, req.count);
        return -1;
    }

    return 0;
}

static int queue_buffer(int vfd, unsigned int index, int dmabuf_fd, uint32_t len)
{
    struct v4l2_buffer buf;

    /* Queue one capture buffer backed by dmabuf_fd. */
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = index;
    buf.m.fd = dmabuf_fd;
    buf.length = len;

    if (xioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "VIDIOC_QBUF(index=%u) failed: %s\n", index, strerror(errno));
        return -1;
    }

    return 0;
}

static int setup_dmabufs(struct sender_cfg *cfg,
                         struct buffer_ctx *bufs,
                         uint32_t count,
                         uint32_t sizeimage)
{
    uint32_t i;

    /* Allocate and map userspace-visible DMABUFs that V4L2 will fill. */
    for (i = 0; i < count; i++) {
        bufs[i].fd = alloc_dmabuf_from_heap(cfg->heap, sizeimage);
        if (bufs[i].fd < 0) {
            return -1;
        }
        bufs[i].len = sizeimage;
        bufs[i].addr = mmap(NULL, sizeimage, PROT_READ | PROT_WRITE,
                            MAP_SHARED, bufs[i].fd, 0);
        if (bufs[i].addr == MAP_FAILED) {
            fprintf(stderr, "mmap(dmabuf idx=%u) failed: %s\n", i, strerror(errno));
            bufs[i].addr = NULL;
            return -1;
        }
    }

    return 0;
}

static void cleanup_dmabufs(struct buffer_ctx *bufs, uint32_t count)
{
    uint32_t i;

    if (!bufs) {
        return;
    }

    for (i = 0; i < count; i++) {
        if (bufs[i].addr) {
            munmap(bufs[i].addr, bufs[i].len);
        }
        if (bufs[i].fd >= 0) {
            close(bufs[i].fd);
        }
    }
}

int main(int argc, char **argv)
{
    struct sender_cfg cfg;
    struct v4l2_format fmt;
    struct buffer_ctx *bufs = NULL;
    struct stream_hello hello;
    struct stream_hello hello_net;
    struct timeval start_tv = {0};
    struct timeval now_tv;
    uint64_t frames = 0;
    int vfd = -1;
    int sock = -1;
    int rc = 1;
    uint32_t i;

    if (parse_args(argc, argv, &cfg) < 0) {
        return 1;
    }

    vfd = open_v4l2_device(cfg.device);
    if (vfd < 0) {
        goto out;
    }

    if (setup_capture_format(vfd, &cfg, &fmt) < 0) {
        goto out;
    }

    if (request_dmabuf_capture(vfd, cfg.buffers) < 0) {
        goto out;
    }

    bufs = calloc(cfg.buffers, sizeof(*bufs));
    if (!bufs) {
        fprintf(stderr, "calloc buffers failed\n");
        goto out;
    }
    for (i = 0; i < cfg.buffers; i++) {
        bufs[i].fd = -1;
    }

    if (setup_dmabufs(&cfg, bufs, cfg.buffers, fmt.fmt.pix.sizeimage) < 0) {
        goto out;
    }

    /* Prime the capture queue with all available buffers before STREAMON. */
    for (i = 0; i < cfg.buffers; i++) {
        if (queue_buffer(vfd, i, bufs[i].fd, fmt.fmt.pix.sizeimage) < 0) {
            goto out;
        }
    }

    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        if (xioctl(vfd, VIDIOC_STREAMON, &type) < 0) {
            fprintf(stderr, "VIDIOC_STREAMON failed: %s\n", strerror(errno));
            goto out;
        }
    }

    sock = tcp_connect(cfg.peer_ip, cfg.port);
    if (sock < 0) {
        goto out;
    }

    /* Send one stream header so receiver can size/interpret incoming frames. */
    hello.magic = STREAM_MAGIC;
    hello.version = PROTO_VERSION;
    hello.width = cfg.width;
    hello.height = cfg.height;
    hello.pixelformat = cfg.pixelformat;
    hello.sizeimage = fmt.fmt.pix.sizeimage;
    hello.buffer_count = cfg.buffers;

    host_to_net_hello(&hello_net, &hello);
    if (send_all(sock, &hello_net, sizeof(hello_net)) < 0) {
        fprintf(stderr, "failed to send stream header\n");
        goto out;
    }

    {
        char fourcc[5];
        fourcc_to_text(cfg.pixelformat, fourcc);
        fprintf(stderr,
                "capturing %ux%u %s, sizeimage=%u, buffers=%u -> %s:%u\n",
                cfg.width, cfg.height, fourcc, fmt.fmt.pix.sizeimage,
                cfg.buffers, cfg.peer_ip, cfg.port);
    }

    gettimeofday(&start_tv, NULL);

    while (cfg.frame_limit < 0 || (int)frames < cfg.frame_limit) {
        struct v4l2_buffer buf;
        struct frame_packet pkt;
        struct frame_packet pkt_net;
        uint32_t bytesused;

        /*
         * DQBUF gives us the next completed DMABUF index.
         * We forward metadata + payload, then re-queue the same DMABUF.
         */
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_DMABUF;

        if (xioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
            goto out;
        }
        if (buf.index >= cfg.buffers) {
            fprintf(stderr, "invalid buffer index from DQBUF: %u\n", buf.index);
            goto out;
        }

        bytesused = buf.bytesused;
        if (bytesused > bufs[buf.index].len) {
            bytesused = (uint32_t)bufs[buf.index].len;
        }

        pkt.magic = FRAME_MAGIC;
        pkt.index = buf.index;
        pkt.bytesused = bytesused;
        pkt.flags = buf.flags;
        pkt.sequence = buf.sequence;
        pkt.ts_sec = (uint64_t)buf.timestamp.tv_sec;
        pkt.ts_usec = (uint64_t)buf.timestamp.tv_usec;

        host_to_net_frame(&pkt_net, &pkt);
        if (send_all(sock, &pkt_net, sizeof(pkt_net)) < 0) {
            fprintf(stderr, "send frame header failed\n");
            goto out;
        }
        if (send_all(sock, bufs[buf.index].addr, bytesused) < 0) {
            fprintf(stderr, "send frame payload failed\n");
            goto out;
        }

        if (queue_buffer(vfd, buf.index, bufs[buf.index].fd, fmt.fmt.pix.sizeimage) < 0) {
            goto out;
        }

        frames++;
        gettimeofday(&now_tv, NULL);
        if (now_tv.tv_sec > start_tv.tv_sec ||
            (now_tv.tv_sec == start_tv.tv_sec &&
             now_tv.tv_usec - start_tv.tv_usec >= 1000000)) {
            double elapsed = (now_tv.tv_sec - start_tv.tv_sec) +
                             (now_tv.tv_usec - start_tv.tv_usec) / 1000000.0;
            fprintf(stderr, "sender fps: %.2f (%" PRIu64 " frames)\n", frames / elapsed, frames);
        }
    }

    rc = 0;
out:
    if (vfd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(vfd, VIDIOC_STREAMOFF, &type);
    }
    if (sock >= 0) {
        close(sock);
    }
    cleanup_dmabufs(bufs, cfg.buffers);
    free(bufs);
    if (vfd >= 0) {
        close(vfd);
    }
    return rc;
}
