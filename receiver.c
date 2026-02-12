#include "common.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

struct buffer_ctx {
    int fd;
    void *addr;
    size_t len;
};

struct receiver_cfg {
    const char *bind_ip;
    const char *heap;
    const char *output_path;
    uint16_t port;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -p <port> [options]\n"
            "Options:\n"
            "  -l <ip>       Listen IPv4 address (default 0.0.0.0)\n"
            "  -p <port>     Listen port (required)\n"
            "  -e <heap>     dma-heap device (default /dev/dma_heap/system)\n"
            "  -o <path>     Optional raw output file\n",
            prog);
}

static int parse_args(int argc, char **argv, struct receiver_cfg *cfg)
{
    int c;

    *cfg = (struct receiver_cfg){
        .bind_ip = "0.0.0.0",
        .heap = "/dev/dma_heap/system",
        .output_path = NULL,
        .port = 0,
    };

    while ((c = getopt(argc, argv, "l:p:e:o:h")) != -1) {
        switch (c) {
        case 'l':
            cfg->bind_ip = optarg;
            break;
        case 'p':
            cfg->port = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'e':
            cfg->heap = optarg;
            break;
        case 'o':
            cfg->output_path = optarg;
            break;
        case 'h':
        default:
            usage(argv[0]);
            return -1;
        }
    }

    if (cfg->port == 0) {
        usage(argv[0]);
        return -1;
    }

    return 0;
}

static int setup_dmabufs(const struct receiver_cfg *cfg,
                         struct buffer_ctx **out_bufs,
                         uint32_t count,
                         uint32_t sizeimage)
{
    struct buffer_ctx *bufs;
    uint32_t i;

    bufs = calloc(count, sizeof(*bufs));
    if (!bufs) {
        return -1;
    }

    for (i = 0; i < count; i++) {
        bufs[i].fd = alloc_dmabuf_from_heap(cfg->heap, sizeimage);
        if (bufs[i].fd < 0) {
            goto fail;
        }
        bufs[i].len = sizeimage;
        bufs[i].addr = mmap(NULL, sizeimage, PROT_READ | PROT_WRITE,
                            MAP_SHARED, bufs[i].fd, 0);
        if (bufs[i].addr == MAP_FAILED) {
            fprintf(stderr, "mmap(dmabuf idx=%u) failed: %s\n", i, strerror(errno));
            bufs[i].addr = NULL;
            goto fail;
        }
    }

    *out_bufs = bufs;
    return 0;

fail:
    for (i = 0; i < count; i++) {
        if (bufs[i].addr) {
            munmap(bufs[i].addr, bufs[i].len);
        }
        if (bufs[i].fd >= 0) {
            close(bufs[i].fd);
        }
    }
    free(bufs);
    return -1;
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
    free(bufs);
}

int main(int argc, char **argv)
{
    struct receiver_cfg cfg;
    struct stream_hello hello_net;
    struct stream_hello hello;
    struct buffer_ctx *bufs = NULL;
    struct timeval start_tv = {0};
    struct timeval now_tv;
    uint64_t frames = 0;
    int sock = -1;
    int out_fd = -1;
    int rc = 1;

    if (parse_args(argc, argv, &cfg) < 0) {
        return 1;
    }

    sock = tcp_listen_and_accept(cfg.bind_ip, cfg.port);
    if (sock < 0) {
        return 1;
    }

    if (recv_all(sock, &hello_net, sizeof(hello_net)) < 0) {
        fprintf(stderr, "failed to receive stream header\n");
        goto out;
    }

    net_to_host_hello(&hello, &hello_net);

    if (hello.magic != STREAM_MAGIC || hello.version != PROTO_VERSION) {
        fprintf(stderr, "protocol mismatch: magic=0x%x version=%u\n",
                hello.magic, hello.version);
        goto out;
    }
    if (hello.buffer_count == 0 || hello.sizeimage == 0) {
        fprintf(stderr, "invalid stream params: buffers=%u sizeimage=%u\n",
                hello.buffer_count, hello.sizeimage);
        goto out;
    }

    if (setup_dmabufs(&cfg, &bufs, hello.buffer_count, hello.sizeimage) < 0) {
        fprintf(stderr, "failed to allocate receiver dmabufs\n");
        goto out;
    }

    if (cfg.output_path) {
        out_fd = open(cfg.output_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
        if (out_fd < 0) {
            fprintf(stderr, "failed to open output %s: %s\n",
                    cfg.output_path, strerror(errno));
            goto out;
        }
    }

    {
        char fourcc[5];
        fourcc_to_text(hello.pixelformat, fourcc);
        fprintf(stderr,
                "receiving %ux%u %s, sizeimage=%u, buffers=%u\n",
                hello.width, hello.height, fourcc,
                hello.sizeimage, hello.buffer_count);
    }

    gettimeofday(&start_tv, NULL);

    for (;;) {
        struct frame_packet pkt_net;
        struct frame_packet pkt;
        uint32_t idx;

        if (recv_all(sock, &pkt_net, sizeof(pkt_net)) < 0) {
            fprintf(stderr, "stream closed\n");
            break;
        }
        net_to_host_frame(&pkt, &pkt_net);

        if (pkt.magic != FRAME_MAGIC) {
            fprintf(stderr, "invalid frame magic: 0x%x\n", pkt.magic);
            goto out;
        }

        if (pkt.bytesused > hello.sizeimage) {
            fprintf(stderr, "invalid bytesused=%u > sizeimage=%u\n",
                    pkt.bytesused, hello.sizeimage);
            goto out;
        }

        idx = pkt.index % hello.buffer_count;

        if (recv_all(sock, bufs[idx].addr, pkt.bytesused) < 0) {
            fprintf(stderr, "failed to receive frame payload\n");
            goto out;
        }

        if (out_fd >= 0) {
            ssize_t wr = write(out_fd, bufs[idx].addr, pkt.bytesused);
            if (wr < 0 || (uint32_t)wr != pkt.bytesused) {
                fprintf(stderr, "write output failed: %s\n", strerror(errno));
                goto out;
            }
        }

        frames++;
        gettimeofday(&now_tv, NULL);
        if (now_tv.tv_sec > start_tv.tv_sec ||
            (now_tv.tv_sec == start_tv.tv_sec &&
             now_tv.tv_usec - start_tv.tv_usec >= 1000000)) {
            double elapsed = (now_tv.tv_sec - start_tv.tv_sec) +
                             (now_tv.tv_usec - start_tv.tv_usec) / 1000000.0;
            fprintf(stderr, "receiver fps: %.2f (%" PRIu64 " frames)\n", frames / elapsed, frames);
        }
    }

    rc = 0;
out:
    if (out_fd >= 0) {
        close(out_fd);
    }
    cleanup_dmabufs(bufs, hello.buffer_count);
    if (sock >= 0) {
        close(sock);
    }
    return rc;
}
