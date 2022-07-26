/*
 * gcc -shared -fPIC -O3 -Wall -o v4l2mjpg.so v4l2mjpg.c
 */

#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <time.h>

#include <linux/videodev2.h>

#ifndef V4L2_PIX_FMT_MJPEG
#define V4L2_PIX_FMT_MJPEG v4l2_fourcc('M', 'J', 'P', 'G')  /* Motion-JPEG */
#endif


struct buffer {
    void *start;
    size_t size;
};


struct context {
    int                 fd;
    struct v4l2_buffer  pending;
    int                 pending_size;
    struct buffer      *buffers;
    unsigned int        count;
};


static int xioctl(int fh, int request, void *arg)
{
    int r;

    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}


static int jpeg_check(const void *buffer,
                      unsigned int size,
                      uint16_t *width,
                      uint16_t *height)
{
    uint8_t *b = (uint8_t *)buffer;
    uint8_t *e = b + size;
    uint16_t w = 0;
    uint16_t h = 0;

    if (size < 4)
        return -1;

    /* start of image */
    if (*b++ != 0xFF || *b++ != 0xD8)
        return -1;

    /* iterate over all blocks */
    while (b < e) {
        for (; *b == 0xFF; b++)
            if (b >= e)
                return -1;

        switch (*b++) {
            case 0xC0:
            case 0xC2:
                /* SOF0/SOF2 (baseline/progressive) */
                if (b + 7 > e)
                    return -1;

                h = (b[3] << 8) + b[4];
                w = (b[5] << 8) + b[6];

                //printf("(%02X%02X: %hux%hu) ", *(b-2), *(b-1), w, h);
                b += (b[0] << 8) + b[1];

                break;

            case 0xC4:
            case 0xC9:
            case 0xCC:
            case 0xDB:
            case 0xDD:
            case 0xE0:
            case 0xE1:
            case 0xE2:
            case 0xE3:
            case 0xE4:
            case 0xE5:
            case 0xE6:
            case 0xE7:
            case 0xE8:
            case 0xE9:
            case 0xEA:
            case 0xEB:
            case 0xEC:
            case 0xED:
            case 0xEE:
            case 0xEF:
            case 0xFE:
                /* known block length */
                if (b + 2 > e)
                    return -1;

                //printf("(%02X%02X) ", *(b-2), *(b-1));
                b += (b[0] << 8) + b[1];

                break;

            case 0xDA:
                /* start of scan */
                //printf("(FFDA) ");
                do {
                    uint8_t *ff = memchr(b, 0xff, e - b);
                    if (ff == NULL)
                        return -1;
                    b = ff + 1;
                } while (b < e &&
                         (*b == 0x00 ||
                         (*b >= 0xD0 && *b <= 0xD7)));

                if (b < e)
                    b--;
                else
                    return -1;

                break;

            case 0xD9:
                /* end of image */
                //printf("(FFD9)\n");
                if (w == 0 || h == 0)
                    return -1;

                *width = w;
                *height = h;

                /* return real jpeg size */
                return (b - (uint8_t *)buffer);

            default:
                //printf("%02X ?\n", *(b - 1));
                return -1;
        }
    };

    return -1;
}


void v4l2_close(struct context *ctx)
{
    unsigned int i;

    if (ctx == NULL)
        return;

    if (ctx->buffers) {
        for (i = 0; i < ctx->count; i++) {
            if (ctx->buffers[i].start)
                munmap(ctx->buffers[i].start,
                       ctx->buffers[i].size);
        }
        free(ctx->buffers);
    }

    if (ctx->fd >= 0)
        close(ctx->fd);

    free(ctx);
}


struct context *v4l2_open(const char *device,
                          uint16_t width,
                          uint16_t height,
                          uint32_t *numerator,
                          uint32_t *denominator)
{
    struct v4l2_streamparm streamparm;
    struct v4l2_requestbuffers req;
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct context *ctx;
    unsigned int i;
    int e;

    ctx = malloc(sizeof(struct context));
    if (ctx == NULL) {
        errno = ENOMEM;
        return NULL;
    }

    ctx->fd = open(device, O_RDWR /* required */ | O_NONBLOCK, 0);
    ctx->count = 0;
    ctx->buffers = NULL;

    if (ctx->fd == -1)
        goto err;

    if (xioctl(ctx->fd, VIDIOC_QUERYCAP, &cap) == -1)
        goto err;

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        errno = EIO;
        goto err;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        errno = EIO;
        goto err;
    }

    memset(&streamparm, 0, sizeof(streamparm));
    streamparm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_G_PARM, &streamparm) == -1)
        goto err;

    if (streamparm.parm.capture.capability & V4L2_CAP_TIMEPERFRAME) {
        *numerator = streamparm.parm.capture.timeperframe.denominator;
        *denominator = streamparm.parm.capture.timeperframe.numerator;
    } else {
        *numerator = 0;
        *denominator = 0;
    }

    /* init video capture */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_MJPEG;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;
    if (xioctl(ctx->fd, VIDIOC_S_FMT, &fmt) == -1)
        goto err;

    /* init mmap */
    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(ctx->fd, VIDIOC_REQBUFS, &req) == -1)
        goto err;

    if (req.count < 2) {
        errno = EIO;
        goto err;
    }

    ctx->buffers = malloc(sizeof(struct buffer) * req.count);
    if (ctx->buffers == NULL) {
        errno = ENOMEM;
        goto err;
    }

    memset(ctx->buffers, 0, sizeof(struct buffer) * req.count);
    ctx->count = req.count;

    for (i = 0; i < ctx->count; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) == -1)
            goto err;

        ctx->buffers[i].size = buf.length;
        ctx->buffers[i].start = mmap(NULL, buf.length,
                                     PROT_READ | PROT_WRITE,
                                     MAP_SHARED, ctx->fd,
                                     buf.m.offset);

        if (ctx->buffers[i].start == MAP_FAILED) {
            ctx->buffers[i].start = NULL;
            errno = ENOMEM;
            goto err;
        }
    }

    return ctx;

err:
    e = errno;
    v4l2_close(ctx);
    errno = e;
    return NULL;
}


int v4l2_start(struct context *ctx)
{
    enum v4l2_buf_type type;
    unsigned int i;

    for (i = 0; i < ctx->count; ++i) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof(buf));

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(ctx->fd, VIDIOC_QBUF, &buf) == -1)
            return -1;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(ctx->fd, VIDIOC_STREAMON, &type) == -1)
        return -1;

    ctx->pending_size = -1;

    return 0;
}


int v4l2_stop(struct context *ctx)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (xioctl(ctx->fd, VIDIOC_STREAMOFF, &type) == -1)
        return -1;

    return 0;
}


void *v4l2_dqbuf(struct context *ctx,
                 unsigned int timeout,
                 size_t *size,
                 unsigned int *width,
                 unsigned int *height)
{
    struct v4l2_buffer buf;
    struct pollfd fd;
    uint16_t w, h;
    int framesize;
    int msec;
    int ret;

    if (ctx->pending_size >= 0) {
        errno = EBUSY;
        return NULL;
    }

    msec = 0;
    fd.fd = ctx->fd;
    fd.events = POLLIN;

    while (msec < timeout) {
        ret = poll(&fd, 1, 10);

        if (ret < 0) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return NULL;
        }

        if (ret == 0) {
            msec += 10;
            continue;
        }

        // dequeue buffer
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        if (xioctl(ctx->fd, VIDIOC_DQBUF, &buf) == -1) {
            if (errno == EINTR || errno == EAGAIN)
                continue;
            return NULL;
        }

        framesize = jpeg_check(ctx->buffers[buf.index].start,
                               buf.bytesused, &w, &h);

        /* {
            unsigned char *b = (unsigned char *)ctx->buffers[buf.index].start;
            printf("%d %d %02X%02X -> %02X%02X\n", buf.bytesused, framesize,
                   b[0], b[1], b[framesize-2], b[framesize-1]);
        } */

        if (framesize == -1) {
            // skip buggy frame
            xioctl(ctx->fd, VIDIOC_QBUF, &buf);
            continue;
        }

        ctx->pending = buf;
        ctx->pending_size = framesize;

        *size = (size_t) framesize;
        *width = w;
        *height = h;

        return ctx->buffers[ctx->pending.index].start;
    }

    errno = ETIME;
    return NULL;
}


int v4l2_qbuf(struct context *ctx)
{
    if (ctx->pending_size == -1) {
        errno = ENOENT;
        return -1;
    }

    // queue buffer
    if (xioctl(ctx->fd, VIDIOC_QBUF, &ctx->pending) == -1)
        return -1;

    ctx->pending_size = -1;

    return 0;
}
