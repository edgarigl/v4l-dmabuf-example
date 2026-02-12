#include "common.h"

#include <SDL2/SDL.h>

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <unistd.h>

struct buffer_ctx {
    int fd;
    void *addr;
    size_t len;
};

struct receiver_cfg {
    const char *bind_ip;
    const char *heap;
    uint16_t port;
};

struct display_ctx {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_Texture *texture;
    int pitch;
};

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s -p <port> [options]\n"
            "Options:\n"
            "  -l <ip>       Listen IPv4 address (default 0.0.0.0)\n"
            "  -p <port>     Listen port (required)\n"
            "  -e <heap>     dma-heap device (default /dev/dma_heap/system)\n",
            prog);
}

static int parse_args(int argc, char **argv, struct receiver_cfg *cfg)
{
    int c;

    *cfg = (struct receiver_cfg){
        .bind_ip = "0.0.0.0",
        .heap = "/dev/dma_heap/system",
        .port = 0,
    };

    while ((c = getopt(argc, argv, "l:p:e:h")) != -1) {
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

    /* Same data path as receiver.c: maintain local DMABUF backing storage. */
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

static int map_fourcc_to_sdl(uint32_t fourcc, uint32_t *sdl_fmt, int *pitch, uint32_t width)
{
    /*
     * Map V4L2 packed-YUV formats to SDL texture formats. We keep this narrow
     * for clarity; unsupported formats fail fast with a clear error.
     */
    switch (fourcc) {
    case V4L2_PIX_FMT_YUYV:
        *sdl_fmt = SDL_PIXELFORMAT_YUY2;
        *pitch = (int)width * 2;
        return 0;
    case V4L2_PIX_FMT_UYVY:
        *sdl_fmt = SDL_PIXELFORMAT_UYVY;
        *pitch = (int)width * 2;
        return 0;
    case V4L2_PIX_FMT_YVYU:
        *sdl_fmt = SDL_PIXELFORMAT_YVYU;
        *pitch = (int)width * 2;
        return 0;
    default:
        return -1;
    }
}

static int display_init(struct display_ctx *disp, const struct stream_hello *hello)
{
    uint32_t sdl_fmt;

    memset(disp, 0, sizeof(*disp));

    if (map_fourcc_to_sdl(hello->pixelformat, &sdl_fmt, &disp->pitch, hello->width) < 0) {
        char fourcc[5];
        fourcc_to_text(hello->pixelformat, fourcc);
        fprintf(stderr, "unsupported pixel format for SDL display: %s\n", fourcc);
        return -1;
    }

    /* Create window/renderer/texture once, then update texture per frame. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    disp->window = SDL_CreateWindow("v4l-dmabuf receiver",
                                    SDL_WINDOWPOS_CENTERED,
                                    SDL_WINDOWPOS_CENTERED,
                                    (int)hello->width,
                                    (int)hello->height,
                                    0);
    if (!disp->window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        return -1;
    }

    disp->renderer = SDL_CreateRenderer(disp->window, -1,
                                        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!disp->renderer) {
        disp->renderer = SDL_CreateRenderer(disp->window, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!disp->renderer) {
        fprintf(stderr, "SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return -1;
    }

    disp->texture = SDL_CreateTexture(disp->renderer,
                                      sdl_fmt,
                                      SDL_TEXTUREACCESS_STREAMING,
                                      (int)hello->width,
                                      (int)hello->height);
    if (!disp->texture) {
        fprintf(stderr, "SDL_CreateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    return 0;
}

static void display_destroy(struct display_ctx *disp)
{
    if (disp->texture) {
        SDL_DestroyTexture(disp->texture);
    }
    if (disp->renderer) {
        SDL_DestroyRenderer(disp->renderer);
    }
    if (disp->window) {
        SDL_DestroyWindow(disp->window);
    }
    SDL_Quit();
}

static int display_frame(struct display_ctx *disp, const void *data)
{
    /* Upload one full frame from mapped DMABUF memory and present it. */
    if (SDL_UpdateTexture(disp->texture, NULL, data, disp->pitch) != 0) {
        fprintf(stderr, "SDL_UpdateTexture failed: %s\n", SDL_GetError());
        return -1;
    }

    if (SDL_RenderClear(disp->renderer) != 0) {
        fprintf(stderr, "SDL_RenderClear failed: %s\n", SDL_GetError());
        return -1;
    }

    if (SDL_RenderCopy(disp->renderer, disp->texture, NULL, NULL) != 0) {
        fprintf(stderr, "SDL_RenderCopy failed: %s\n", SDL_GetError());
        return -1;
    }

    SDL_RenderPresent(disp->renderer);
    return 0;
}

int main(int argc, char **argv)
{
    struct receiver_cfg cfg;
    struct stream_hello hello_net;
    struct stream_hello hello;
    struct buffer_ctx *bufs = NULL;
    struct display_ctx disp;
    struct timeval start_tv = {0};
    struct timeval now_tv;
    uint64_t frames = 0;
    int sock = -1;
    int rc = 1;
    bool display_ok = false;

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

    /* Parse sender stream description before creating SDL objects. */
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

    if (display_init(&disp, &hello) < 0) {
        goto out;
    }
    display_ok = true;

    {
        char fourcc[5];
        fourcc_to_text(hello.pixelformat, fourcc);
        fprintf(stderr,
                "displaying %ux%u %s, sizeimage=%u, buffers=%u\n",
                hello.width, hello.height, fourcc,
                hello.sizeimage, hello.buffer_count);
    }

    gettimeofday(&start_tv, NULL);

    for (;;) {
        struct frame_packet pkt_net;
        struct frame_packet pkt;
        uint32_t idx;
        SDL_Event ev;

        /* Keep window responsive while waiting for network frames. */
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                rc = 0;
                goto out;
            }
        }

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

        /* Receive payload into DMABUF mapping, then draw it via SDL. */
        if (recv_all(sock, bufs[idx].addr, pkt.bytesused) < 0) {
            fprintf(stderr, "failed to receive frame payload\n");
            goto out;
        }

        if (display_frame(&disp, bufs[idx].addr) < 0) {
            goto out;
        }

        frames++;
        gettimeofday(&now_tv, NULL);
        if (now_tv.tv_sec > start_tv.tv_sec ||
            (now_tv.tv_sec == start_tv.tv_sec &&
             now_tv.tv_usec - start_tv.tv_usec >= 1000000)) {
            double elapsed = (now_tv.tv_sec - start_tv.tv_sec) +
                             (now_tv.tv_usec - start_tv.tv_usec) / 1000000.0;
            fprintf(stderr, "receiver_sdl fps: %.2f (%" PRIu64 " frames)\n", frames / elapsed, frames);
        }
    }

    rc = 0;
out:
    if (display_ok) {
        display_destroy(&disp);
    }
    cleanup_dmabufs(bufs, hello.buffer_count);
    if (sock >= 0) {
        close(sock);
    }
    return rc;
}
