#include "common.h"
#include "virtio_media_uapi.h"

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
    /* handle_id is stable across frames and maps to one imported dmabuf. */
    uint64_t handle_id;
    int dmabuf_fd;
    void *addr;
    size_t len;
    bool needs_release;
};

struct receiver_cfg {
    const char *device;
    const char *bind_ip;
    const char *output_path;
    uint16_t port;
    int frame_limit;
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

static int import_handle_direct(int vfd, const struct zc_handle_packet *hp,
                                struct import_entry *entry)
{
    struct virtio_media_ioc_import_buffer imp;
    uint32_t i;

    memset(&imp, 0, sizeof(imp));
    imp.handle_id = hp->handle_id;
    imp.flags = VIRTIO_MEDIA_IMPORT_F_DIRECT_GREFS;
    imp.gref_count = hp->gref_count;
    imp.gref_page_size = hp->gref_page_size;
    imp.gref_domid = hp->gref_domid;
    imp.len = hp->len;
    for (i = 0; i < hp->gref_count && i < VIRTIO_MEDIA_MAX_IMPORT_GREFS; i++) {
        imp.gref_ids[i] = hp->gref_ids[i];
    }

    /*
     * Direct-gref import avoids local QEMU handle lookup. This is required
     * when sender and receiver are different guests (different QEMU instances).
     */
    if (xioctl(vfd, VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER, &imp) < 0) {
        fprintf(stderr, "VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER(handle=%" PRIu64 ") failed: %s\n",
                hp->handle_id, strerror(errno));
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

    entry->handle_id = hp->handle_id;
    entry->dmabuf_fd = imp.dmabuf_fd;
    entry->len = (size_t)imp.len;
    entry->needs_release = false;

    return 0;
}

static int append_entry(struct import_entry **entries,
                        size_t *count,
                        size_t *capacity,
                        struct import_entry **out)
{
    struct import_entry *entry;

    /* Grow a tiny cache so we can import all announced handles up front. */
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

        if (entries[i].needs_release) {
            memset(&rel, 0, sizeof(rel));
            rel.handle_id = entries[i].handle_id;
            xioctl(vfd, VIDIOC_VIRTIO_MEDIA_RELEASE_HANDLE, &rel);
        }
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
    struct timeval start_tv = {0};
    struct timeval now_tv;
    uint64_t frames = 0;
    int vfd = -1;
    int sock = -1;
    int out_fd = -1;
    int rc = 1;

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

    /*
     * Import all shared handles advertised by the sender before reading frame
     * packets. This gives deterministic failure points and avoids per-frame
     * control path stalls.
     */
    {
        uint32_t i;

        for (i = 0; i < hello.buffer_count; i++) {
            struct zc_handle_packet hp_net;
            struct zc_handle_packet hp;
            struct import_entry *entry;

            if (recv_all(sock, &hp_net, sizeof(hp_net)) < 0) {
                fprintf(stderr, "failed to receive handle metadata packet\n");
                goto out;
            }
            net_to_host_zc_handle(&hp, &hp_net);

            if (hp.magic != ZC_HANDLE_MAGIC ||
                !(hp.flags & VIRTIO_MEDIA_IMPORT_F_DIRECT_GREFS) ||
                hp.gref_count == 0 ||
                hp.gref_count > VIRTIO_MEDIA_MAX_IMPORT_GREFS ||
                hp.gref_page_size == 0 || hp.len == 0) {
                fprintf(stderr,
                        "invalid handle packet magic=0x%x flags=0x%x grefs=%u len=%" PRIu64 "\n",
                        hp.magic, hp.flags, hp.gref_count, (uint64_t)hp.len);
                goto out;
            }

            if (find_entry(entries, entry_count, hp.handle_id)) {
                fprintf(stderr, "duplicate handle metadata for %" PRIu64 "\n",
                        hp.handle_id);
                goto out;
            }

            if (append_entry(&entries, &entry_count, &entry_capacity, &entry) < 0) {
                goto out;
            }
            if (import_handle_direct(vfd, &hp, entry) < 0) {
                goto out;
            }
            entry_count += 1;
        }
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
                "zero-copy receiver: %s listening on %s:%u (%ux%u %s)\n",
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

        if (recv_all(sock, &pkt_net, sizeof(pkt_net)) < 0) {
            fprintf(stderr, "stream closed\n");
            break;
        }

        net_to_host_zc_frame(&pkt, &pkt_net);

        if (pkt.magic != FRAME_MAGIC) {
            fprintf(stderr, "invalid frame magic: 0x%x\n", pkt.magic);
            goto out;
        }

        entry = find_entry(entries, entry_count, pkt.handle_id);
        if (!entry) {
            fprintf(stderr, "unknown handle in frame packet: %" PRIu64 "\n",
                    pkt.handle_id);
            goto out;
        }

        if (pkt.bytesused > entry->len) {
            fprintf(stderr,
                    "frame bytesused too large: %u > %zu (handle=%" PRIu64 ")\n",
                    pkt.bytesused, entry->len, pkt.handle_id);
            goto out;
        }

        /*
         * Zero-copy path: payload is already in shared memory mapped by import.
         * We only optionally copy out to a file for verification.
         */
        if (out_fd >= 0) {
            ssize_t wr = write(out_fd, entry->addr, pkt.bytesused);
            if (wr < 0 || (uint32_t)wr != pkt.bytesused) {
                fprintf(stderr, "write output failed: %s\n", strerror(errno));
                goto out;
            }
        }

        /*
         * ACK is the ownership hand-off back to sender for this frame.
         * Sender requeues only after receiving this packet.
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
                    "receiver_zc fps: %.2f (%" PRIu64 " frames, %zu handles imported)\n",
                    frames / elapsed, frames, entry_count);
        }
    }

    rc = 0;
out:
    if (out_fd >= 0) {
        close(out_fd);
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
