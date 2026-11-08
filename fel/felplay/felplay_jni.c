/*
 * felplay_jni.c - JNI front end of libfelplay for the Jellyfin app
 * (org.jellyfin.playback.media3.exoplayer.dovi.FelPlay).  Built with felplay.c into libfelplay.so
 * (armeabi-v7a); build command in README.md.  Handles are fp_handle pointers passed as jlong.
 */
#include <errno.h>
#include <fcntl.h>
#include <jni.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <android/log.h>

#include "felplay.h"

#define TAG "CubeFel"
#define JNI_FN(name) Java_org_jellyfin_playback_media3_exoplayer_dovi_FelPlay_##name

static void log_cb(void *ctx, const char *msg)
{
    (void)ctx;
    __android_log_write(ANDROID_LOG_INFO, TAG, msg);
}

static fp_handle *H(jlong h)
{
    return (fp_handle *)(uintptr_t)h;
}

/* Returns the handle (> 0) or a negative errno. */
JNIEXPORT jlong JNICALL JNI_FN(nOpen)(JNIEnv *env, jclass cls, jint width, jint height, jint fpsNum, jint fpsDen,
                                      jboolean dv)
{
    fp_config cfg;
    fp_handle *h;
    int err = 0;
    (void)env;
    (void)cls;
    memset(&cfg, 0, sizeof(cfg));
    cfg.width = (uint32_t)width;
    cfg.height = (uint32_t)height;
    cfg.fps_num = (uint32_t)fpsNum;
    cfg.fps_den = (uint32_t)fpsDen;
    cfg.dv = dv ? 1 : 0;
    cfg.pts_mode = FP_PTS_NONE;      /* FireOS's parser-less port ignores checked-in PTS */
    cfg.sysinfo_external_pts = 0;
    cfg.video_delay_limit_ms = 0;
    cfg.noblock = 1;                 /* fp_write(timeout 0) returns -EAGAIN instead of blocking */
    cfg.log = log_cb;
    cfg.log_ctx = NULL;
    h = fp_open(&cfg, &err);
    if (!h)
        return err < 0 ? (jlong)err : (jlong)-EIO;
    return (jlong)(uintptr_t)h;
}

/* Direct ByteBuffer: bytes [off, off+len). Returns len, -EAGAIN (retry the same AU later) or -errno. */
JNIEXPORT jint JNICALL JNI_FN(nWriteDirect)(JNIEnv *env, jclass cls, jlong h, jobject buf, jint off, jint len,
                                            jlong ptsUs)
{
    uint8_t *p;
    (void)cls;
    if (!h)
        return -EBADF;
    p = (uint8_t *)(*env)->GetDirectBufferAddress(env, buf);
    if (!p || off < 0 || len <= 0 || (jlong)off + len > (*env)->GetDirectBufferCapacity(env, buf))
        return -EINVAL;
    return fp_write(H(h), p + off, (size_t)len, (int64_t)ptsUs, 0);
}

JNIEXPORT jint JNICALL JNI_FN(nWriteArray)(JNIEnv *env, jclass cls, jlong h, jbyteArray arr, jint off, jint len,
                                           jlong ptsUs)
{
    jbyte *p;
    int r;
    (void)cls;
    if (!h)
        return -EBADF;
    if (off < 0 || len <= 0 || off + len > (*env)->GetArrayLength(env, arr))
        return -EINVAL;
    p = (*env)->GetPrimitiveArrayCritical(env, arr, NULL);
    if (!p)
        return -ENOMEM;
    r = fp_write(H(h), p + off, (size_t)len, (int64_t)ptsUs, 0);
    (*env)->ReleasePrimitiveArrayCritical(env, arr, p, JNI_ABORT);
    return r;
}

JNIEXPORT jint JNICALL JNI_FN(nSetPcrscr)(JNIEnv *env, jclass cls, jlong h, jint pts90k)
{
    (void)env;
    (void)cls;
    return h ? fp_set_pcrscr(H(h), (uint32_t)pts90k) : -EBADF;
}

JNIEXPORT jint JNICALL JNI_FN(nPause)(JNIEnv *env, jclass cls, jlong h, jboolean on)
{
    (void)env;
    (void)cls;
    return h ? fp_pause(H(h), on ? 1 : 0) : -EBADF;
}

/* Close + reopen with the same config (seek).  On failure the handle stays allocated but unusable. */
JNIEXPORT jint JNICALL JNI_FN(nReset)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env;
    (void)cls;
    return h ? fp_reset(H(h)) : -EBADF;
}

JNIEXPORT void JNICALL JNI_FN(nClose)(JNIEnv *env, jclass cls, jlong h)
{
    (void)env;
    (void)cls;
    if (h)
        fp_close(H(h));
}

/* Video pts in 90 kHz ticks (AMSTREAM_GET_VPTS), or -1. */
JNIEXPORT jlong JNICALL JNI_FN(nGetVpts)(JNIEnv *env, jclass cls, jlong h)
{
    fp_status st;
    (void)env;
    (void)cls;
    if (!h || fp_get(H(h), &st) < 0 || !st.vpts_ok)
        return -1;
    return (jlong)st.vpts;
}

/* Writes value (no newline added) to a sysfs node; 0 or -errno. */
JNIEXPORT jint JNICALL JNI_FN(sysfsWrite)(JNIEnv *env, jclass cls, jstring path, jstring value)
{
    const char *p, *v;
    int fd, r = 0;
    size_t n;
    (void)cls;
    if (!path || !value)
        return -EINVAL;
    p = (*env)->GetStringUTFChars(env, path, NULL);
    v = (*env)->GetStringUTFChars(env, value, NULL);
    if (!p || !v) {
        r = -ENOMEM;
        goto out;
    }
    fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        r = -errno;
        goto out;
    }
    n = strlen(v);
    {
        ssize_t w = write(fd, v, n);
        if (w < 0)
            r = -errno;
        else if ((size_t)w != n)
            r = -EIO;
    }
    close(fd);
out:
    if (p)
        (*env)->ReleaseStringUTFChars(env, path, p);
    if (v)
        (*env)->ReleaseStringUTFChars(env, value, v);
    return r;
}

/* Reads up to 4 KiB of a sysfs node; null on error. */
JNIEXPORT jstring JNICALL JNI_FN(sysfsRead)(JNIEnv *env, jclass cls, jstring path)
{
    char buf[4096];
    const char *p;
    int fd;
    ssize_t n, total = 0;
    (void)cls;
    if (!path)
        return NULL;
    p = (*env)->GetStringUTFChars(env, path, NULL);
    if (!p)
        return NULL;
    fd = open(p, O_RDONLY | O_CLOEXEC);
    (*env)->ReleaseStringUTFChars(env, path, p);
    if (fd < 0)
        return NULL;
    while (total < (ssize_t)sizeof(buf) - 1 && (n = read(fd, buf + total, sizeof(buf) - 1 - (size_t)total)) > 0)
        total += n;
    close(fd);
    buf[total] = 0;
    for (ssize_t i = 0; i < total; i++)   /* NewStringUTF wants modified UTF-8: keep it ASCII */
        if ((unsigned char)buf[i] > 0x7e || (buf[i] < 0x20 && buf[i] != '\n' && buf[i] != '\t'))
            buf[i] = '?';
    return (*env)->NewStringUTF(env, buf);
}
