/*
 * felplay.c - libfelplay implementation.  See felplay.h for the contract.
 * ioctl structs and the open sequence copy dl/felprobe/src/vcodec.c.
 */
#define _GNU_SOURCE
#include "felplay.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#define DEV_HEVC     "/dev/amstream_hevc"
#define DEV_DV_HEVC  "/dev/amstream_dves_hevc"
#define DEV_AMVIDEO  "/dev/amvideo"

#define AMSTREAM_IOC_MAGIC 'S'
#define AMSTREAM_IOC_SYSINFO     _IOW(AMSTREAM_IOC_MAGIC, 0x0a, int)
#define AMSTREAM_IOC_VPAUSE      _IOW(AMSTREAM_IOC_MAGIC, 0x17, int)
#define AMSTREAM_IOC_TSTAMP      _IOW(AMSTREAM_IOC_MAGIC, 0x0e, int)
#define AMSTREAM_IOC_TSTAMP_uS64 _IOW(AMSTREAM_IOC_MAGIC, 0x95, int)
#define AMSTREAM_IOC_GET_VERSION _IOR(AMSTREAM_IOC_MAGIC, 0xc0, int)
#define AMSTREAM_IOC_GET    _IOWR(AMSTREAM_IOC_MAGIC, 0xc1, struct am_ioctl_parm)
#define AMSTREAM_IOC_SET    _IOW(AMSTREAM_IOC_MAGIC, 0xc2, struct am_ioctl_parm)
#define AMSTREAM_IOC_GET_EX _IOWR(AMSTREAM_IOC_MAGIC, 0xc3, struct am_ioctl_parm_ex)

#define AMSTREAM_SET_VFORMAT       0x105
#define AMSTREAM_SET_TSTAMP        0x10E
#define AMSTREAM_SET_TSTAMP_US64   0x10F
#define AMSTREAM_PORT_INIT         0x111
#define AMSTREAM_SET_PCRSCR        0x118
#define AMSTREAM_SET_VIDEO_DELAY_LIMIT_MS 0x11A
#define AMSTREAM_SET_DRMMODE       0x11C
#define AMSTREAM_GET_VPTS          0x805
#define AMSTREAM_GET_PCRSCR        0x806
#define AMSTREAM_GET_LAST_CHECKIN_VPTS 0x808
#define AMSTREAM_GET_VPTS_U64      0x815
#define AMSTREAM_GET_EX_VB_STATUS  0x900

#define VFORMAT_HEVC 11
#define EXTERNAL_PTS 1

/* Same layout as vcodec.c; arm32 and arm64 agree (u64 aligned to 8). */
struct am_ioctl_parm {
    union {
        unsigned int data_32;
        unsigned long long data_64;
        int data_vformat;
        char data[8];
    };
    unsigned int cmd;
    char reserved[4];
};

struct buf_status {
    int size;
    int data_len;
    int free_len;
    unsigned int read_pointer;
    unsigned int write_pointer;
};

/* vcodec.c's union also holds struct vdec_status (112 bytes); only the size matters here. */
struct am_ioctl_parm_ex {
    union {
        struct buf_status status;
        unsigned long long align8;
        char data[112];
    };
    unsigned int cmd;
    char reserved[4];
};

typedef struct {
    unsigned int format, width, height, rate, extra, status, ratio;
    void *param;
    unsigned long long ratio64;
} dec_sysinfo_t;

_Static_assert(sizeof(struct am_ioctl_parm) == 16, "am_ioctl_parm must be 16 bytes");
_Static_assert(sizeof(struct buf_status) == 20, "buf_status must be 20 bytes");
_Static_assert(sizeof(struct am_ioctl_parm_ex) == 120, "am_ioctl_parm_ex must be 120 bytes");
_Static_assert(sizeof(void *) != 4 || sizeof(dec_sysinfo_t) == 40, "dec_sysinfo_t (arm32) must be 40 bytes");

struct fp_handle {
    fp_config cfg;
    int fd;          /* amstream port */
    int cntl;        /* /dev/amvideo */
    int au_pending;
    size_t au_len;
    size_t au_off;
    int64_t au_pts;
};

static void fp_logf(const fp_config *c, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    if (!c->log)
        return;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    c->log(c->log_ctx, msg);
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int sub_set(int fd, unsigned int cmd, unsigned long long val, int is64)
{
    struct am_ioctl_parm p;
    memset(&p, 0, sizeof(p));
    p.cmd = cmd;
    if (is64)
        p.data_64 = val;
    else
        p.data_32 = (unsigned int)val;
    return ioctl(fd, AMSTREAM_IOC_SET, &p) < 0 ? -errno : 0;
}

static int sub_get(int fd, unsigned int cmd, unsigned long long *val, int is64)
{
    struct am_ioctl_parm p;
    memset(&p, 0, sizeof(p));
    p.cmd = cmd;
    if (ioctl(fd, AMSTREAM_IOC_GET, &p) < 0)
        return -errno;
    *val = is64 ? p.data_64 : p.data_32;
    return 0;
}

/* Try the stream handle first, then /dev/amvideo (which node answers is unverified). */
static int get_any(fp_handle *h, unsigned int cmd, unsigned long long *val, int is64)
{
    int r = sub_get(h->fd, cmd, val, is64);
    if ((r == -ENOTTY || r == -EINVAL) && h->cntl >= 0)
        r = sub_get(h->cntl, cmd, val, is64);
    return r;
}

static void close_fds(fp_handle *h)
{
    if (h->cntl >= 0)
        close(h->cntl);
    if (h->fd >= 0)
        close(h->fd);
    h->cntl = h->fd = -1;
    h->au_pending = 0;
    h->au_len = h->au_off = 0;
}

/* vcodec_video_es_init + vcodec_init_cntl + PORT_INIT, in vcodec.c order. */
static int open_port(fp_handle *h)
{
    const fp_config *c = &h->cfg;
    const char *dev = c->dv ? DEV_DV_HEVC : DEV_HEVC;
    dec_sysinfo_t si;
    int version = 0, tries = 0, r;

    h->fd = h->cntl = -1;
    while ((h->fd = open(dev, O_WRONLY | (c->noblock ? O_NONBLOCK : 0))) < 0) {
        if (errno != EBUSY || ++tries >= 300) { /* vcodec.c retries 1000 x 10 ms */
            r = -errno;
            fp_logf(c, "open %s failed: errno %d", dev, -r);
            return r;
        }
        sleep_ms(10);
    }
    fp_logf(c, "opened %s fd=%d", dev, h->fd);

    if (ioctl(h->fd, AMSTREAM_IOC_GET_VERSION, &version) < 0 || version < 0x20000) {
        fp_logf(c, "amstream version %x: new-style ioctls unsupported", version);
        r = -ENOTSUP;
        goto fail;
    }
    if ((r = sub_set(h->fd, AMSTREAM_SET_VFORMAT, VFORMAT_HEVC, 0)) < 0) {
        fp_logf(c, "SET_VFORMAT failed: %d", r);
        goto fail;
    }
    memset(&si, 0, sizeof(si));
    si.format = 0; /* esplayer leaves it unset for HEVC */
    si.width = c->width;
    si.height = c->height;
    if (c->fps_num)
        si.rate = (unsigned int)(((uint64_t)96000 * c->fps_den + c->fps_num / 2) / c->fps_num);
    if (c->sysinfo_external_pts)
        si.param = (void *)(uintptr_t)EXTERNAL_PTS;
    if (ioctl(h->fd, AMSTREAM_IOC_SYSINFO, &si) < 0) {
        r = -errno;
        fp_logf(c, "SYSINFO failed: %d", r);
        goto fail;
    }
    if ((r = sub_set(h->fd, AMSTREAM_SET_DRMMODE, 0, 0)) < 0) {
        fp_logf(c, "SET_DRMMODE failed: %d", r);
        goto fail;
    }
    if ((h->cntl = open(DEV_AMVIDEO, O_RDWR)) < 0) {
        r = -errno;
        fp_logf(c, "open %s failed: %d", DEV_AMVIDEO, r);
        goto fail;
    }
    /* vbuf_size stays 0 -> kernel default ring, as esplayer and Kodi. */
    if ((r = sub_set(h->fd, AMSTREAM_PORT_INIT, 0, 0)) < 0) {
        fp_logf(c, "PORT_INIT failed: %d", r);
        goto fail;
    }
    if (c->video_delay_limit_ms > 0 &&
        (r = sub_set(h->fd, AMSTREAM_SET_VIDEO_DELAY_LIMIT_MS, (unsigned)c->video_delay_limit_ms, 0)) < 0)
        fp_logf(c, "SET_VIDEO_DELAY_LIMIT_MS failed: %d (continuing)", r);
    fp_logf(c, "port ready (version %x, sysinfo rate %u)", version, si.rate);
    return 0;
fail:
    close_fds(h);
    return r;
}

fp_handle *fp_open(const fp_config *cfg, int *err)
{
    fp_handle *h = calloc(1, sizeof(*h));
    int r;
    if (!h) {
        if (err)
            *err = -ENOMEM;
        return NULL;
    }
    h->cfg = *cfg;
    h->fd = h->cntl = -1;
    r = open_port(h);
    if (r < 0) {
        free(h);
        if (err)
            *err = r;
        return NULL;
    }
    return h;
}

int fp_write(fp_handle *h, const void *buf, size_t len, int64_t pts_us, int timeout_ms)
{
    const unsigned char *p = buf;
    int64_t deadline = mono_ms() + timeout_ms;
    int r;

    if (!h || h->fd < 0)
        return -EBADF;
    if (h->au_pending) {
        if (len != h->au_len)
            return -EINVAL;
    } else {
        if (pts_us >= 0 && h->cfg.pts_mode == FP_PTS_US64)
            r = sub_set(h->fd, AMSTREAM_SET_TSTAMP_US64, (unsigned long long)pts_us, 1);
        else if (pts_us >= 0 && h->cfg.pts_mode == FP_PTS_90K)
            r = sub_set(h->fd, AMSTREAM_SET_TSTAMP,
                        (unsigned long long)((uint64_t)pts_us * 9 / 100) & 0xffffffffu, 0);
        else if (pts_us >= 0 && h->cfg.pts_mode == FP_PTS_OLD90K)
            r = ioctl(h->fd, AMSTREAM_IOC_TSTAMP,
                      (unsigned long)(((uint64_t)pts_us * 9 / 100) & 0xffffffffu)) < 0 ? -errno : 0;
        else if (pts_us >= 0 && h->cfg.pts_mode == FP_PTS_OLD64) {
            unsigned long long v = (unsigned long long)pts_us;
            r = ioctl(h->fd, AMSTREAM_IOC_TSTAMP_uS64, &v) < 0 ? -errno : 0;
        } else
            r = 0;
        if (r < 0) {
            fp_logf(&h->cfg, "PTS check-in failed: %d", r);
            return r;
        }
        h->au_pending = 1;
        h->au_len = len;
        h->au_off = 0;
        h->au_pts = pts_us;
    }
    while (h->au_off < len) {
        ssize_t n = write(h->fd, p + h->au_off, len - h->au_off);
        if (n > 0) {
            h->au_off += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR)
            continue;
        if (n == 0 || (n < 0 && errno == EAGAIN)) {
            if (mono_ms() >= deadline)
                return -EAGAIN;
            sleep_ms(5);
            continue;
        }
        r = -errno;
        fp_logf(&h->cfg, "write failed: errno %d", -r);
        h->au_pending = 0;
        return r;
    }
    h->au_pending = 0;
    return (int)len;
}

int fp_set_pcrscr(fp_handle *h, uint32_t pts90k)
{
    int r;
    if (!h || h->fd < 0)
        return -EBADF;
    r = sub_set(h->fd, AMSTREAM_SET_PCRSCR, pts90k, 0);
    if ((r == -ENOTTY || r == -EINVAL) && h->cntl >= 0)
        r = sub_set(h->cntl, AMSTREAM_SET_PCRSCR, pts90k, 0);
    return r;
}

int fp_get(fp_handle *h, fp_status *st)
{
    unsigned long long v = 0;
    struct am_ioctl_parm_ex ex;
    int any = 0;

    memset(st, 0, sizeof(*st));
    if (!h || h->fd < 0)
        return -EBADF;
    if (get_any(h, AMSTREAM_GET_VPTS, &v, 0) == 0) {
        st->vpts_ok = 1; st->vpts = (uint32_t)v; any = 1;
    }
    if (get_any(h, AMSTREAM_GET_VPTS_U64, &v, 1) == 0) {
        st->vpts_u64_ok = 1; st->vpts_u64 = v; any = 1;
    }
    if (get_any(h, AMSTREAM_GET_PCRSCR, &v, 0) == 0) {
        st->pcrscr_ok = 1; st->pcrscr = (uint32_t)v; any = 1;
    }
    if (get_any(h, AMSTREAM_GET_LAST_CHECKIN_VPTS, &v, 0) == 0) {
        st->last_checkin_ok = 1; st->last_checkin_vpts = (uint32_t)v; any = 1;
    }
    memset(&ex, 0, sizeof(ex));
    ex.cmd = AMSTREAM_GET_EX_VB_STATUS;
    if (ioctl(h->fd, AMSTREAM_IOC_GET_EX, &ex) == 0) {
        st->vb_ok = 1;
        st->vb_size = ex.status.size;
        st->vb_data_len = ex.status.data_len;
        st->vb_free_len = ex.status.free_len;
        st->vb_read_ptr = ex.status.read_pointer;
        st->vb_write_ptr = ex.status.write_pointer;
        any = 1;
    }
    return any ? 0 : -ENOTSUP;
}

int fp_pause(fp_handle *h, int on)
{
    if (!h || h->cntl < 0)
        return -EBADF;
    return ioctl(h->cntl, AMSTREAM_IOC_VPAUSE, on ? 1 : 0) < 0 ? -errno : 0;
}

int fp_reset(fp_handle *h)
{
    if (!h)
        return -EBADF;
    close_fds(h);
    return open_port(h);
}

void fp_close(fp_handle *h)
{
    if (!h)
        return;
    close_fds(h);
    free(h);
}
