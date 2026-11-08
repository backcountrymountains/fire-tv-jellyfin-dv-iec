/*
 * felplay_cli.c - test driver for libfelplay.  Run as root on the Cube.
 * Input: framed stream ("FELS" header + records), see README.md.
 */
#define _GNU_SOURCE
#include "felplay.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define MAX_AU (64u * 1024u * 1024u)

static volatile sig_atomic_t g_stop;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

static void sleep_ms(int ms)
{
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

static void logcb(void *ctx, const char *msg)
{
    (void)ctx;
    fprintf(stderr, "felplay: %s\n", msg);
}

/* ---- sysfs helpers ---- */

static int sys_write(const char *path, const char *val)
{
    int fd = open(path, O_WRONLY);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = write(fd, val, strlen(val));
    close(fd);
    return n < 0 ? -1 : 0;
}

static int sys_read(const char *path, char *out, size_t cap)
{
    int fd = open(path, O_RDONLY);
    ssize_t n;
    if (fd < 0)
        return -1;
    n = read(fd, out, cap - 1);
    close(fd);
    if (n < 0)
        return -1;
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == ' '))
        out[--n] = '\0';
    return 0;
}

/* Remember the old value, write the new one; restore later. */
struct saved {
    const char *path;
    char old[160];
    int have;
};

static void sys_set_saved(struct saved *s, const char *path, const char *val)
{
    s->path = path;
    s->have = sys_read(path, s->old, sizeof(s->old)) == 0;
    if (sys_write(path, val) < 0)
        fprintf(stderr, "felplay-cli: write %s failed (errno %d)\n", path, errno);
}

static void sys_restore(struct saved *s)
{
    if (s->path && s->have)
        sys_write(s->path, s->old);
}

/* /sys/class/display/axis: esplayer pushes the OSD off-screen with first value 2048. */
static void axis_push(struct saved *s)
{
    char buf[160], nb[200];
    int v[8] = { 0 };
    int n;
    s->path = "/sys/class/display/axis";
    s->have = sys_read(s->path, buf, sizeof(buf)) == 0;
    if (!s->have) {
        return;
    }
    snprintf(s->old, sizeof(s->old), "%s", buf);
    n = sscanf(buf, "%d %d %d %d %d %d %d %d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
    if (n < 8) {
        s->have = 0;
        return;
    }
    snprintf(nb, sizeof(nb), "2048 %d %d %d %d %d %d %d", v[1], v[2], v[3], v[4], v[5], v[6], v[7]);
    sys_write(s->path, nb);
}

/* ---- options ---- */

struct opts {
    const char *input;
    int pts_mode;
    int clock_mono;
    int tsync;
    int blank_osd;
    const char *dvblpath;
    double pause_at, pause_for, seek_at, seek_skip, max_seconds;
    int throttle;
    int clock_ms, preroll_ms, lead_ms, drain_ms, pcr_rel;
    int dv, ext_pts, delay_ms, noblock;
};

static void usage(void)
{
    fputs("usage: felplay-cli <file|-> [--pts none|us64|90k] [--clock none|mono] [--clock-ms N]\n"
          "  [--preroll-ms N] [--tsync] [--blank-osd] [--dvblpath amvideo|amlvideo]\n"
          "  [--pause-at S --pause-for S] [--seek-at S --seek-skip S] [--max-seconds N]\n"
          "  [--throttle] [--lead-ms N] [--drain-ms N] [--no-dv] [--ext-pts 0|1]\n"
          "  [--video-delay-ms N] [--blocking]\n", stderr);
}

static int parse_opts(int argc, char **argv, struct opts *o)
{
    static const struct option lo[] = {
        { "pcr-rel", 0, 0, 40 }, { "pts", 1, 0, 1 }, { "clock", 1, 0, 2 }, { "clock-ms", 1, 0, 3 }, { "preroll-ms", 1, 0, 4 },
        { "tsync", 0, 0, 5 }, { "blank-osd", 0, 0, 6 }, { "dvblpath", 1, 0, 7 }, { "pause-at", 1, 0, 8 },
        { "pause-for", 1, 0, 9 }, { "seek-at", 1, 0, 10 }, { "seek-skip", 1, 0, 11 },
        { "max-seconds", 1, 0, 12 }, { "throttle", 0, 0, 13 }, { "lead-ms", 1, 0, 14 },
        { "drain-ms", 1, 0, 15 }, { "no-dv", 0, 0, 16 }, { "ext-pts", 1, 0, 17 },
        { "video-delay-ms", 1, 0, 18 }, { "blocking", 0, 0, 19 }, { 0, 0, 0, 0 }
    };
    int c;
    memset(o, 0, sizeof(*o));
    o->pts_mode = FP_PTS_NONE;
    o->pause_at = o->seek_at = -1;
    o->clock_ms = 100;
    o->preroll_ms = 500;
    o->lead_ms = 2000;
    o->drain_ms = 3000;
    o->dv = 1;
    o->ext_pts = -1;
    o->noblock = 1;
    while ((c = getopt_long(argc, argv, "", lo, NULL)) != -1) {
        switch (c) {
        case 1:
            if (!strcmp(optarg, "none")) o->pts_mode = FP_PTS_NONE;
            else if (!strcmp(optarg, "us64")) o->pts_mode = FP_PTS_US64;
            else if (!strcmp(optarg, "90k")) o->pts_mode = FP_PTS_90K;
            else if (!strcmp(optarg, "old90k")) o->pts_mode = FP_PTS_OLD90K;
            else if (!strcmp(optarg, "old64")) o->pts_mode = FP_PTS_OLD64;
            else return -1;
            break;
        case 2:
            if (!strcmp(optarg, "mono")) o->clock_mono = 1;
            else if (!strcmp(optarg, "none")) o->clock_mono = 0;
            else return -1;
            break;
        case 3: o->clock_ms = atoi(optarg); break;
        case 4: o->preroll_ms = atoi(optarg); break;
        case 40: o->pcr_rel = 1; break;
        case 5: o->tsync = 1; break;
        case 6: o->blank_osd = 1; break;
        case 7:
            if (strcmp(optarg, "amvideo") && strcmp(optarg, "amlvideo")) return -1;
            o->dvblpath = optarg;
            break;
        case 8: o->pause_at = atof(optarg); break;
        case 9: o->pause_for = atof(optarg); break;
        case 10: o->seek_at = atof(optarg); break;
        case 11: o->seek_skip = atof(optarg); break;
        case 12: o->max_seconds = atof(optarg); break;
        case 13: o->throttle = 1; break;
        case 14: o->lead_ms = atoi(optarg); break;
        case 15: o->drain_ms = atoi(optarg); break;
        case 16: o->dv = 0; break;
        case 17: o->ext_pts = atoi(optarg); break;
        case 18: o->delay_ms = atoi(optarg); break;
        case 19: o->noblock = 0; break;
        default: return -1;
        }
    }
    if (optind >= argc)
        return -1;
    o->input = argv[optind];
    if (o->clock_ms < 1)
        o->clock_ms = 100;
    return 0;
}

/* ---- input ---- */

static int read_full(int fd, void *buf, size_t n)
{
    unsigned char *p = buf;
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, p + got, n - got);
        if (r > 0) {
            got += (size_t)r;
        } else if (r == 0) {
            return got == 0 ? 0 : -1; /* 0 = clean EOF, -1 = truncated */
        } else if (errno == EINTR) {
            if (g_stop)
                return -2;
        } else {
            return -1;
        }
    }
    return 1;
}

static uint32_t le32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static int64_t le64s(const unsigned char *p)
{
    return (int64_t)((uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32);
}

/* ---- main ---- */

struct clk {
    int started;
    int64_t base_pts_us;   /* stream pts at t0 */
    int64_t t0_us;         /* mono time when base_pts was current */
    int64_t paused_total;
    int paused;
    int64_t pause_start, frozen_clk;
};

static int64_t clk_now(const struct clk *c, int64_t t)
{
    if (!c->started)
        return -1;
    if (c->paused)
        return c->frozen_clk;
    return c->base_pts_us + (t - c->t0_us) - c->paused_total;
}

int main(int argc, char **argv)
{
    struct opts o;
    unsigned char hdr[24], rh[16];
    unsigned char *rec = NULL;
    size_t rec_cap = 0;
    int fd, rc = 0, have = 0, eof = 0, seeking = 0, pause_done = 0, seek_done = 0, pausing = 0;
    uint32_t rlen = 0, rflags = 0;
    int64_t rpts = 0, first_pts_ever = -1, last_pts = -1, target_us = 0, seg_min_pts = -1;
    uint64_t records = 0, eagain = 0, errors = 0;
    int64_t t_start, t_next_status, t_next_tick, t_eof = 0, last_vpts_ms = -1;
    struct clk clk;
    fp_config cfg;
    fp_handle *h;
    int err = 0;
    struct saved s_tsync = { 0 }, s_free = { 0 }, s_fb_dbg = { 0 }, s_fb_blank = { 0 }, s_vpu = { 0 };
    struct saved s_dis = { 0 }, s_glob = { 0 }, s_axis = { 0 };
    int dvbl_changed = 0;
    struct sigaction sa;
    fp_status st;

    if (parse_opts(argc, argv, &o) < 0) {
        usage();
        return 2;
    }
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal; /* no SA_RESTART: blocking read returns EINTR */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    if (strncmp(o.input, "tcp:", 4) == 0) {
        /* tcp:<ipv4>:<port> -- connect directly (toybox nc drops buffered data at EOF) */
        char host[64];
        const char *colon = strrchr(o.input + 4, ':');
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        if (!colon || (size_t)(colon - (o.input + 4)) >= sizeof(host)) {
            fprintf(stderr, "felplay-cli: bad tcp input %s\n", o.input);
            return 2;
        }
        memcpy(host, o.input + 4, (size_t)(colon - (o.input + 4)));
        host[colon - (o.input + 4)] = 0;
        sa.sin_port = htons((unsigned short)atoi(colon + 1));
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0 || inet_pton(AF_INET, host, &sa.sin_addr) != 1 ||
            connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            fprintf(stderr, "felplay-cli: connect %s failed (errno %d)\n", o.input, errno);
            return 2;
        }
    } else {
        fd = strcmp(o.input, "-") ? open(o.input, O_RDONLY) : open("/proc/self/fd/0", O_RDONLY);
    }
    if (fd < 0) {
        fprintf(stderr, "felplay-cli: open %s: errno %d\n", o.input, errno);
        return 2;
    }
    if (read_full(fd, hdr, sizeof(hdr)) != 1 || memcmp(hdr, "FELS", 4) != 0 || le32(hdr + 4) != 1) {
        fprintf(stderr, "felplay-cli: bad stream header\n");
        return 2;
    }
    memset(&cfg, 0, sizeof(cfg));
    cfg.width = le32(hdr + 8);
    cfg.height = le32(hdr + 12);
    cfg.fps_num = le32(hdr + 16);
    cfg.fps_den = le32(hdr + 20);
    cfg.dv = o.dv;
    cfg.pts_mode = o.pts_mode;
    cfg.sysinfo_external_pts = o.ext_pts >= 0 ? o.ext_pts : (o.pts_mode != FP_PTS_NONE);
    cfg.video_delay_limit_ms = o.delay_ms;
    cfg.noblock = o.noblock;
    cfg.log = logcb;

    if (o.blank_osd) {
        sys_set_saved(&s_fb_dbg, "/sys/class/graphics/fb0/osd_display_debug", "1");
        sys_set_saved(&s_fb_blank, "/sys/class/graphics/fb0/blank", "1");
        sys_set_saved(&s_vpu, "/sys/kernel/debug/dri/0/vpu/blank", "1");
        sys_set_saved(&s_dis, "/sys/class/video/disable_video", "2");
        sys_set_saved(&s_glob, "/sys/class/video/video_global_output", "1");
        axis_push(&s_axis);
    }
    if (o.dvblpath) {
        /* must be in place before PORT_INIT; boot default is "dvbldec amvideo" */
        sys_write("/sys/class/vfm/map", "rm dvblpath");
        sys_write("/sys/class/vfm/map",
                  !strcmp(o.dvblpath, "amlvideo") ? "add dvblpath dvbldec amlvideo amvideo"
                                                  : "add dvblpath dvbldec amvideo");
        dvbl_changed = 1;
    }

    h = fp_open(&cfg, &err);
    if (!h) {
        fprintf(stderr, "felplay-cli: fp_open failed: %d\n", err);
        errors++;
        rc = 1;
        goto cleanup;
    }
    if (o.tsync) {
        sys_set_saved(&s_tsync, "/sys/class/tsync/enable", "1");
        sys_set_saved(&s_free, "/sys/class/video/freerun_mode", "0");
    } else {
        sys_set_saved(&s_tsync, "/sys/class/tsync/enable", "0");
        sys_set_saved(&s_free, "/sys/class/video/freerun_mode", "1");
    }

    memset(&clk, 0, sizeof(clk));
    t_start = now_us();
    t_next_status = t_start + 1000000;
    t_next_tick = t_start;

    while (!g_stop) {
        int64_t t = now_us();
        int64_t clk_us = clk_now(&clk, t);

        if (o.max_seconds > 0 && (t - t_start) >= (int64_t)(o.max_seconds * 1e6))
            break;

        /* clock feed */
        if (o.clock_mono && clk.started && !clk.paused && t >= t_next_tick) {
            /* --pcr-rel: the parser-less 5.4 decoder outputs pts counted from 0 at the first output picture,
             * so feed the clock relative to the lowest pts written since the (re)start */
            int64_t rel = (o.pcr_rel && seg_min_pts >= 0) ? seg_min_pts : 0;
            int64_t v = (clk_us - rel - (int64_t)o.preroll_ms * 1000) * 9 / 100;
            if (v < 0)
                v = 0;
            if (fp_set_pcrscr(h, (uint32_t)v) < 0)
                errors++;
            t_next_tick = t + (int64_t)o.clock_ms * 1000;
        }

        /* pause state machine (stream time = clock relative to first pts) */
        if (clk.started && first_pts_ever >= 0) {
            double stime = (double)(clk_us - first_pts_ever) / 1e6;
            if (o.pause_at >= 0 && !pause_done && !clk.paused && stime >= o.pause_at) {
                if (fp_pause(h, 1) < 0)
                    errors++;
                clk.paused = 1;
                clk.pause_start = t;
                clk.frozen_clk = clk_us;
                pausing = 1;
            }
            if (!seeking && !seek_done && !clk.paused && o.seek_at >= 0 && stime >= o.seek_at) {
                int r = fp_reset(h);
                if (r < 0) {
                    fprintf(stderr, "felplay-cli: fp_reset failed: %d\n", r);
                    errors++;
                    rc = 1;
                    break;
                }
                seek_done = 1;
                seeking = 1;
                target_us = first_pts_ever + (int64_t)((o.seek_at + o.seek_skip) * 1e6);
                have = 0;
                seg_min_pts = -1;
                memset(&clk, 0, sizeof(clk));
                t_next_tick = t;
            }
        }
        if (clk.paused && pausing && (t - clk.pause_start) >= (int64_t)(o.pause_for * 1e6)) {
            if (fp_pause(h, 0) < 0)
                errors++;
            clk.paused = 0;
            clk.paused_total += t - clk.pause_start;
            pausing = 0;
            pause_done = 1;
        }

        /* status line */
        if (t >= t_next_status) {
            long long vp = -1, pc = -1, dl = -1, sz = -1;
            fp_get(h, &st);
            if (st.vpts_ok) { vp = st.vpts / 90; last_vpts_ms = vp; }
            if (st.pcrscr_ok) pc = st.pcrscr / 90;
            if (st.vb_ok) { dl = st.vb_data_len; sz = st.vb_size; }
            printf("T=%lld wr=%llu pts=%lld clk=%lld vpts=%lld pcr=%lld vb=%lld/%lld\n",
                   (long long)((t - t_start) / 1000000), (unsigned long long)records,
                   (long long)(last_pts >= 0 ? last_pts / 1000 : -1),
                   (long long)(clk_us >= 0 ? clk_us / 1000 : -1), vp, pc, dl, sz);
            fflush(stdout);
            t_next_status += 1000000;
        }

        /* next record */
        if (!have && !eof) {
            int r = read_full(fd, rh, sizeof(rh));
            if (r == -2)
                break;
            if (r == 0) {
                eof = 1;
                t_eof = t;
            } else if (r < 0) {
                fprintf(stderr, "felplay-cli: truncated record header\n");
                errors++;
                rc = 1;
                break;
            } else {
                rlen = le32(rh);
                rflags = le32(rh + 4);
                rpts = le64s(rh + 8);
                if (rlen == 0 || rlen > MAX_AU) {
                    fprintf(stderr, "felplay-cli: bad record length %u\n", rlen);
                    errors++;
                    rc = 1;
                    break;
                }
                if (rlen > rec_cap) {
                    unsigned char *nb = realloc(rec, rlen);
                    if (!nb) { errors++; rc = 1; break; }
                    rec = nb;
                    rec_cap = rlen;
                }
                r = read_full(fd, rec, rlen);
                if (r == -2)
                    break;
                if (r != 1) {
                    fprintf(stderr, "felplay-cli: truncated record payload\n");
                    errors++;
                    rc = 1;
                    break;
                }
                have = 1;
                if (seeking) {
                    if (rpts >= target_us && (rflags & 1))
                        seeking = 0; /* this record is the resume point */
                    else
                        have = 0;
                }
                if (have && first_pts_ever < 0)
                    first_pts_ever = rpts;
            }
        }
        if (eof) {
            if (t - t_eof >= (int64_t)o.drain_ms * 1000)
                break;
            sleep_ms(20);
            continue;
        }
        if (!have)
            continue;

        /* throttle */
        if (o.throttle && o.clock_mono && clk.started && rpts - clk_us > (int64_t)o.lead_ms * 1000) {
            sleep_ms(5);
            continue;
        }

        {
            int r = fp_write(h, rec, rlen, rpts, 20);
            if (r == -EAGAIN) {
                eagain++;
                continue; /* same AU is retried; library kept the progress */
            }
            if (r < 0) {
                fprintf(stderr, "felplay-cli: fp_write failed: %d\n", r);
                errors++;
                rc = 1;
                break;
            }
            records++;
            last_pts = rpts;
            if (seg_min_pts < 0 || rpts < seg_min_pts)
                seg_min_pts = rpts;
            have = 0;
            if (!clk.started) {
                clk.started = 1;
                clk.base_pts_us = rpts;
                clk.t0_us = now_us();
                clk.paused_total = 0;
                t_next_tick = 0; /* feed pcrscr immediately */
            }
        }
    }

    fp_get(h, &st);
    if (st.vpts_ok)
        last_vpts_ms = st.vpts / 90;
    fp_close(h);

cleanup:
    sys_restore(&s_tsync);
    sys_restore(&s_free);
    if (dvbl_changed) {
        sys_write("/sys/class/vfm/map", "rm dvblpath");
        sys_write("/sys/class/vfm/map", "add dvblpath dvbldec amvideo");
    }
    if (o.blank_osd) {
        if (s_axis.have)
            sys_write(s_axis.path, s_axis.old);
        sys_restore(&s_vpu);
        sys_restore(&s_fb_blank);
        sys_restore(&s_fb_dbg);
        sys_restore(&s_dis);
        sys_restore(&s_glob);
    }
    free(rec);
    printf("RESULT: records=%llu eagain=%llu errors=%llu last_vpts_ms=%lld\n",
           (unsigned long long)records, (unsigned long long)eagain, (unsigned long long)errors,
           (long long)last_vpts_ms);
    return (rc || errors) ? 1 : 0;
}
