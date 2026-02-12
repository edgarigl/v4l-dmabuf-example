#include "common.h"
#include "virtio_media_uapi.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/videodev2.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

struct capture_buffer {
    void *addr;
    size_t len;
    uint32_t offset;
    /* Exported handle for this MMAP queue slot. */
    bool exported;
    uint64_t handle_id;
    /* Import metadata (grant refs) fetched once and sent to peer guest. */
    bool share_meta_ready;
    struct virtio_media_ioc_import_buffer imp;
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
    int ack_timeout_ms;
    int connect_timeout_ms;
    uint32_t peer_domid;
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
            "  -k <ms>       ACK wait timeout in milliseconds (default 200)\n"
            "  -t <ms>       TCP connect timeout in milliseconds (default 3000)\n"
            "  -D <domid>    Peer Xen domid for grant export (required for cross-guest)\n",
            prog);
}

static int tcp_connect_timeout(const char *ip, uint16_t port, int timeout_ms)
{
    struct sockaddr_in addr;
    struct pollfd pfd;
    int fd;
    int flags;
    int ret;
    int soerr = 0;
    socklen_t soerr_len = sizeof(soerr);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(fd);
        return -1;
    }

    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0) {
        (void)fcntl(fd, F_SETFL, flags);
        return fd;
    }
    if (errno != EINPROGRESS) {
        close(fd);
        return -1;
    }

    pfd.fd = fd;
    pfd.events = POLLOUT;
    ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0) {
        if (ret == 0) {
            errno = ETIMEDOUT;
        }
        close(fd);
        return -1;
    }

    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &soerr_len) < 0) {
        close(fd);
        return -1;
    }
    if (soerr != 0) {
        errno = soerr;
        close(fd);
        return -1;
    }

    if (fcntl(fd, F_SETFL, flags) < 0) {
        close(fd);
        return -1;
    }

    return fd;
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
        .ack_timeout_ms = 200,
        .connect_timeout_ms = 3000,
        .peer_domid = 0,
    };

    while ((c = getopt(argc, argv, "d:a:p:W:H:f:b:n:k:t:D:h")) != -1) {
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
        case 'k':
            cfg->ack_timeout_ms = (int)strtol(optarg, NULL, 10);
            if (cfg->ack_timeout_ms < 0) {
                fprintf(stderr, "ack timeout must be >= 0\n");
                return -1;
            }
            break;
        case 't':
            cfg->connect_timeout_ms = (int)strtol(optarg, NULL, 10);
            if (cfg->connect_timeout_ms <= 0) {
                fprintf(stderr, "connect timeout must be > 0\n");
                return -1;
            }
            break;
        case 'D':
            cfg->peer_domid = (uint32_t)strtoul(optarg, NULL, 10);
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
    if (cfg->peer_domid == 0) {
        fprintf(stderr, "peer domid must be set with -D for cross-guest zero-copy\n");
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

static int fetch_share_metadata(int vfd, struct capture_buffer *b, uint32_t peer_domid)
{
    struct virtio_media_ioc_import_buffer imp;

    memset(&imp, 0, sizeof(imp));
    imp.handle_id = b->handle_id;
    imp.flags = VIRTIO_MEDIA_IMPORT_F_TARGET_DOMID;
    imp.gref_domid = peer_domid;
    imp.dmabuf_fd = -1;

    /*
     * Resolve our exported handle through the local backend to retrieve Xen
     * grant refs. Those refs are what the peer guest imports directly.
     */
    if (xioctl(vfd, VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER, &imp) < 0) {
        fprintf(stderr,
                "VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER(handle=%" PRIu64 ") failed: %s\n",
                b->handle_id, strerror(errno));
        return -1;
    }

    if (!imp.gref_count || imp.gref_count > VIRTIO_MEDIA_MAX_IMPORT_GREFS ||
        imp.gref_page_size == 0 || imp.len == 0) {
        fprintf(stderr,
                "invalid share metadata for handle=%" PRIu64
                " (grefs=%u page=%u len=%" PRIu64 ")\n",
                b->handle_id, imp.gref_count, imp.gref_page_size,
                (uint64_t)imp.len);
        if (imp.dmabuf_fd >= 0) {
            close(imp.dmabuf_fd);
        }
        return -1;
    }

    if (imp.dmabuf_fd >= 0) {
        close(imp.dmabuf_fd);
        imp.dmabuf_fd = -1;
    }

    b->imp = imp;
    b->share_meta_ready = true;
    return 0;
}

static int send_handle_packet(int sock, const struct capture_buffer *b)
{
    struct zc_handle_packet hp;
    struct zc_handle_packet hp_net;
    uint32_t i;

    if (!b->share_meta_ready) {
        return -1;
    }

    memset(&hp, 0, sizeof(hp));
    hp.magic = ZC_HANDLE_MAGIC;
    hp.flags = VIRTIO_MEDIA_IMPORT_F_DIRECT_GREFS;
    hp.handle_id = b->handle_id;
    hp.len = b->imp.len;
    hp.gref_count = b->imp.gref_count;
    hp.gref_page_size = b->imp.gref_page_size;
    hp.gref_domid = b->imp.gref_domid;
    for (i = 0; i < b->imp.gref_count; i++) {
        hp.gref_ids[i] = b->imp.gref_ids[i];
    }

    host_to_net_zc_handle(&hp_net, &hp);
    if (send_all(sock, &hp_net, sizeof(hp_net)) < 0) {
        fprintf(stderr, "failed to send handle metadata packet\n");
        return -1;
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
    bool ack_timeout_warned = false;
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

    /*
     * Export all queue buffers before STREAMON. If this times out, backend
     * export/import support is likely missing or broken on this QEMU build.
     */
    for (i = 0; i < cfg.buffers; i++) {
        if (export_handle_for_buffer(vfd, V4L2_BUF_TYPE_VIDEO_CAPTURE, i,
                                     &bufs[i]) < 0) {
            fprintf(stderr,
                    "export failed before STREAMON; cannot continue zero-copy mode\n");
            goto out;
        }
        if (fetch_share_metadata(vfd, &bufs[i], cfg.peer_domid) < 0) {
            fprintf(stderr,
                    "failed to fetch share metadata for buffer %u\n", i);
            goto out;
        }
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

    fprintf(stderr, "sender_zc: connecting to %s:%u (timeout=%dms)\n",
            cfg.peer_ip, cfg.port, cfg.connect_timeout_ms);
    sock = tcp_connect_timeout(cfg.peer_ip, cfg.port, cfg.connect_timeout_ms);
    if (sock < 0) {
        fprintf(stderr, "connect failed: %s\n", strerror(errno));
        goto out;
    }

    if (cfg.ack_timeout_ms > 0) {
        struct timeval tv = {
            .tv_sec = cfg.ack_timeout_ms / 1000,
            .tv_usec = (cfg.ack_timeout_ms % 1000) * 1000,
        };

        if (setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) < 0) {
            fprintf(stderr, "setsockopt(SO_RCVTIMEO) failed: %s\n",
                    strerror(errno));
            goto out;
        }
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

    /*
     * Send per-buffer grant metadata up front so the receiver can import all
     * handles before frame traffic starts.
     */
    for (i = 0; i < cfg.buffers; i++) {
        if (send_handle_packet(sock, &bufs[i]) < 0) {
            goto out;
        }
    }

    fprintf(stderr,
            "zero-copy sender: %s -> %s:%u (%ux%u buffers=%u ack_timeout=%dms peer_domid=%u)\n",
            cfg.device, cfg.peer_ip, cfg.port,
            cfg.width, cfg.height, cfg.buffers, cfg.ack_timeout_ms,
            cfg.peer_domid);

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
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                if (!ack_timeout_warned) {
                    fprintf(stderr,
                            "ACK timeout reached; continuing without strict per-frame ACK\n");
                    ack_timeout_warned = true;
                }
            } else {
                fprintf(stderr, "failed to receive ACK: %s\n", strerror(errno));
                goto out;
            }
        } else {
            net_to_host_zc_ack(&ack, &ack_net);
            if (ack.magic != FRAME_MAGIC || ack.status != 0 ||
                ack.handle_id != pkt.handle_id || ack.sequence != pkt.sequence) {
                fprintf(stderr,
                        "ignoring mismatched ACK (magic=0x%x status=%u handle=%" PRIu64 " seq=%" PRIu64 ")\n",
                        ack.magic, ack.status, ack.handle_id, ack.sequence);
            }
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
