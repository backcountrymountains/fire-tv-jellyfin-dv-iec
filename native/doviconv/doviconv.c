/*
 * JNI shim over libdovi (quietvoid/dovi_tool): converts a Dolby Vision profile 7 RPU NAL (type 62, starting
 * at the 2-byte NAL header, no start code) to profile 8.1 (mode 2), the same calls as Kodi's
 * BitstreamConverter::convert_dovi_rpu_nal. Returns null when the RPU is not profile 7 or conversion fails.
 * Exported for org.jellyfin.playback.media3.exoplayer.dovi.DoviConv.
 */
#include <jni.h>
#include <stdint.h>
#include <libdovi/rpu_parser.h>

static jbyteArray convert(JNIEnv *env, jbyteArray nal) {
	jsize n = (*env)->GetArrayLength(env, nal);
	jbyte *in = (*env)->GetByteArrayElements(env, nal, NULL);
	if (!in) return NULL;
	jbyteArray out = NULL;
	DoviRpuOpaque *rpu = dovi_parse_unspec62_nalu((const uint8_t *) in, (size_t) n);
	const DoviRpuDataHeader *header = dovi_rpu_get_header(rpu);
	if (header && header->guessed_profile == 7 && dovi_convert_rpu_with_mode(rpu, 2) >= 0) {
		const DoviData *d = dovi_write_unspec62_nalu(rpu);
		if (d) {
			out = (*env)->NewByteArray(env, (jsize) d->len);
			if (out) (*env)->SetByteArrayRegion(env, out, 0, (jsize) d->len, (const jbyte *) d->data);
			dovi_data_free(d);
		}
	}
	if (header) dovi_rpu_free_header(header);
	dovi_rpu_free(rpu);
	(*env)->ReleaseByteArrayElements(env, nal, in, JNI_ABORT);
	return out;
}

JNIEXPORT jbyteArray JNICALL Java_org_jellyfin_playback_media3_exoplayer_dovi_DoviConv_convertP7(JNIEnv *env, jclass c, jbyteArray nal) {
	return convert(env, nal);
}
