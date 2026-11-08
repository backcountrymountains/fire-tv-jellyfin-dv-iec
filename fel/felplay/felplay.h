/*
 * felplay.h - libfelplay: drive an Amlogic amstream HEVC port (optionally the
 * Dolby Vision dual-layer port) with checked-in PTS.  No globals, no printf.
 * Protocol modelled on Kodi's AMLCodec (CoreELEC); ioctl wrappers
 * mirror dl/felprobe/src/vcodec.c (proven on the Cube).
 */
#ifndef FELPLAY_H
#define FELPLAY_H

#include <stdint.h>
#include <stddef.h>

typedef struct fp_handle fp_handle;

enum fp_pts_mode {
    FP_PTS_NONE = 0, /* no check-in (esplayer behaviour) */
    FP_PTS_US64 = 1, /* AMSTREAM_SET_TSTAMP_US64, microseconds */
    FP_PTS_90K = 2,  /* AMSTREAM_SET_TSTAMP, u32 90 kHz ticks */
    FP_PTS_OLD90K = 3, /* legacy AMSTREAM_IOC_TSTAMP, u32 90 kHz ticks by value */
    FP_PTS_OLD64 = 4   /* legacy AMSTREAM_IOC_TSTAMP_uS64, pointer to u64 microseconds */
};

typedef struct fp_config {
    uint32_t width;
    uint32_t height;
    uint32_t fps_num;               /* e.g. 24000 */
    uint32_t fps_den;               /* e.g. 1001 */
    int dv;                         /* 1 = /dev/amstream_dves_hevc, 0 = /dev/amstream_hevc */
    int pts_mode;                   /* enum fp_pts_mode */
    int sysinfo_external_pts;       /* sysinfo.param |= EXTERNAL_PTS (1), as Kodi */
    int video_delay_limit_ms;       /* AMSTREAM_SET_VIDEO_DELAY_LIMIT_MS after init; 0 = leave */
    int noblock;                    /* 1 = O_NONBLOCK port (needed for fp_write timeouts) */
    void (*log)(void *ctx, const char *msg); /* optional */
    void *log_ctx;
} fp_config;

/* A field is only meaningful when its *_ok flag is 1 (ioctl answered). */
typedef struct fp_status {
    int vpts_ok;          uint32_t vpts;            /* 90 kHz */
    int vpts_u64_ok;      uint64_t vpts_u64;
    int pcrscr_ok;        uint32_t pcrscr;          /* 90 kHz */
    int last_checkin_ok;  uint32_t last_checkin_vpts;
    int vb_ok;
    int32_t vb_size, vb_data_len, vb_free_len;
    uint32_t vb_read_ptr, vb_write_ptr;
} fp_status;

/* Returns NULL on failure; *err (if non-NULL) gets a negative errno. */
fp_handle *fp_open(const fp_config *cfg, int *err);

/*
 * Write one access unit.  pts_us < 0 means "no PTS for this AU" (no check-in).
 * Check-in happens once, before the first byte of the AU.
 * Returns len on success, -errno on failure.  -EAGAIN means the timeout expired
 * with the AU partly (or not at all) written: the library remembers the
 * progress and the PTS already checked in, so the caller MUST retry with the
 * same buf/len/pts_us (a different len returns -EINVAL) and nothing is
 * re-checked-in or re-written.  Any other error discards the pending AU.
 * fp_reset() also discards it.
 */
int fp_write(fp_handle *h, const void *buf, size_t len, int64_t pts_us, int timeout_ms);

int fp_set_pcrscr(fp_handle *h, uint32_t pts90k);
int fp_get(fp_handle *h, fp_status *st);       /* 0 if at least one field valid, else -ENOTSUP */
int fp_pause(fp_handle *h, int on);            /* AMSTREAM_IOC_VPAUSE on /dev/amvideo */
int fp_reset(fp_handle *h);                    /* close + reopen, same config (Kodi seek) */
void fp_close(fp_handle *h);

#endif
