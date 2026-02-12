# v4l-dmabuf-example

This repository contains two different sender/receiver flows.

- Payload-copy flow (`sender`, `receiver`, `receiver_sdl`): sends frame bytes over TCP.
- Handle-based zero-copy flow (`sender_zc`, `receiver_zc`): sends only control packets over TCP and shares frame buffers through virtio-media export/import handles.

Both flows are intended for two separate guests:

- Guest A captures video from `/dev/videoX`.
- Guest B receives frames.

## Build

```bash
make
```

Builds binaries:

- `sender`
- `receiver`
- `sender_zc`
- `receiver_zc`
- `receiver_sdl` (only if `sdl2` development package is available)
- `receiver_zc_sdl` (only if `sdl2` development package is available)

## Flow 1: Payload Copy Over TCP

### What it demonstrates

- DMABUF-backed or MMAP-backed capture in Guest A.
- Frame metadata and frame payload sent over TCP/IP.
- Receiver writes payload into its local DMABUF mappings.

This is not cross-guest zero-copy.

### Receiver (Guest B)

```bash
./receiver -p 9000 -o /tmp/capture.raw
```

Options:

- `-l <ip>` listen address (default `0.0.0.0`)
- `-p <port>` listen port (required)
- `-e <heap>` DMA heap (default `/dev/dma_heap/system`)
- `-o <path>` optional raw output file

Optional live display receiver:

```bash
./receiver_sdl -p 9000
```

Supported live display formats in `receiver_sdl`: `YUYV`, `UYVY`, `YVYU`.

### Sender (Guest A)

```bash
./sender -d /dev/video0 -a 10.0.0.2 -p 9000 -W 640 -H 480 -f YUYV -b 4
```

Options:

- `-d <dev>` capture device (default `/dev/video0`)
- `-a <ip>` receiver IP (required)
- `-p <port>` receiver port (required)
- `-W <width>` capture width (default `640`)
- `-H <height>` capture height (default `480`)
- `-f <fourcc>` pixel format as 4 chars (default `YUYV`)
- `-b <count>` buffer count (default `4`)
- `-n <frames>` stop after N frames (default unlimited)
- `-e <heap>` DMA heap (default `/dev/dma_heap/system`)
- `-m <mode>` capture memory mode: `auto` (default), `dmabuf`, `mmap`

`auto` first tries `V4L2_MEMORY_DMABUF` and falls back to `V4L2_MEMORY_MMAP`
if DMABUF is rejected during `REQBUFS` or `QBUF`.

### Quick validation

```bash
ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 /tmp/capture.raw
```

Adjust format/size to match sender settings.

## Flow 2: Handle-Based Zero-Copy (virtio-media extension)

### What it demonstrates

- Guest A captures with `V4L2_MEMORY_MMAP`.
- Guest A exports queue buffers via private virtio-media ioctl (`VIDIOC_VIRTIO_MEDIA_EXPORT_BUFFER`).
- Sender resolves exported handles into Xen grant metadata and sends one setup
  control packet per handle over TCP.
- Guest B imports with private ioctl `VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER` using
  direct-gref mode (`VIRTIO_MEDIA_IMPORT_F_DIRECT_GREFS`), so it does not rely
  on local QEMU handle lookup.
- Per-frame packets then carry only metadata + `handle_id` over TCP.
- Guest B maps imported DMABUFs and reads frames directly from shared memory.

No frame payload bytes are sent over TCP in this flow.

### Requirements

- Out-of-tree virtio-media driver/QEMU implementation with export/import support.
- Private ioctls from `virtio_media_uapi.h` available in the running guest driver.
- Both guests connected to compatible virtio-media backend setup.
- Sender and receiver binaries from the same `PROTO_VERSION`.

### Receiver (Guest B)

```bash
./receiver_zc -d /dev/video0 -p 3344 -o /tmp/capture-zc.raw
```

Options:

- `-d <dev>` virtio-media device used for import ioctls (default `/dev/video0`)
- `-l <ip>` listen address (default `0.0.0.0`)
- `-p <port>` listen port (required)
- `-o <path>` optional raw output file for verification
- `-n <frames>` stop after N frames (default unlimited)

Live display variant:

```bash
./receiver_zc_sdl -d /dev/video0 -p 3344
```

`receiver_zc_sdl` imports handles exactly like `receiver_zc`, but displays
imported frames with SDL instead of only optional file output.

### Sender (Guest A)

```bash
./sender_zc -d /dev/video0 -a 10.0.3.16 -p 3344 -W 640 -H 480 -f YUYV -b 4
```

Options:

- `-d <dev>` capture device (default `/dev/video0`)
- `-a <ip>` receiver IP (required)
- `-p <port>` receiver port (required)
- `-W <width>` capture width (default `640`)
- `-H <height>` capture height (default `480`)
- `-f <fourcc>` pixel format as 4 chars (default `YUYV`)
- `-b <count>` MMAP buffer count (default `4`)
- `-n <frames>` stop after N frames (default unlimited)
- `-k <ms>` ACK wait timeout in ms (default `200`)
- `-t <ms>` TCP connect timeout in ms (default `3000`)
- `-D <domid>` receiver guest Xen domid (required for cross-guest grants)

### Synchronization model

`sender_zc` waits for one ACK from `receiver_zc` per frame before requeueing the
capture buffer. This is a simple ownership protocol for demos where explicit
cross-guest fence support is not available.

If ACK packets are delayed or missing, `sender_zc` times out after `-k` and
continues (with a warning) so capture does not stall permanently.

## SDL/DRM and virtio-gpu

`receiver_sdl` can exercise `virtio-gpu` in the receiver guest, depending on
SDL backend selection:

- `SDL_VIDEODRIVER=kmsdrm` uses DRM/KMS directly and typically hits guest `virtio-gpu`.
- `SDL_VIDEODRIVER=x11` or `wayland` is usually indirect via compositor/display server.
