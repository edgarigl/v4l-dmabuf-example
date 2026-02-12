#ifndef V4L_DMABUF_VIRTIO_MEDIA_UAPI_H
#define V4L_DMABUF_VIRTIO_MEDIA_UAPI_H

#include <linux/videodev2.h>
#include <sys/ioctl.h>

/*
 * Private ioctl ABI used by the out-of-tree virtio-media driver.
 * Keep this header local so the example can be built standalone.
 */

#define VIRTIO_MEDIA_MAX_IMPORT_GREFS 4000

struct virtio_media_ioc_export_buffer {
    __u32 queue_type;
    __u32 buffer_index;
    __u32 plane_index;
    __u32 flags;
    __u64 handle_id;
    __u64 len;
    __u32 plane_count;
    __s32 dmabuf_fd;
};

struct virtio_media_ioc_import_buffer {
    __u64 handle_id;
    __u32 flags;
    __u32 gref_count;
    __u32 gref_page_size;
    __u32 gref_domid;
    __u32 __reserved;
    __u64 driver_addr;
    __u64 len;
    __s32 dmabuf_fd;
    __u32 __pad;
    __u32 gref_ids[VIRTIO_MEDIA_MAX_IMPORT_GREFS];
};

#define VIRTIO_MEDIA_IMPORT_F_DIRECT_GREFS (1U << 0)
#define VIRTIO_MEDIA_IMPORT_F_TARGET_DOMID (1U << 1)
#define VIRTIO_MEDIA_IMPORT_DOMID_SHIFT 16
#define VIRTIO_MEDIA_IMPORT_DOMID_MASK 0xffffU

struct virtio_media_ioc_release_handle {
    __u64 handle_id;
};

#define VIDIOC_VIRTIO_MEDIA_EXPORT_BUFFER \
    _IOWR('V', BASE_VIDIOC_PRIVATE + 0, struct virtio_media_ioc_export_buffer)
#define VIDIOC_VIRTIO_MEDIA_IMPORT_BUFFER \
    _IOWR('V', BASE_VIDIOC_PRIVATE + 1, struct virtio_media_ioc_import_buffer)
#define VIDIOC_VIRTIO_MEDIA_RELEASE_HANDLE \
    _IOWR('V', BASE_VIDIOC_PRIVATE + 2, struct virtio_media_ioc_release_handle)

#endif
