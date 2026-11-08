/*
 * felfix_reloc — patch the kernel addresses into an unpatched felfix template on the Cube itself, so the boot
 * module needs no VM build per boot (KASLR moves the kernel every boot).
 *
 * Same rules as scripts/build_felfix.py: the template holds 24 little-endian u64 placeholders
 * 0xFE1F000000000000 + i (order = the A_* enum in felfix.c); text symbols come from /proc/kallsyms, the data
 * symbols (not in kallsyms) are link-time addresses + the KASLR slide measured on dv_toggle_frame.
 * Caller must set /proc/sys/kernel/kptr_restrict to 0 first (else kallsyms reads as zeros).
 *
 * Usage: felfix_reloc <template.ko> <out.ko> [kallsyms path]   -> exit 0 and "RESULT: status=ok ..." on success
 * Build: zig cc -target arm-linux-musleabihf -static -O2 -Wall -Wextra -o felfix-reloc felfix_reloc.c
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define N_ADDR 24
#define MAGIC 0xFE1F000000000000ULL
#define DV_TOGGLE_LINK 0xFFFFFFC010A3C358ULL

static const char *const names[N_ADDR] = {
    "register_kprobe", "unregister_kprobe", "dv_toggle_frame", "VSYNC_WR_DV_REG", "VSYNC_WR_DV_REG_BITS",
    "cur_dispbuf2", "printk", "set_layer_display_canvas", "vd_layer0", "vd_layer1", "VSYNC_WR_MPEG_REG_BITS",
    "aml_write_dosbus", "kallsyms_lookup_name",
    "vf_peek", "vf_get", "vf_put", "vf_notify_provider_by_name", "amdv_vf_put", "dv_vf_light_reg_provider", "amdv_parse_metadata_v2_stb",
    "vf_get_provider", "vf_get_provider_by_name", "recv_common_dequeue_frame",
    "vf_notify_provider",
};
/* 0 = look up in kallsyms; else link-time address of a data symbol */
static const uint64_t data_link[N_ADDR] = {
    0, 0, 0, 0, 0, 0xFFFFFFC011ACA780ULL, 0, 0, 0xFFFFFFC011ACEFD8ULL, 0xFFFFFFC011ACF678ULL, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

int main(int argc, char **argv)
{
    uint64_t addr[N_ADDR] = {0};
    uint64_t slide;
    char line[512];
    unsigned char *ko;
    long size;
    FILE *f;
    int i;

    if (argc < 3) {
        fprintf(stderr, "usage: %s <template.ko> <out.ko> [kallsyms]\n", argv[0]);
        puts("RESULT: status=usage");
        return 2;
    }
    f = fopen(argc > 3 ? argv[3] : "/proc/kallsyms", "r");
    if (!f) {
        puts("RESULT: status=fail error=kallsyms_open");
        return 1;
    }
    while (fgets(line, sizeof(line), f)) {
        char sym[256], type[8];
        unsigned long long a;
        /* "<addr> <type> <name>[ \t[module]]" — module symbols carry a 4th field, skip them */
        int n = sscanf(line, "%llx %7s %255s", &a, type, sym);
        if (n != 3 || strchr(line, '['))
            continue;
        for (i = 0; i < N_ADDR; i++)
            if (!data_link[i] && !addr[i] && strcmp(sym, names[i]) == 0)
                addr[i] = a;
    }
    fclose(f);
    if (!addr[2]) {
        puts("RESULT: status=fail error=no_dv_toggle_frame (kptr_restrict?)");
        return 1;
    }
    slide = addr[2] - DV_TOGGLE_LINK;
    for (i = 0; i < N_ADDR; i++) {
        if (data_link[i])
            addr[i] = data_link[i] + slide;
        if (!addr[i]) {
            printf("RESULT: status=fail error=missing_symbol sym=%s\n", names[i]);
            return 1;
        }
    }

    f = fopen(argv[1], "rb");
    if (!f || fseek(f, 0, SEEK_END) || (size = ftell(f)) <= 0 || fseek(f, 0, SEEK_SET)) {
        puts("RESULT: status=fail error=template_open");
        return 1;
    }
    ko = malloc((size_t)size);
    if (!ko || fread(ko, 1, (size_t)size, f) != (size_t)size) {
        puts("RESULT: status=fail error=template_read");
        return 1;
    }
    fclose(f);
    for (i = 0; i < N_ADDR; i++) {
        unsigned char pat[8];
        long off, hit = -1;
        int count = 0, b;
        for (b = 0; b < 8; b++)
            pat[b] = (unsigned char)((MAGIC + (uint64_t)i) >> (8 * b));
        for (off = 0; off + 8 <= size; off++)
            if (memcmp(ko + off, pat, 8) == 0) {
                count++;
                hit = off;
            }
        if (count != 1) {
            printf("RESULT: status=fail error=placeholder idx=%d count=%d\n", i, count);
            return 1;
        }
        for (b = 0; b < 8; b++)
            ko[hit + b] = (unsigned char)(addr[i] >> (8 * b));
    }
    f = fopen(argv[2], "wb");
    if (!f || fwrite(ko, 1, (size_t)size, f) != (size_t)size || fclose(f)) {
        puts("RESULT: status=fail error=write");
        return 1;
    }
    printf("RESULT: status=ok slide=%#llx bytes=%ld\n", (unsigned long long)slide, size);
    return 0;
}
