#include "common.h"
#include "virtio_media_uapi.h"

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
#include <unistd.h>

struct capture_buffer {
    void *addr;
    size_t len;
    uint32_t offset;
    /* Handle metadata is created lazily on first frame for this queue index. */
    bool exported;
    uint64_t handle_id;
};

struct sender_cfg {
    const char *device;
    const char *peer_ip;
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
            "  -n <frames>   Stop after N frames (default: unlimited)\n",
            prog);
}

static int parse_args(int argc, char **argv, struct sender_cfg *cfg)
{
    int c;

    *cfg = (struct sender_cfg){
        .device = "/dev/video0",
        .peer_ip = NULL,
        .port = 0,
        .width = 640,
        .height = 480,
        .pixelformat = v4l2_fourcc('Y', 'U', 'Y', 'V'),
        .buffers = 4,
        .frame_limit = -1,
    };

    while ((c = getopt(argc, argv, "d:a:p:W:H:f:b:n:h")) != -1) {
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

static int setup_format(int vfd, struct sender_cfg *cfg, struct v4l2_format *fmt)
{
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

static int setup_mmap_capture(int vfd, struct sender_cfg *cfg, struct capture_buffer **out_bufs)
{
    struct v4l2_requestbuffers req = {
        .count = cfg->buffers,
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
        .memory = V4L2_MEMORY_MMAP,
    };
    struct capture_buffer *bufs;
    uint32_t i;

    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0) {
        fprintf(stderr, "VIDIOC_REQBUFS(MMAP) failed: %s\n", strerror(errno));
        return -1;
    }
    if (req.count == 0) {
        fprintf(stderr, "driver returned zero MMAP buffers\n");
        return -1;
    }

    cfg->buffers = req.count;
    bufs = calloc(cfg->buffers, sizeof(*bufs));
    if (!bufs) {
        return -1;
    }

    for (i = 0; i < cfg->buffers; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(vfd, VIDIOC_QUERYBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_QUERYBUF(%u) failed: %s\n", i, strerror(errno));
            free(bufs);
            return -1;
        }

        bufs[i].len = buf.length;
        bufs[i].offset = buf.m.offset;
        /*
         * MMAP is used here because these queue buffers are what we export as
         * cross-guest handles.
         */
        bufs[i].addr = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                            MAP_SHARED, vfd, buf.m.offset);
        if (bufs[i].addr == MAP_FAILED) {
            fprintf(stderr, "mmap(%u) failed: %s\n", i, strerror(errno));
            bufs[i].addr = NULL;
            free(bufs);
            return -1;
        }
    }

    *out_bufs = bufs;
    return 0;
}

static void cleanup_mmap_capture(struct capture_buffer *bufs, uint32_t count)
{
    uint32_t i;

    if (!bufs) {
        return;
    }

    for (i = 0; i < count; i++) {
        if (bufs[i].addr) {
            munmap(bufs[i].addr, bufs[i].len);
        }
    }

    free(bufs);
}

static int queue_index(int vfd, uint32_t idx)
{
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = idx;

    if (xioctl(vfd, VIDIOC_QBUF, &buf) < 0) {
        fprintf(stderr, "VIDIOC_QBUF(%u) failed: %s\n", idx, strerror(errno));
        return -1;
    }

    return 0;
}

static int export_handle_for_buffer(int vfd, uint32_t queue_type,
                                    uint32_t index, struct capture_buffer *b)
{
    struct virtio_media_ioc_export_buffer exp;

    memset(&exp, 0, sizeof(exp));
    exp.queue_type = queue_type;
    exp.buffer_index = index;
    exp.plane_index = 0;
    exp.flags = 0;
    exp.dmabuf_fd = -1;

    /*
     * Driver-private export ioctl: maps a local queue buffer index to an
     * opaque shareable handle understood by peer guests.
     */
    if (xioctl(vfd, VIDIOC_VIRTIO_MEDIA_EXPORT_BUFFER, &exp) < 0) {
        fprintf(stderr, "VIDIOC_VIRTIO_MEDIA_EXPORT_BUFFER(index=%u) failed: %s\n",
                index, strerror(errno));
        return -1;
    }

    b->exported = true;
    b->handle_id = exp.handle_id;

    if (exp.dmabuf_fd >= 0) {
        close(exp.dmabuf_fd);
    }

    return 0;
}

static void release_exported_handles(int vfd, const struct capture_buffer *bufs, uint32_t count)
{
    uint32_t i;

    for (i = 0; i < count; i++) {
        struct virtio_media_ioc_release_handle rel;

        if (!bufs[i].exported) {
            continue;
        }

        memset(&rel, 0, sizeof(rel));
        rel.handle_id = bufs[i].handle_id;
        xioctl(vfd, VIDIOC_VIRTIO_MEDIA_RELEASE_HANDLE, &rel);
    }
}

int main(int argc, char **argv)
{
    struct sender_cfg cfg;
    struct v4l2_format fmt;
    struct capture_buffer *bufs = NULL;
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

    if (setup_format(vfd, &cfg, &fmt) < 0) {
        goto out;
    }

    if (setup_mmap_capture(vfd, &cfg, &bufs) < 0) {
        goto out;
    }

    /* Queue all capture buffers before STREAMON to start a steady pipeline. */
    for (i = 0; i < cfg.buffers; i++) {
        if (queue_index(vfd, i) < 0) {
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

    fprintf(stderr,
            "zero-copy sender: %s -> %s:%u (%ux%u buffers=%u)\n",
            cfg.device, cfg.peer_ip, cfg.port,
            cfg.width, cfg.height, cfg.buffers);

    gettimeofday(&start_tv, NULL);

    while (cfg.frame_limit < 0 || (int)frames < cfg.frame_limit) {
        struct v4l2_buffer buf;
        struct zc_frame_packet pkt;
        struct zc_frame_packet pkt_net;
        struct zc_ack_packet ack_net;
        struct zc_ack_packet ack;
        uint32_t bytesused;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(vfd, VIDIOC_DQBUF, &buf) < 0) {
            fprintf(stderr, "VIDIOC_DQBUF failed: %s\n", strerror(errno));
            goto out;
        }
        if (buf.index >= cfg.buffers) {
            fprintf(stderr, "invalid DQBUF index %u\n", buf.index);
            goto out;
        }

        /* Export each queue buffer once, then reuse its handle id every frame. */
        if (!bufs[buf.index].exported) {
            if (export_handle_for_buffer(vfd, buf.type, buf.index, &bufs[buf.index]) < 0) {
                goto out;
            }
        }

        bytesused = buf.bytesused;
        if (bytesused > bufs[buf.index].len) {
            bytesused = (uint32_t)bufs[buf.index].len;
        }

        /*
         * Zero-copy control packet: only metadata and handle_id go over TCP.
         * The pixels stay in shared memory backed by that handle.
         */
        pkt.magic = FRAME_MAGIC;
        pkt.buffer_index = buf.index;
        pkt.bytesused = bytesused;
        pkt.flags = buf.flags;
        pkt.handle_id = bufs[buf.index].handle_id;
        pkt.sequence = buf.sequence;
        pkt.ts_sec = (uint64_t)buf.timestamp.tv_sec;
        pkt.ts_usec = (uint64_t)buf.timestamp.tv_usec;

        host_to_net_zc_frame(&pkt_net, &pkt);
        if (send_all(sock, &pkt_net, sizeof(pkt_net)) < 0) {
            fprintf(stderr, "failed to send frame control packet\n");
            goto out;
        }

        /*
         * Wait for receiver ACK before requeueing this buffer.
         * This preserves ownership ordering without extra fencing support.
         */
        if (recv_all(sock, &ack_net, sizeof(ack_net)) < 0) {
            fprintf(stderr, "failed to receive ACK\n");
            goto out;
        }
        net_to_host_zc_ack(&ack, &ack_net);
        if (ack.magic != FRAME_MAGIC || ack.status != 0 ||
            ack.handle_id != pkt.handle_id || ack.sequence != pkt.sequence) {
            fprintf(stderr,
                    "invalid ACK (magic=0x%x status=%u handle=%" PRIu64 " seq=%" PRIu64 ")\n",
                    ack.magic, ack.status, ack.handle_id, ack.sequence);
            goto out;
        }

        if (queue_index(vfd, buf.index) < 0) {
            goto out;
        }

        frames++;
        gettimeofday(&now_tv, NULL);
        if (now_tv.tv_sec > start_tv.tv_sec ||
            (now_tv.tv_sec == start_tv.tv_sec &&
             now_tv.tv_usec - start_tv.tv_usec >= 1000000)) {
            double elapsed = (now_tv.tv_sec - start_tv.tv_sec) +
                             (now_tv.tv_usec - start_tv.tv_usec) / 1000000.0;
            fprintf(stderr, "sender_zc fps: %.2f (%" PRIu64 " frames)\n", frames / elapsed, frames);
        }
    }

    rc = 0;
out:
    if (vfd >= 0) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(vfd, VIDIOC_STREAMOFF, &type);
    }

    if (vfd >= 0 && bufs) {
        release_exported_handles(vfd, bufs, cfg.buffers);
    }

    if (sock >= 0) {
        close(sock);
    }
    cleanup_mmap_capture(bufs, cfg.buffers);
    if (vfd >= 0) {
        close(vfd);
    }

    return rc;
}
