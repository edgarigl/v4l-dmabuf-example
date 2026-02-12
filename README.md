# v4l-dmabuf-example

Minimal sender/receiver example that demonstrates:

- Allocating capture buffers as DMABUFs (`/dev/dma_heap/system` by default).
- Capturing video from a V4L2 device with `V4L2_MEMORY_DMABUF`.
- Sending frame metadata and frame payload over a TCP/IP socket to another guest.
- Reconstructing received frames into DMABUFs on the receiver side.

This is meant for two separate guests:

- Guest A runs `sender` and captures from `/dev/videoX`.
- Guest B runs `receiver` and accepts frames over TCP.

## Important note

A DMABUF file descriptor itself cannot be transferred over TCP/IP. This example
uses TCP as a transport for:

- Per-stream metadata (format, dimensions, buffer count).
- Per-frame metadata (buffer index, bytesused, sequence, timestamp).
- Raw frame bytes copied from sender DMABUF mapping into receiver DMABUF mapping.

So this is not cross-guest zero-copy. It is a practical transport pattern using
DMABUF-backed buffers at both ends.

## Build

```bash
make
```

Builds binaries:

- `sender`
- `receiver`
- `receiver_sdl` (only if `sdl2` development package is available)

## Usage

### 1. Start receiver on Guest B

```bash
./receiver -p 9000 -o /tmp/capture.raw
```

Options:

- `-l <ip>` listen address (default `0.0.0.0`)
- `-p <port>` listen port (required)
- `-e <heap>` DMA heap (default `/dev/dma_heap/system`)
- `-o <path>` optional output raw dump

### 1b. Start live SDL receiver on Guest B (optional)

```bash
./receiver_sdl -p 9000
```

Options:

- `-l <ip>` listen address (default `0.0.0.0`)
- `-p <port>` listen port (required)
- `-e <heap>` DMA heap (default `/dev/dma_heap/system`)

Supported live display formats in `receiver_sdl`: `YUYV`, `UYVY`, `YVYU`.

### 2. Start sender on Guest A

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
- `-b <count>` DMABUF count (default `4`)
- `-n <frames>` stop after N frames (default unlimited)
- `-e <heap>` DMA heap (default `/dev/dma_heap/system`)
- `-m <mode>` capture memory mode: `auto` (default), `dmabuf`, `mmap`

`auto` first tries `V4L2_MEMORY_DMABUF` and falls back to `V4L2_MEMORY_MMAP`
if the driver rejects DMABUF `REQBUFS`.

## Quick validation

After capture, on receiver you can inspect the raw dump with ffplay:

```bash
ffplay -f rawvideo -pixel_format yuyv422 -video_size 640x480 /tmp/capture.raw
```

Adjust `pixel_format` and `video_size` to match the sender settings.

## How this maps to virtio-media experiments

This example demonstrates user space behavior only. It does not require private
virtio-media ioctls and does not move grant references. It is useful as a
baseline for comparing capture path behavior across KVM/Xen and for validating
that the application-level transport logic is sound.

If your capture node fails with:

- `VIDIOC_REQBUFS(DMABUF) failed: Invalid argument`

run sender in fallback mode:

```bash
./sender -d /dev/video0 -a <receiver-ip> -p 9000 -m mmap
```

## SDL/DRM and virtio-gpu

`receiver_sdl` can exercise `virtio-gpu` in the receiver guest, but it depends
on SDL backend selection:

- `SDL_VIDEODRIVER=kmsdrm` uses DRM/KMS directly and hits the guest DRM device
  (typically `virtio-gpu` when that is your virtual GPU).
- `SDL_VIDEODRIVER=x11` or `wayland` goes through a compositor/display server;
  this is usually indirect from the app point of view.
