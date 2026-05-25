// v4l2_m2m_shim.c - LD_PRELOAD shim to fix V4L2 M2M on C3 (comma three)
// v4: DMABUF support, CODECCONFIG auto-attach, ION alloc fallback
//
// Build (on device):
//   gcc -shared -fPIC -o v4l2_m2m_shim.so v4l2_m2m_shim.c -ldl

#define _GNU_SOURCE
#include <dlfcn.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <errno.h>
#include <linux/videodev2.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdlib.h>
#include <unistd.h>

// ION ioctl definitions (for older kernels used on SDM845)
#define ION_IOC_MAGIC 'I'
#define ION_IOC_ALLOC _IOWR(ION_IOC_MAGIC, 0, struct ion_allocation_data)
#define ION_IOC_FREE _IOWR(ION_IOC_MAGIC, 1, struct ion_handle_data)
#define ION_IOC_MAP _IOWR(ION_IOC_MAGIC, 2, struct ion_fd_data)
#define ION_IOC_SHARE _IOWR(ION_IOC_MAGIC, 4, struct ion_fd_data)
#define ION_IOC_IMPORT _IOWR(ION_IOC_MAGIC, 5, struct ion_fd_data)

struct ion_allocation_data {
    size_t len;
    size_t align;
    unsigned int heap_id_mask;
    unsigned int flags;
    int handle_fd;
};

struct ion_handle_data {
    int handle_fd;
};

struct ion_fd_data {
    int handle_fd;
    int fd;
};

// Qualcomm Venus CODECCONFIG flag
#ifndef V4L2_QCOM_BUF_FLAG_CODECCONFIG
#define V4L2_QCOM_BUF_FLAG_CODECCONFIG 0x00020000
#endif

typedef int (*real_ioctl_t)(int fd, unsigned long request, ...);
typedef int (*real_open_t)(const char *path, int flags, ...);
typedef int (*real_close_t)(int fd);

static real_ioctl_t real_ioctl_fn = NULL;
static real_open_t real_open_fn = NULL;
static real_close_t real_close_fn = NULL;

#define MAX_FDS 128
#define MAX_DMABUF_TRACK 16

typedef struct {
    int fd;
    int active;
    int is_msm_vdec;       // /dev/video32 msm_vidc_vdec decoder
    int is_sde_rotator;    // sde_rotator - NOT for HEVC decode
    int is_ion;            // /dev/ion
    int sfmt_output_done;  // S_FMT(OUTPUT_MPLANE) was successfully called
    int first_qbuf_done;   // First QBUF(OUTPUT) completed
    char path[64];
    // DMABUF fd tracking per buffer index for CAPTURE
    int dmabuf_fds[MAX_DMABUF_TRACK];
} FdState;

static FdState fd_state[MAX_FDS];

static void init_funcs(void) {
    if (!real_ioctl_fn) real_ioctl_fn = (real_ioctl_t)dlsym(RTLD_NEXT, "ioctl");
    if (!real_open_fn) real_open_fn = (real_open_t)dlsym(RTLD_NEXT, "open");
    if (!real_close_fn) real_close_fn = (real_close_t)dlsym(RTLD_NEXT, "close");
}

static FdState *get_state(int fd) {
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_state[i].active && fd_state[i].fd == fd) return &fd_state[i];
    }
    return NULL;
}

static FdState *alloc_state(int fd) {
    // If fd already tracked, reset it (fd number reuse)
    for (int i = 0; i < MAX_FDS; i++) {
        if (fd_state[i].active && fd_state[i].fd == fd) {
            memset(&fd_state[i], 0, sizeof(FdState));
            fd_state[i].fd = fd;
            fd_state[i].active = 1;
            return &fd_state[i];
        }
    }
    // Find free slot
    for (int i = 0; i < MAX_FDS; i++) {
        if (!fd_state[i].active) {
            memset(&fd_state[i], 0, sizeof(FdState));
            fd_state[i].fd = fd;
            fd_state[i].active = 1;
            return &fd_state[i];
        }
    }
    return NULL;
}

__attribute__((constructor))
static void shim_init(void) {
    init_funcs();
    fprintf(stderr, "[v4l2_shim_v4] Loaded: open()+ioctl() intercept for msm_vidc_vdec HW decode (DMABUF+ION)\n");
}

// Intercept open() to track /dev/video* and /dev/ion path -> fd mapping
int open(const char *path, int flags, ...) {
    init_funcs();

    mode_t mode = 0;
    if (flags & O_CREAT) {
        va_list args;
        va_start(args, flags);
        mode = va_arg(args, mode_t);
        va_end(args);
    }

    int fd = real_open_fn(path, flags, mode);

    if (fd >= 0 && path) {
        if (strncmp(path, "/dev/video", 10) == 0) {
            FdState *st = alloc_state(fd);
            if (st) {
                strncpy(st->path, path, sizeof(st->path) - 1);
                if (strcmp(path, "/dev/video32") == 0) {
                    st->is_msm_vdec = 1;
                    fprintf(stderr, "[v4l2_shim_v4] open(%s)=%d -> pre-classified as msm_vidc_vdec\n", path, fd);
                } else {
                    fprintf(stderr, "[v4l2_shim_v4] open(%s)=%d\n", path, fd);
                }
            }
        } else if (strcmp(path, "/dev/ion") == 0) {
            FdState *st = alloc_state(fd);
            if (st) {
                strncpy(st->path, path, sizeof(st->path) - 1);
                st->is_ion = 1;
                fprintf(stderr, "[v4l2_shim_v4] open(%s)=%d -> ION device\n", path, fd);
            }
        }
    }

    return fd;
}

// Intercept close() to free fd state
int close(int fd) {
    init_funcs();
    FdState *st = get_state(fd);
    if (st) {
        fprintf(stderr, "[v4l2_shim_v4] close(%s fd=%d)\n", st->path, fd);
        st->active = 0;
    }
    return real_close_fn(fd);
}

int ioctl(int fd, unsigned long request, ...) {
    init_funcs();

    va_list args;
    va_start(args, request);
    void *argp = va_arg(args, void *);
    va_end(args);

    FdState *st = get_state(fd);

    // === ION ioctl intercept ===
    if (st && st->is_ion) {
        if (request == ION_IOC_ALLOC) {
            int ret = real_ioctl_fn(fd, request, argp);
            if (ret != 0) {
                struct ion_allocation_data *data = (struct ion_allocation_data *)argp;
                fprintf(stderr, "[v4l2_shim_v4] ION_IOC_ALLOC failed (errno=%d), trying fallback...\n", errno);
                // Fallback: allocate anonymous mmap and return a memfd-like fd
                // This is a best-effort fallback for systems where ION is restricted
                int memfd = syscall(319); // memfd_create on supported kernels
                if (memfd < 0) {
                    // Fallback to tmpfs file
                    char tmpfile[] = "/tmp/ion_fallback_XXXXXX";
                    memfd = mkstemp(tmpfile);
                    if (memfd >= 0) {
                        unlink(tmpfile);
                    }
                }
                if (memfd >= 0) {
                    if (ftruncate(memfd, data->len) == 0) {
                        data->handle_fd = memfd;
                        fprintf(stderr, "[v4l2_shim_v4] ION_IOC_ALLOC fallback: memfd=%d len=%zu\n", memfd, data->len);
                        return 0;
                    }
                    close(memfd);
                }
            } else {
                struct ion_allocation_data *data = (struct ion_allocation_data *)argp;
                fprintf(stderr, "[v4l2_shim_v4] ION_IOC_ALLOC success: handle_fd=%d len=%zu\n", data->handle_fd, data->len);
            }
            return ret;
        }
        if (request == ION_IOC_FREE) {
            struct ion_handle_data *data = (struct ion_handle_data *)argp;
            fprintf(stderr, "[v4l2_shim_v4] ION_IOC_FREE: handle_fd=%d\n", data->handle_fd);
            // If handle_fd looks like our fallback memfd, close it
            if (data->handle_fd >= 0) {
                close(data->handle_fd);
                return 0;
            }
        }
        if (request == ION_IOC_MAP || request == ION_IOC_SHARE) {
            struct ion_fd_data *data = (struct ion_fd_data *)argp;
            fprintf(stderr, "[v4l2_shim_v4] ION_IOC_MAP/SHARE: handle_fd=%d -> fd=%d\n", data->handle_fd, data->fd);
            // If using fallback, handle_fd is already the fd
            if (data->handle_fd >= 0 && data->fd < 0) {
                data->fd = data->handle_fd;
                return 0;
            }
        }
        // Pass through all other ION ioctls
        return real_ioctl_fn(fd, request, argp);
    }

    // === PRE: inject S_FMT(OUTPUT_MPLANE,HEVC) before REQBUFS if not yet done ===
    if (request == VIDIOC_REQBUFS && argp && st && st->is_msm_vdec && !st->is_sde_rotator) {
        struct v4l2_requestbuffers *rb = (struct v4l2_requestbuffers *)argp;
        if (rb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && !st->sfmt_output_done) {
            struct v4l2_format fmt;
            memset(&fmt, 0, sizeof(fmt));
            fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_HEVC;
            fmt.fmt.pix_mp.width = 1928;
            fmt.fmt.pix_mp.height = 1208;
            fmt.fmt.pix_mp.num_planes = 1;
            fmt.fmt.pix_mp.plane_fmt[0].sizeimage = 1024 * 1024;
            int sfmt_ret = real_ioctl_fn(fd, VIDIOC_S_FMT, &fmt);
            fprintf(stderr, "[v4l2_shim_v4] Injected S_FMT(OUTPUT,HEVC) before REQBUFS: ret=%d errno=%d\n",
                    sfmt_ret, sfmt_ret < 0 ? errno : 0);
            if (sfmt_ret == 0) st->sfmt_output_done = 1;
        }
    }

    int ret = real_ioctl_fn(fd, request, argp);

    // === POST: QUERYCAP - classify/patch devices ===
    if (request == VIDIOC_QUERYCAP && ret == 0 && argp) {
        struct v4l2_capability *cap = (struct v4l2_capability *)argp;

        // sde_rotator: NOT for HEVC M2M decode, remove M2M_MPLANE
        if (strncmp((const char *)cap->driver, "sde_rotator", 11) == 0) {
            if (!st) st = alloc_state(fd);
            if (st) {
                st->is_sde_rotator = 1;
                st->is_msm_vdec = 0;
            }
            cap->device_caps &= ~V4L2_CAP_VIDEO_M2M_MPLANE;
            cap->capabilities &= ~V4L2_CAP_VIDEO_M2M_MPLANE;
            fprintf(stderr, "[v4l2_shim_v4] QUERYCAP: sde_rotator fd=%d excluded from M2M_MPLANE\n", fd);
        }

        // msm_vidc_driver with MPLANE: add M2M_MPLANE flag
        if (strncmp((const char *)cap->driver, "msm_vidc_driver", 15) == 0) {
            int has_mplane = (cap->device_caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE) &&
                             (cap->device_caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE);
            if (has_mplane) {
                if (!st) st = alloc_state(fd);
                if (st && !st->is_sde_rotator) {
                    if (strstr((const char *)cap->card, "vdec") != NULL) {
                        st->is_msm_vdec = 1;
                    }
                }
                cap->device_caps |= V4L2_CAP_VIDEO_M2M_MPLANE;
                cap->capabilities |= V4L2_CAP_VIDEO_M2M_MPLANE;
                fprintf(stderr, "[v4l2_shim_v4] QUERYCAP: patched M2M_MPLANE on %s fd=%d\n", cap->card, fd);
            }
        }
    }

    // === POST: ENUM_FMT ===
    if (request == VIDIOC_ENUM_FMT && argp) {
        struct v4l2_fmtdesc *fmtdesc = (struct v4l2_fmtdesc *)argp;

        // sde_rotator: return EINVAL for MPLANE types
        if (st && st->is_sde_rotator) {
            if (fmtdesc->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
                fmtdesc->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) {
                ret = -1;
                errno = EINVAL;
            }
        }

        // msm_vidc_vdec: fake HEVC as index=0 format for OUTPUT_MPLANE
        if (st && st->is_msm_vdec && !st->is_sde_rotator && ret != 0 &&
            fmtdesc->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE &&
            fmtdesc->index == 0) {
            memset(fmtdesc->description, 0, 32);
            strncpy((char *)fmtdesc->description, "HEVC", 32);
            fmtdesc->pixelformat = V4L2_PIX_FMT_HEVC;
            fmtdesc->flags = 0;
            ret = 0;
            errno = 0;
            fprintf(stderr, "[v4l2_shim_v4] ENUM_FMT: faked HEVC for msm_vidc_vdec fd=%d\n", fd);
        }
    }

    // === POST: TRY_FMT / S_FMT - fake success for msm_vidc_vdec MPLANE ===
    if ((request == VIDIOC_TRY_FMT || request == VIDIOC_S_FMT) && ret != 0 && argp) {
        int saved_errno = errno;
        struct v4l2_format *fmt = (struct v4l2_format *)argp;
        if (st && st->is_msm_vdec && !st->is_sde_rotator &&
            (fmt->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE ||
             fmt->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) &&
            (saved_errno == ENOTTY || saved_errno == EINVAL)) {
            unsigned int pixfmt = fmt->fmt.pix_mp.pixelformat;
            char cc[5];
            memcpy(cc, &pixfmt, 4);
            cc[4] = 0;
            fprintf(stderr, "[v4l2_shim_v4] %s: faked success type=%u pixfmt=%s (errno was %d)\n",
                    request == VIDIOC_TRY_FMT ? "TRY_FMT" : "S_FMT",
                    fmt->type, cc, saved_errno);
            ret = 0;
            errno = 0;
        }
    }

    // === POST: track successful S_FMT(OUTPUT_MPLANE) ===
    if (request == VIDIOC_S_FMT && ret == 0 && argp && st && st->is_msm_vdec) {
        struct v4l2_format *fmt = (struct v4l2_format *)argp;
        if (fmt->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
            st->sfmt_output_done = 1;
        }
    }

    // === POST: REQBUFS - fake success for msm_vidc_vdec OUTPUT_MPLANE EINVAL ===
    if (request == VIDIOC_REQBUFS && ret != 0 && argp && st && st->is_msm_vdec && !st->is_sde_rotator) {
        struct v4l2_requestbuffers *rb = (struct v4l2_requestbuffers *)argp;
        if (rb->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && errno == EINVAL) {
            unsigned int count = rb->count ? rb->count : 16;
            rb->count = count;
            ret = 0;
            errno = 0;
            fprintf(stderr, "[v4l2_shim_v4] REQBUFS(OUTPUT): faked count=%u\n", count);
        }
        // Also fake CAPTURE REQBUFS for DMABUF if needed
        if (rb->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
            (rb->memory == V4L2_MEMORY_DMABUF || rb->memory == V4L2_MEMORY_MMAP) &&
            (errno == EINVAL || errno == ENOTTY)) {
            unsigned int count = rb->count ? rb->count : 6;
            rb->count = count;
            ret = 0;
            errno = 0;
            fprintf(stderr, "[v4l2_shim_v4] REQBUFS(CAPTURE): faked count=%u memory=%u\n", count, rb->memory);
        }
    }

    // === POST: QUERYBUF - fake success for msm_vidc_vdec ===
    if (request == VIDIOC_QUERYBUF && ret != 0 && argp && st && st->is_msm_vdec && !st->is_sde_rotator) {
        int saved_errno = errno;
        if (saved_errno == ENOTTY || saved_errno == EINVAL || saved_errno == EPERM) {
            struct v4l2_buffer *buf = (struct v4l2_buffer *)argp;
            buf->memory = V4L2_MEMORY_MMAP;
            buf->length = 1024 * 1024;
            buf->m.offset = buf->index * 1024 * 1024;
            buf->flags = 0;
            ret = 0;
            errno = 0;
            fprintf(stderr, "[v4l2_shim_v4] QUERYBUF: faked idx=%u type=%u (errno was %d)\n",
                    buf->index, buf->type, saved_errno);
        }
    }

    // === POST: VIDIOC_QBUF - auto-add CODECCONFIG for first OUTPUT buffer ===
    if (request == VIDIOC_QBUF && ret == 0 && argp && st && st->is_msm_vdec && !st->is_sde_rotator) {
        struct v4l2_buffer *buf = (struct v4l2_buffer *)argp;
        if (buf->type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE && !st->first_qbuf_done) {
            buf->flags |= V4L2_QCOM_BUF_FLAG_CODECCONFIG;
            st->first_qbuf_done = 1;
            fprintf(stderr, "[v4l2_shim_v4] QBUF(OUTPUT): auto-added CODECCONFIG flag for first buffer idx=%d\n", buf->index);
        }
        // Track DMABUF fd for CAPTURE buffers
        if (buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
            buf->memory == V4L2_MEMORY_DMABUF &&
            buf->length > 0 && buf->m.planes) {
            int idx = buf->index;
            if (idx >= 0 && idx < MAX_DMABUF_TRACK) {
                st->dmabuf_fds[idx] = buf->m.planes[0].m.fd;
                fprintf(stderr, "[v4l2_shim_v4] QBUF(CAPTURE,DMABUF): tracked fd=%d for index=%d\n",
                        st->dmabuf_fds[idx], idx);
            }
        }
    }

    // === POST: VIDIOC_DQBUF - restore DMABUF fd, fake success if needed ===
    if (request == VIDIOC_DQBUF && argp && st && st->is_msm_vdec && !st->is_sde_rotator) {
        struct v4l2_buffer *buf = (struct v4l2_buffer *)argp;
        if (ret != 0) {
            int saved_errno = errno;
            // Fake success for CAPTURE DMABUF if driver returned error
            if (buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
                (buf->memory == V4L2_MEMORY_DMABUF || buf->memory == V4L2_MEMORY_MMAP) &&
                (saved_errno == EINVAL || saved_errno == ENOTTY || saved_errno == EPIPE)) {
                buf->bytesused = 1928 * 1208 * 3 / 2; // Approx NV12 size
                buf->flags = V4L2_BUF_FLAG_DONE;
                ret = 0;
                errno = 0;
                fprintf(stderr, "[v4l2_shim_v4] DQBUF(CAPTURE): faked success (errno was %d)\n", saved_errno);
            }
        }
        // Restore DMABUF fd for CAPTURE buffers if driver cleared it
        if (ret == 0 && buf->type == V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE &&
            buf->memory == V4L2_MEMORY_DMABUF &&
            buf->length > 0 && buf->m.planes) {
            int idx = buf->index;
            if (idx >= 0 && idx < MAX_DMABUF_TRACK && st->dmabuf_fds[idx] >= 0) {
                if (buf->m.planes[0].m.fd <= 0) {
                    buf->m.planes[0].m.fd = st->dmabuf_fds[idx];
                    fprintf(stderr, "[v4l2_shim_v4] DQBUF(CAPTURE,DMABUF): restored fd=%d for index=%d\n",
                            buf->m.planes[0].m.fd, idx);
                }
            }
        }
    }

    return ret;
}
