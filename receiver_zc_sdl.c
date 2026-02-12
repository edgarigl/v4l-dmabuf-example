#include "common.h"
#include "virtio_media_uapi.h"

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

struct import_entry {
    uint64_t handle_id;
    int dmabuf_fd;
    void *addr;
    size_t len;
};

struct receiver_cfg {
    const char *device;
    const char *bind_ip;
    const char *output_path;
    uint16_t port;
    int frame_limit;
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
            "  -d <dev>      virtio-media device for import ioctls (default /dev/video0)\n"
            "  -l <ip>       Listen IPv4 address (default 0.0.0.0)\n"
            "  -p <port>     Listen port (required)\n"
            "  -o <path>     Optional raw output file\n"
            "  -n <frames>   Stop after N frames (default: unlimited)\n",
            prog);
}

static int parse_args(int argc, char **argv, struct receiver_cfg *cfg)
{
    int c;

    *cfg = (struct receiver_cfg){
        .device = "/dev/video0",
        .bind_ip = "0.0.0.0",
        .output_path = NULL,
        .port = 0,
        .frame_limit = -1,
    };

    while ((c = getopt(argc, argv, "d:l:p:o:n:h")) != -1) {
        switch (c) {
        case 'd':
            cfg->device = optarg;
            break;
        case 'l':
            cfg->bind_ip = optarg;
            break;
        case 'p':
            cfg->port = (uint16_t)strtoul(optarg, NULL, 10);
            break;
        case 'o':
            cfg->output_path = optarg;
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

    if (cfg->port == 0) {
        usage(argv[0]);
        return -1;
    }

    return 0;
}

static int map_fourcc_to_sdl(uint32_t fourcc, uint32_t *sdl_fmt,
                             int *pitch, uint32_t width)
{
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

    if (map_fourcc_to_sdl(hello->pixelformat, &sdl_fmt, &disp->pitch,
                          hello->width) < 0) {
        char fourcc[5];
        fourcc_to_text(hello->pixelformat, fourcc);
        fprintf(stderr, "unsupported pixel format for SDL display: %s\n", fourcc);
        return -1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return -1;
    }

    disp->window = SDL_CreateWindow("v4l-dmabuf receiver_zc_sdl",
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
                                        SDL_RENDERER_ACCELERATED |
                                            SDL_RENDERER_PRESENTVSYNC);
    if (!disp->renderer) {
        disp->renderer = SDL_CreateRenderer(disp->window, -1,
                                            SDL_RENDERER_SOFTWARE);
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

static int display_frame(struct display_ctx *disp, const void *pixels)
{
    if (SDL_UpdateTexture(disp->texture, NULL, pixels, disp->pitch) != 0) {
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

static struct import_entry *find_entry(struct import_entry *entries,
                                       size_t count,
                                       uint64_t handle_id)
{
    size_t i;

    for (i = 0; i < count; i++) {
        if (entries[i].handle_id == handle_id) {
            return &entries[i];
        }
    }

    return NULL;
}

static int import_handle(int vfd, uint64_t handle_id, struct import_entry *entry)
{
    struct virtio_media_ioc_import_buffer imp;

    memset(&imp, 0, sizeof(imp));
    imp.handle_id = handle_id;

    if (xioctl(vfd, VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER, &imp) < 0) {
        fprintf(stderr,
                "VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER(handle=%" PRIu64 ") failed: %s\n",
                handle_id, strerror(errno));
        return -1;
    }

    if (imp.dmabuf_fd < 0 || imp.len == 0) {
        fprintf(stderr,
                "import returned invalid dmabuf_fd=%d len=%" PRIu64 "\n",
                imp.dmabuf_fd, (uint64_t)imp.len);
        if (imp.dmabuf_fd >= 0) {
            close(imp.dmabuf_fd);
        }
        return -1;
    }

    entry->addr = mmap(NULL, (size_t)imp.len, PROT_READ | PROT_WRITE,
                       MAP_SHARED, imp.dmabuf_fd, 0);
    if (entry->addr == MAP_FAILED) {
        fprintf(stderr, "mmap(imported dmabuf) failed: %s\n", strerror(errno));
        close(imp.dmabuf_fd);
        return -1;
    }

    entry->handle_id = handle_id;
    entry->dmabuf_fd = imp.dmabuf_fd;
    entry->len = (size_t)imp.len;

    return 0;
}

static int ensure_entry(int vfd,
                        struct import_entry **entries,
                        size_t *count,
                        size_t *capacity,
                        uint64_t handle_id,
                        struct import_entry **out)
{
    struct import_entry *entry;

    entry = find_entry(*entries, *count, handle_id);
    if (entry) {
        *out = entry;
        return 0;
    }

    if (*count == *capacity) {
        size_t new_cap = (*capacity == 0) ? 8 : (*capacity * 2);
        struct import_entry *new_entries = realloc(*entries,
                                                   new_cap * sizeof(**entries));
        if (!new_entries) {
            return -1;
        }
        *entries = new_entries;
        *capacity = new_cap;
    }

    entry = &(*entries)[*count];
    memset(entry, 0, sizeof(*entry));
    entry->dmabuf_fd = -1;

    if (import_handle(vfd, handle_id, entry) < 0) {
        return -1;
    }

    *count += 1;
    *out = entry;
    return 0;
}

static void cleanup_entries(int vfd, struct import_entry *entries, size_t count)
{
    size_t i;

    if (!entries) {
        return;
    }

    for (i = 0; i < count; i++) {
        struct virtio_media_ioc_release_handle rel;

        if (entries[i].addr && entries[i].len) {
            munmap(entries[i].addr, entries[i].len);
        }
        if (entries[i].dmabuf_fd >= 0) {
            close(entries[i].dmabuf_fd);
        }

        memset(&rel, 0, sizeof(rel));
        rel.handle_id = entries[i].handle_id;
        xioctl(vfd, VIDIOC_VIRTIO_MEDIA_RELEASE_HANDLE, &rel);
    }

    free(entries);
}

int main(int argc, char **argv)
{
    struct receiver_cfg cfg;
    struct stream_hello hello_net;
    struct stream_hello hello;
    struct import_entry *entries = NULL;
    size_t entry_count = 0;
    size_t entry_capacity = 0;
    struct display_ctx disp;
    struct timeval start_tv = {0};
    struct timeval now_tv;
    uint64_t frames = 0;
    int vfd = -1;
    int sock = -1;
    int out_fd = -1;
    int rc = 1;
    bool display_ok = false;

    if (parse_args(argc, argv, &cfg) < 0) {
        return 1;
    }

    vfd = open_v4l2_device(cfg.device);
    if (vfd < 0) {
        return 1;
    }

    sock = tcp_listen_and_accept(cfg.bind_ip, cfg.port);
    if (sock < 0) {
        goto out;
    }

    if (recv_all(sock, &hello_net, sizeof(hello_net)) < 0) {
        fprintf(stderr, "failed to receive stream header\n");
        goto out;
    }

    net_to_host_hello(&hello, &hello_net);
    if (hello.magic != STREAM_MAGIC || hello.version != PROTO_VERSION) {
        fprintf(stderr, "protocol mismatch magic=0x%x version=%u\n",
                hello.magic, hello.version);
        goto out;
    }

    if (display_init(&disp, &hello) < 0) {
        goto out;
    }
    display_ok = true;

    if (cfg.output_path) {
        out_fd = open(cfg.output_path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC,
                      0644);
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
                "zero-copy receiver SDL: %s listening on %s:%u (%ux%u %s)\n",
                cfg.device, cfg.bind_ip, cfg.port,
                hello.width, hello.height, fourcc);
    }

    gettimeofday(&start_tv, NULL);

    while (cfg.frame_limit < 0 || (int)frames < cfg.frame_limit) {
        struct zc_frame_packet pkt_net;
        struct zc_frame_packet pkt;
        struct zc_ack_packet ack;
        struct zc_ack_packet ack_net;
        struct import_entry *entry;
        SDL_Event ev;

        /* Keep the window responsive while waiting for new frame metadata. */
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

        net_to_host_zc_frame(&pkt, &pkt_net);

        if (pkt.magic != FRAME_MAGIC) {
            fprintf(stderr, "invalid frame magic: 0x%x\n", pkt.magic);
            goto out;
        }

        if (ensure_entry(vfd, &entries, &entry_count, &entry_capacity,
                         pkt.handle_id, &entry) < 0) {
            goto out;
        }

        if (pkt.bytesused > entry->len) {
            fprintf(stderr,
                    "frame bytesused too large: %u > %zu (handle=%" PRIu64 ")\n",
                    pkt.bytesused, entry->len, pkt.handle_id);
            goto out;
        }

        /*
         * Zero-copy display: pixels are read directly from imported shared
         * memory; no frame payload traverses TCP.
         */
        if (display_frame(&disp, entry->addr) < 0) {
            goto out;
        }

        if (out_fd >= 0) {
            ssize_t wr = write(out_fd, entry->addr, pkt.bytesused);
            if (wr < 0 || (uint32_t)wr != pkt.bytesused) {
                fprintf(stderr, "write output failed: %s\n", strerror(errno));
                goto out;
            }
        }

        /*
         * ACK indicates that receiver finished reading this frame and sender
         * can safely requeue the originating capture buffer.
         */
        ack.magic = FRAME_MAGIC;
        ack.status = 0;
        ack.handle_id = pkt.handle_id;
        ack.sequence = pkt.sequence;
        host_to_net_zc_ack(&ack_net, &ack);
        if (send_all(sock, &ack_net, sizeof(ack_net)) < 0) {
            fprintf(stderr, "failed to send ACK\n");
            goto out;
        }

        frames++;
        gettimeofday(&now_tv, NULL);
        if (now_tv.tv_sec > start_tv.tv_sec ||
            (now_tv.tv_sec == start_tv.tv_sec &&
             now_tv.tv_usec - start_tv.tv_usec >= 1000000)) {
            double elapsed = (now_tv.tv_sec - start_tv.tv_sec) +
                             (now_tv.tv_usec - start_tv.tv_usec) / 1000000.0;
            fprintf(stderr,
                    "receiver_zc_sdl fps: %.2f (%" PRIu64 " frames, %zu handles imported)\n",
                    frames / elapsed, frames, entry_count);
        }
    }

    rc = 0;
out:
    if (out_fd >= 0) {
        close(out_fd);
    }
    if (display_ok) {
        display_destroy(&disp);
    }
    cleanup_entries(vfd, entries, entry_count);
    if (sock >= 0) {
        close(sock);
    }
    if (vfd >= 0) {
        close(vfd);
    }

    return rc;
}
