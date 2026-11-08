/*
 * felfix — runtime FEL enable for the FireOS 5.4 Dolby Vision path on the Cube (T7).
 *
 * Amazon's kernel has the dual-layer (FEL) display code but three gaps keep the EL out of core1a
 * found by comparing register dumps of the same clip on CoreELEC and Fire OS:
 *   1. dv_toggle_frame() only fetches the EL frame (amdv_vf_peek_el -> cur_dispbuf2) when
 *      !multi_dv_mode, and the stb 2.6 cores always run multi mode.        -> kprobe redirect
 *   2. dv_core1a_set() writes core1a SWAP_CTRL0 = enable bit only (no composer / EL 4:1 / EL-in
 *      bits) and core1a reg1 = 0x70 without the EL 4:1 bypass bit.         -> rewrite in VSYNC_WR_DV_REG
 *   3. The T7 EL routing (AMDV_PATH_SWAP_CTRL2[5:4] = VD2, VPP_VD2_DSC_CTRL[4] = 0) is never set.
 *                                                                          -> rewrite in VSYNC_WR_DV_REG_BITS
 *      VPP_VD2_DSC_CTRL[4] (VD2 core1b bypass) is written to 1 once and never again, so the rewrite
 *      never sees it; left at 1, real FEL content shows black/green.  -> write 0 on each EL frame (4)
 *   4. The vsync EL code (inlined dvel_swap_frame) calls set_layer_display_canvas(&vd_layer[0], el_vf)
 *      where CoreELEC passes &vd_layer[1], so the EL frame replaces the BL on VD1.
 *                                                                          -> swap the layer argument
 * Values are CoreELEC's (common_drivers 5.15 amdv_hw.c dv_core1a_set, T7 stb branch).
 *   5. Not a display gap: decoder_common's vdec_sync_input_write() on parserless SoCs (T7 included,
 *      cpu id >= 0x36) programs HEVC_STREAM_WR_PTR only for the master of a dual-layer pair; the
 *      slave (EL) copies the master's software wp but decodes against the register value of the
 *      master's last run, sees an empty fifo, returns AGAIN, and the BL never runs again (seek stall,
 *      "h265 data skipped"). SamuriHL media_modules 0002.            -> write the register (5)
 *   6. amdv pairs each BL with whatever EL heads the "dvel" queue (EL frames carry no pts). A BL shown
 *      without its EL (miss, e.g. the EL decoder starting late after a seek) leaves that EL at the head
 *      for good: every later BL composites an older EL (measured 15 pictures behind on one film) and the
 *      stale ELs pin the EL decoder's buffers, so misses keep coming. SamuriHL common_drivers 0013:
 *      before the pairing, retire the ELs the decoders' put counts say are owed.  -> kprobes (6)-(8)
 *      (off by default: hangs the SoC / retires correct ELs, see FELFIX_SKIP)
 *   7. The EL decoder drops pictures right after a mid-stream start (IDR), 3-11 in the first GOP on
 *      one film, so positional pairing runs that many pictures off with no "not found el" at all.
 *      Pair by POC instead: when the EL at the queue head is a LATER picture than the BL, this BL's
 *      EL was dropped; show the BL alone and leave the EL queued. EL head EARLIER than the BL = a
 *      stale EL (its BL already went out alone): retire it the way the vendor realign does
 *      (vf_get, vf_put, vf_notify_provider(dvel, 2)) and look again. Matching EL that the decoder
 *      error-marked (decoded against the reference it lost: blotchy garbage on camera): retire it
 *      too and show the BL alone.                                            -> kprobe (9)
 *   8. The vendor's own realign (video_receiver.c recv_common_dequeue_frame) retires bl_put - el_put
 *      ELs on every dequeue. An EL picture the decoder dropped is never put, so it retires a GOOD EL
 *      for each one, and it retires one for every BL that (9) shows alone. With POC pairing it only
 *      fights it: skip it.                                                   -> kprobe (10)
 *   9. Cause of (7): Amazon's vh265_work_implement reads hevc->switch_dvlayer_flag to hand the turn to
 *      the other layer but never clears it (both reference sources do: `if (flag) { flag = 0; ...}`;
 *      the only clears are in the ISR). When the EL runs short of input mid-picture (BUFEMPTY -> AGAIN,
 *      restore_decode_state), the stale flag from its previous picture hands the turn to the BL, which
 *      starts at the EL picture's rewound position, skips its EL NALs and decodes ahead: that EL
 *      picture is lost and later ones miss it as a reference ("not in list1"). Only on an input
 *      underrun, so timing-dependent (gone with amvdec_h265 debug 0x1, which slows the decoder).
 *      Clear the flag right after the read, as the source does.               -> kprobe (11)
 *
 * Built without kernel headers by scripts/build_felfix.py: no imported symbols (kernel addresses
 * are patched into felfix_addr[] at build time), modinfo/__versions/struct module layout from
 * a GPL module from the device (amvdec_h265.ko). Runtime only: rmmod or reboot removes it.
 */
typedef unsigned long u64;
typedef unsigned int u32;

struct pt_regs { u64 regs[31]; u64 sp; u64 pc; u64 pstate; };

/* struct kprobe, Linux 5.4 arm64: addr 0x28, symbol_name 0x30, offset 0x38, pre_handler 0x40,
 * flags 0x80 (offsets read from FireOS register_kprobe); padded to 0x100. */
struct kprobe {
	char pad0[0x28];
	void *addr;
	const char *symbol_name;
	u32 offset;
	u32 pad1;
	int (*pre_handler)(struct kprobe *, struct pt_regs *);
	char pad2[0x100 - 0x48];
};

enum { A_REGISTER_KPROBE, A_UNREGISTER_KPROBE, A_DV_TOGGLE_FRAME, A_WR_DV_REG, A_WR_DV_REG_BITS,
       A_CUR_DISPBUF2, A_PRINTK, A_SET_CANVAS, A_VD_LAYER0, A_VD_LAYER1, A_WR_MPEG_REG_BITS,
       A_WRITE_DOSBUS, A_KALLSYMS_LOOKUP, A_VF_PEEK, A_VF_GET, A_VF_PUT, A_VF_NOTIFY_PROVIDER, A_AMDV_VF_PUT,
       A_DV_LIGHT_REG, A_PARSE_STB, A_VF_GET_PROVIDER, A_VF_GET_PROVIDER_BY_NAME,
       A_RECV_DEQUEUE, A_VF_NOTIFY_PROVIDER_RECV, A_N };
/* Magic placeholders, replaced by build_felfix.py with addresses from /proc/kallsyms. */
volatile u64 felfix_addr[A_N] = {
	0xfe1f000000000000UL, 0xfe1f000000000001UL, 0xfe1f000000000002UL, 0xfe1f000000000003UL,
	0xfe1f000000000004UL, 0xfe1f000000000005UL, 0xfe1f000000000006UL, 0xfe1f000000000007UL,
	0xfe1f000000000008UL, 0xfe1f000000000009UL, 0xfe1f00000000000aUL, 0xfe1f00000000000bUL,
	0xfe1f00000000000cUL, 0xfe1f00000000000dUL, 0xfe1f00000000000eUL, 0xfe1f00000000000fUL,
	0xfe1f000000000010UL, 0xfe1f000000000011UL, 0xfe1f000000000012UL, 0xfe1f000000000013UL,
	0xfe1f000000000014UL, 0xfe1f000000000015UL, 0xfe1f000000000016UL, 0xfe1f000000000017UL,
};

#define TOGGLE_GATE_OFF   0x60        /* adrp x8, multi_dv_mode page (the !multi_dv_mode test) */
#define TOGGLE_PEEK_OFF   0xd4        /* mov x0, x19 ; bl amdv_vf_peek_el */
#define INSN_GATE         0x90008648u
#define INSN_PEEK         0xaa1303e0u

#define CORE1A_REG1       0x1000001u  /* CORE1A_OFFSET | 0x01 */
#define CORE1A_SWAP_CTRL0 0x10000f3u  /* CORE1A_OFFSET | 0xf3 */
#define PATH_SWAP_CTRL2   0x1a71u
#define VPP_VD2_DSC_CTRL  0x1a84u

/* decoder_common vdec_sync_input_write (FireOS amlogic media 5.4.125 build): at +0x124 the slave branch has
 * just called vbuf.ops->set_wp(x0 = &vdec->vbuf, master wp) and x20 = &vdec->vbuf = vdec + 0x50 */
#define SYNCWR_SYM        "vdec_sync_input_write"
#define SYNCWR_SLAVE_OFF  0x124       /* b <ret> right after the slave's set_wp */
#define INSN_SLAVE_SETWP  0xd63f0260u /* +0x120: blr x19 (set_wp) */
#define INSN_SLAVE_RET    0x14000018u /* +0x124: b +0x60 */
#define WORK_SYM          "vh265_work_implement"
#define WORK_SWITCH_OFF   0x1040      /* insn right after the switch_dvlayer_flag read */
#define INSN_WORK_LDRB    0x3942f268u /* +0x103c: ldrb w8, [x19, #0xbc] */
#define INSN_WORK_NEXT    0xf9402361u /* +0x1040: ldr x1, [x27, #0x40] (vdec->slave) */
#define HEVC_SWITCH_DVLAYER 0xbc      /* hevc_state_s.switch_dvlayer_flag (x19 = hevc) */
#define VDEC_VBUF         0x50
#define VDEC_TARGET       0x264       /* vdec->input.target; 1 = VDEC_INPUT_TARGET_HEVC */
#define VDEC_VBUF_OPS     0x130       /* vdec->vbuf.ops; get_wp at +0x18 */
#define HEVC_STREAM_WR_PTR 0x3104u

/* amdv_parse_metadata_v2_stb (FireOS kernel): the inlined dvel_vf_get() of the pairing,
 * x20 = the BL vframe being parsed */
#define STB_ELGET_OFF     0xd80       /* bl vf_get("dvel") */
#define STB_ELGOT_OFF     0xd84       /* cbz x0 (no EL: "not found el"); checked, not probed */
#define INSN_STB_ELGET    0x97f940aeu
#define INSN_STB_ELGOT    0xb400bce0u
#define EV_GET_AUX_DATA   0x80        /* VFRAME_EVENT_RECEIVER_GET_AUX_DATA; req.vf NULL = put count query */
#ifndef RETIRE_MAX
#define RETIRE_MAX        32
#endif
#define NHELD             16

/* POC lookup (FireOS amvdec_h265 / vfm, from vh265_event_cb and vf_peek): provider op_arg +0x10 = vdec,
 * vdec +0x11e40 = hevc (vdec->private), hevc +0x2ac8 = m_PIC[24], PIC_s +0x10 = POC, vframe +0 = index (low
 * byte = picture slot) */
#define PROV_OP_ARG       0x10
#define VDEC_PRIVATE      0x11e40
#define HEVC_M_PIC        0x2ac8
#define PIC_POC           0x10
#define PIC_ERROR_MARK    0x30        /* unsigned char error_mark (check_pic_decoded_error stores it) */
#define MAX_PIC_SLOT      24
/* POC restarts at every IDR, so order is (GOP epoch, POC): both layers come out in display order, a POC lower
 * than the previous one of the same layer is the next GOP. (A fixed |EL - BL| wrap threshold misread an EL more
 * than 12 behind as ahead, kept stale ELs, filled the EL pool and deadlocked both decoders, 2026-11-08.) */

/* recv_common_dequeue_frame (FireOS kernel): +0x110 starts the DV EL put-count realign (adrp "dvel";
 * vf_peek), every exit of it lands on +0x1d4 */
#define RECV_REALIGN_OFF  0x110
#define RECV_REALIGN_END  0x1d4
#define INSN_RECV_ADRP    0x900045a0u
#define INSN_RECV_PEEK    0x97feea8fu  /* +0x118 bl vf_peek */
#define INSN_RECV_END     0xb4002039u  /* +0x1d4 cbz x25 */

#ifdef FELFIX_DEBUG_VFGET
#define NKP 12
#else
#define NKP 11
#endif
#define KP_SWITCH 10                  /* (11) switch_dvlayer_flag clear */
/* kprobes left unregistered (bit i = kp[i]); build-time A/B switch (-DFELFIX_SKIP=...). Default 0xe0 = the
 * 0013 realign (6)-(8) OFF: with held misses feeding the retire it hung the SoC 1-2 s after a decoder session
 * start (no pstore record), and without them it retires correct ELs because FireOS puts a BL before its EL
 */
/* 0x300 also leaves (9) POC pairing and (10) vendor-realign skip off: they pair every picture correctly
 * (0 mismatched in ~3000 traced pairs, 5 films) but do not stop the start stall, so they are opt-in
 * (-DFELFIX_SKIP=0xe0) until the EL decoder's start behaviour is understood. */
#ifndef FELFIX_SKIP
#define FELFIX_SKIP 0x3e0
#endif
typedef int (*wr_bits_fn)(u32 adr, u32 val, u32 start, u32 len);
typedef void (*dosbus_fn)(u32 reg, u32 val);
typedef u32 (*get_wp_fn)(u64 vbuf);
typedef u64 (*lookup_fn)(const char *name);
typedef int (*printk_fn_t)(const char *, ...);
typedef u64 (*get_provider_fn)(const char *name);
typedef int (*notify_recv_fn)(const char *recv, int event, void *data);
#define EV_RECEIVER_PUT 2             /* what recv_common_dequeue_frame sends after each retired EL */
static u64 n_pair_match, n_pair_skip, n_pair_behind, n_pair_nopoc, n_vendor_skip, n_pair_elerr;
static u64 ep_el_vdec, ep_el_vf;              /* EL decoder instance / last EL head seen */
static int ep_bl, ep_bl_poc, ep_el, ep_el_poc;
typedef u64 (*vf_peek_fn)(const char *recv);
typedef void (*vf_put_fn)(u64 vf, const char *recv);
typedef int (*notify_fn)(const char *provider, int event, void *data);
static struct kprobe kp[NKP];
static u64 hits[6];
static const char DVEL[] = "dvel";
static u64 held[NHELD];               /* BLs shown without their EL, not yet put back */
static u32 last_bl_put;
static u64 n_retired, n_miss, n_retire_events, n_reset;
static u64 dbg_hits[5];
static u64 n_switch_clear;
static unsigned long active_skip;
#define DBG(k, ...) do { if (dbg_hits[k]++ < 3) ((printk_fn_t)felfix_addr[A_PRINTK])(__VA_ARGS__); } while (0)

static inline int el_active(void)
{
	return *(void *volatile *)felfix_addr[A_CUR_DISPBUF2] != 0;
}

/* (1) take the EL-fetch branch even in multi mode */
static int toggle_pre(struct kprobe *p, struct pt_regs *regs)
{
	hits[0]++;
	regs->pc = felfix_addr[A_DV_TOGGLE_FRAME] + TOGGLE_PEEK_OFF;
	return 1;                     /* pc changed: do not single-step the probed insn */
}

/* (2) VSYNC_WR_DV_REG(u32 adr, u32 val) */
static int wr_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 adr = (u32)regs->regs[0];

	if (adr != CORE1A_SWAP_CTRL0 && adr != CORE1A_REG1)
		return 0;
	if (!el_active())
		return 0;
	if (adr == CORE1A_SWAP_CTRL0 && (regs->regs[1] & 1)) {
		/* bl_enable | composer_enable<<1 | el_41_mode<<2 | (0x3<<4) */
		regs->regs[1] = (regs->regs[1] & ~0xffUL) | 0x37;
		hits[1]++;
	} else if (adr == CORE1A_REG1) {
		regs->regs[1] |= 0x8;     /* el_41_mode bypass flag */
		hits[2]++;
	}
	return 0;
}

/* (3) VSYNC_WR_DV_REG_BITS(u32 adr, u32 val, u32 start, u32 len) */
static int wrbits_pre(struct kprobe *p, struct pt_regs *regs)
{
	u32 adr = (u32)regs->regs[0], start = (u32)regs->regs[2], len = (u32)regs->regs[3];

	if (!el_active())
		return 0;
	if (adr == PATH_SWAP_CTRL2 && start == 4 && len == 2) {
		regs->regs[1] = 1;        /* core1a EL in = vd2 */
		hits[3]++;
	} else if (adr == VPP_VD2_DSC_CTRL && start == 4 && len == 1) {
		regs->regs[1] = 0;        /* vd2 path to core1a EL */
		hits[3]++;
	}
	return 0;
}

/* (4) set_layer_display_canvas(layer, vf, frame_par, layer_info, line) */
static int canvas_pre(struct kprobe *p, struct pt_regs *regs)
{
	u64 el = *(volatile u64 *)felfix_addr[A_CUR_DISPBUF2];

	if (el && regs->regs[1] == el && regs->regs[0] == felfix_addr[A_VD_LAYER0]) {
		regs->regs[0] = felfix_addr[A_VD_LAYER1];
		hits[4]++;
		/* (3b) VPP_VD2_DSC_CTRL[4] = 0: FireOS sets it once (core1b off) and never rewrites it,
		 * so the rewrite in wrbits_pre never fires for it; with it set, VD2 bypasses the EL path */
		((wr_bits_fn)felfix_addr[A_WR_MPEG_REG_BITS])(VPP_VD2_DSC_CTRL, 0, 4, 1);
	}
	return 0;
}

/* (5) the slave decoder's HEVC_STREAM_WR_PTR (SamuriHL media_modules 0002) */
static int slave_wp_pre(struct kprobe *p, struct pt_regs *regs)
{
	u64 vbuf = regs->regs[20], vdec = vbuf - VDEC_VBUF, ops;

	if (*(volatile u32 *)(vdec + VDEC_TARGET) != 1)
		return 0;
	ops = *(volatile u64 *)(vdec + VDEC_VBUF_OPS);
	if (!ops)
		return 0;
	((dosbus_fn)felfix_addr[A_WRITE_DOSBUS])(HEVC_STREAM_WR_PTR, ((get_wp_fn)*(u64 *)(ops + 0x18))(vbuf));
	hits[5]++;
	return 0;
}

/* the decoder's vf_put_count (vh265_event_cb answers GET_AUX_DATA with req.vf == NULL);
 * provider_aux_req_s: vf +0, aux_size +0x18 */
static int put_count(const char *provider, u32 *out)
{
	volatile u64 req[8];
	int i;

	for (i = 0; i < 8; i++)
		req[i] = 0;
	((volatile u32 *)req)[6] = 0xffffffffu;
	((notify_fn)felfix_addr[A_VF_NOTIFY_PROVIDER])(provider, EV_GET_AUX_DATA, (void *)req);
	if (((volatile u32 *)req)[6] == 0xffffffffu)
		return 0;
	*out = ((volatile u32 *)req)[6];
	return 1;
}

static void held_reset(void)
{
	int i;

	for (i = 0; i < NHELD; i++)
		held[i] = 0;
	last_bl_put = 0;
	n_reset++;
}

static void held_drop(u64 bl)
{
	int i;

	for (i = 0; i < NHELD; i++)
		if (held[i] == bl)
			held[i] = 0;
}

/* Record (miss) or clear (!miss) a BL shown without its EL. An empty dvel queue right before the
 * pairing's vf_get() is a miss: vf_get() returns NULL ("not found el"). A probe on the cbz after
 * the call instead (together with the one on the bl) hung the Cube at a session start, 2026-11-08. */
static void held_note(u64 bl, int miss)
{
	int i, free = -1;

	if (!miss) {
		held_drop(bl);
		return;
	}
	n_miss++;
	for (i = 0; i < NHELD; i++) {
		if (held[i] == bl)
			return;
		if (!held[i] && free < 0)
			free = i;
	}
	if (free >= 0)
		held[free] = bl;
}

/* (6) before the pairing's vf_get("dvel"): retire the ELs owed by BLs already returned */
static int elget_pre(struct kprobe *p, struct pt_regs *regs)
{
	u64 bl = regs->regs[20], el;
	u32 bl_put, el_put;
	int owed, n = 0, i;
	printk_fn_t pr = (printk_fn_t)felfix_addr[A_PRINTK];

	DBG(0, "\0016felfix: dbg elget enter bl %lx\n", bl);
	if (!put_count("dvbldec", &bl_put) || !put_count("dveldec", &el_put))
		return 0;
	if (bl_put < last_bl_put)         /* new decoder instances (seek / new play) */
		held_reset();
	DBG(1, "\0016felfix: dbg put counts bl %u el %u\n", bl_put, el_put);
	last_bl_put = bl_put;
	owed = (int)(bl_put - el_put);
	for (i = 0; i < NHELD; i++)
		if (held[i] && held[i] != bl)
			owed++;
	while (owed > 0 && n < RETIRE_MAX && ((vf_peek_fn)felfix_addr[A_VF_PEEK])(DVEL)) {
		el = ((vf_peek_fn)felfix_addr[A_VF_GET])(DVEL);
		if (!el)
			break;
		((vf_put_fn)felfix_addr[A_VF_PUT])(el, DVEL);
		owed--;
		n++;
	}
	held_note(bl, !((vf_peek_fn)felfix_addr[A_VF_PEEK])(DVEL));
	if (n) {
		n_retired += n;
		if (++n_retire_events <= 20 || n_retire_events % 100 == 0)
			pr("\0016felfix: bl put %u el put %u - retired %d stale el (events %lu, total %lu)\n",
			   bl_put, el_put, n, n_retire_events, n_retired);
	}
	return 0;
}

/* (7) amdv_vf_put(vf): a BL went back to the decoder; its put now counts */
static int amdv_put_pre(struct kprobe *p, struct pt_regs *regs)
{
	DBG(3, "\0016felfix: dbg amdv_vf_put %lx\n", regs->regs[0]);
	held_drop(regs->regs[0]);
	return 0;
}

/* (8) dv_vf_light_reg_provider(): a new dvel session */
static int light_reg_pre(struct kprobe *p, struct pt_regs *regs)
{
	DBG(4, "\0016felfix: dbg light_reg\n");
	held_reset();
	return 0;
}

/* the POC of a picture a decoder handed out (vf must still be held, so its slot is not reused) */
static int vf_poc(u64 prov, u64 vf, int *poc, int *err)
{
	u64 vdec, hevc, pic;
	u32 slot;

	if (!prov || !vf)
		return 0;
	vdec = *(volatile u64 *)(prov + PROV_OP_ARG);
	if (!vdec)
		return 0;
	hevc = *(volatile u64 *)(vdec + VDEC_PRIVATE);
	slot = *(volatile unsigned char *)vf;
	if (!hevc || slot >= MAX_PIC_SLOT)
		return 0;
	pic = *(volatile u64 *)(hevc + HEVC_M_PIC + 8 * slot);
	if (!pic)
		return 0;
	*poc = *(volatile int *)(pic + PIC_POC);
	*err = *(volatile unsigned char *)(pic + PIC_ERROR_MARK);
	return 1;
}

/* (9) at the pairing's vf_get("dvel"): pair by POC. EL head later than the BL = the EL decoder dropped
 * this BL's picture: emulate vf_get() == NULL (BL shown alone, EL stays queued for its own BL). */
static int pocpair_pre(struct kprobe *p, struct pt_regs *regs)
{
	u64 bl = regs->regs[20], el, elprov, vdec;
	int bp, ep, d, ahead, n, berr, eerr;
	printk_fn_t pr = (printk_fn_t)felfix_addr[A_PRINTK];

	elprov = ((get_provider_fn)felfix_addr[A_VF_GET_PROVIDER])(DVEL);
	if (!elprov || !vf_poc(((get_provider_fn)felfix_addr[A_VF_GET_PROVIDER_BY_NAME])("dvbldec"), bl, &bp, &berr))
		return 0;
	vdec = *(volatile u64 *)(elprov + PROV_OP_ARG);
	if (vdec != ep_el_vdec) {                  /* new decoder pair (play / seek): epochs restart */
		ep_el_vdec = vdec;
		ep_el_vf = 0;
		ep_bl = ep_el = 0;
		ep_bl_poc = ep_el_poc = -0x7fffffff;
	}
	if (bp < ep_bl_poc)
		ep_bl++;
	ep_bl_poc = bp;
	for (n = 0; ; n++) {
		el = ((vf_peek_fn)felfix_addr[A_VF_PEEK])(DVEL);
		if (!el)
			return 0;
		if (!vf_poc(elprov, el, &ep, &eerr)) {
			n_pair_nopoc++;
			return 0;
		}
		if (el != ep_el_vf) {
			if (ep < ep_el_poc)
				ep_el++;
			ep_el_poc = ep;
			ep_el_vf = el;
		}
		d = (ep_el - ep_bl) * 0x10000 + (ep - bp);
		if (!d && !eerr) {
			n_pair_match++;
			return 0;
		}
		if (!d) {
			/* this BL's own EL, but decoded with errors: drop it, BL alone */
			el = ((vf_peek_fn)felfix_addr[A_VF_GET])(DVEL);
			if (el) {
				((vf_put_fn)felfix_addr[A_VF_PUT])(el, DVEL);
				((notify_recv_fn)felfix_addr[A_VF_NOTIFY_PROVIDER_RECV])(DVEL, EV_RECEIVER_PUT, 0);
			}
			n_pair_elerr++;
			ahead = 1;
			break;
		}
		ahead = d > 0;
		if (n_pair_skip + n_pair_behind < 40 || (n_pair_skip + n_pair_behind) % 200 == 0)
			pr("\0016felfix: pair bl poc %d el poc %d -> %s (match %lu skip %lu behind %lu)\n", bp, ep,
			   ahead ? "bl alone" : "retire el", n_pair_match, n_pair_skip, n_pair_behind);
		if (ahead || n >= RETIRE_MAX)
			break;
		/* stale EL: retire it like recv_common_dequeue_frame does */
		el = ((vf_peek_fn)felfix_addr[A_VF_GET])(DVEL);
		if (!el)
			return 0;
		((vf_put_fn)felfix_addr[A_VF_PUT])(el, DVEL);
		((notify_recv_fn)felfix_addr[A_VF_NOTIFY_PROVIDER_RECV])(DVEL, EV_RECEIVER_PUT, 0);
		n_pair_behind++;
	}
	if (!ahead)
		return 0;
	n_pair_skip++;
	regs->regs[0] = 0;
	regs->pc = felfix_addr[A_PARSE_STB] + STB_ELGOT_OFF;
	return 1;                     /* pc changed: the bl is not executed */
}

/* (10) skip the vendor's put-count EL realign */
static int vendor_realign_pre(struct kprobe *p, struct pt_regs *regs)
{
	n_vendor_skip++;
	regs->pc = felfix_addr[A_RECV_DEQUEUE] + RECV_REALIGN_END;
	return 1;
}

#ifdef FELFIX_DEBUG_VFGET
/* debug: who takes ELs (vf_get("dvel")) besides the pairing, after the first POC skip */
static u64 dbg_vfget_n;
static int dbg_vfget_pre(struct kprobe *p, struct pt_regs *regs)
{
	const char *r = (const char *)regs->regs[0];

	if (!n_pair_skip || !r || r[0] != 'd' || r[1] != 'v' || r[2] != 'e' || r[3] != 'l' || r[4])
		return 0;
	if (dbg_vfget_n++ < 30)
		((printk_fn_t)felfix_addr[A_PRINTK])("\0016felfix: dbg vf_get(dvel) from %pS / %lx\n",
						    (void *)regs->regs[30], regs->regs[30]);
	return 0;
}
#endif

/* (11) consume hevc->switch_dvlayer_flag the way the source does, so an AGAIN keeps the turn */
static int switchflag_pre(struct kprobe *p, struct pt_regs *regs)
{
	volatile unsigned char *flag = (volatile unsigned char *)(regs->regs[19] + HEVC_SWITCH_DVLAYER);

	if (*flag) {
		*flag = 0;
		n_switch_clear++;
	}
	return 0;
}

typedef int (*reg_fn)(struct kprobe *);
typedef void (*unreg_fn)(struct kprobe *);
typedef int (*printk_fn)(const char *, ...);

int init_module(void)
{
	reg_fn reg = (reg_fn)felfix_addr[A_REGISTER_KPROBE];
	unreg_fn unreg = (unreg_fn)felfix_addr[A_UNREGISTER_KPROBE];
	printk_fn pr = (printk_fn)felfix_addr[A_PRINTK];
	u64 tog = felfix_addr[A_DV_TOGGLE_FRAME];
	u64 syncwr = ((lookup_fn)felfix_addr[A_KALLSYMS_LOOKUP])(SYNCWR_SYM);
	u64 stb = felfix_addr[A_PARSE_STB];
	u64 recv = felfix_addr[A_RECV_DEQUEUE];
	u64 work = ((lookup_fn)felfix_addr[A_KALLSYMS_LOOKUP])(WORK_SYM);
	unsigned long skip = FELFIX_SKIP;
	int i, ret;

	if (*(volatile u32 *)(tog + TOGGLE_GATE_OFF) != INSN_GATE ||
	    *(volatile u32 *)(tog + TOGGLE_PEEK_OFF) != INSN_PEEK) {
		pr("\0013felfix: dv_toggle_frame does not match the expected FireOS build, not loading\n");
		return -22;
	}
	if (!syncwr || *(volatile u32 *)(syncwr + SYNCWR_SLAVE_OFF - 4) != INSN_SLAVE_SETWP ||
	    *(volatile u32 *)(syncwr + SYNCWR_SLAVE_OFF) != INSN_SLAVE_RET) {
		pr("\0013felfix: vdec_sync_input_write (%lx) does not match the expected decoder_common, not loading\n",
		   syncwr);
		return -22;
	}
	if (*(volatile u32 *)(stb + STB_ELGET_OFF) != INSN_STB_ELGET ||
	    *(volatile u32 *)(stb + STB_ELGOT_OFF) != INSN_STB_ELGOT) {
		pr("\0013felfix: amdv_parse_metadata_v2_stb does not match the expected FireOS build, not loading\n");
		return -22;
	}
	if (*(volatile u32 *)(recv + RECV_REALIGN_OFF) != INSN_RECV_ADRP ||
	    *(volatile u32 *)(recv + RECV_REALIGN_OFF + 8) != INSN_RECV_PEEK ||
	    *(volatile u32 *)(recv + RECV_REALIGN_END) != INSN_RECV_END) {
		pr("\0013felfix: recv_common_dequeue_frame does not match the expected FireOS build, not loading\n");
		return -22;
	}
	if (!work || *(volatile u32 *)(work + WORK_SWITCH_OFF - 4) != INSN_WORK_LDRB ||
	    *(volatile u32 *)(work + WORK_SWITCH_OFF) != INSN_WORK_NEXT) {
		pr("\0014felfix: vh265_work_implement (%lx) does not match the expected amvdec_h265, (11) off\n", work);
		skip |= 1UL << KP_SWITCH;
	}
	kp[0].addr = (void *)(tog + TOGGLE_GATE_OFF);
	kp[0].pre_handler = toggle_pre;
	kp[1].addr = (void *)felfix_addr[A_WR_DV_REG];
	kp[1].pre_handler = wr_pre;
	kp[2].addr = (void *)felfix_addr[A_WR_DV_REG_BITS];
	kp[2].pre_handler = wrbits_pre;
	kp[3].addr = (void *)felfix_addr[A_SET_CANVAS];
	kp[3].pre_handler = canvas_pre;
	kp[4].addr = (void *)(syncwr + SYNCWR_SLAVE_OFF);
	kp[4].pre_handler = slave_wp_pre;
	kp[5].addr = (void *)(stb + STB_ELGET_OFF);
	kp[5].pre_handler = elget_pre;
	kp[6].addr = (void *)felfix_addr[A_AMDV_VF_PUT];
	kp[6].pre_handler = amdv_put_pre;
	kp[7].addr = (void *)felfix_addr[A_DV_LIGHT_REG];
	kp[7].pre_handler = light_reg_pre;
	kp[8].addr = (void *)(stb + STB_ELGET_OFF);
	kp[8].pre_handler = pocpair_pre;
	kp[9].addr = (void *)(recv + RECV_REALIGN_OFF);
	kp[9].pre_handler = vendor_realign_pre;
	kp[KP_SWITCH].addr = (void *)(work + WORK_SWITCH_OFF);
	kp[KP_SWITCH].pre_handler = switchflag_pre;
#ifdef FELFIX_DEBUG_VFGET
	kp[11].addr = (void *)felfix_addr[A_VF_GET];
	kp[11].pre_handler = dbg_vfget_pre;
#endif
	for (i = 0; i < NKP; i++) {
		if (skip & (1UL << i))
			continue;
		ret = reg(&kp[i]);
		if (ret) {
			pr("\0013felfix: register_kprobe %d failed %d\n", i, ret);
			while (--i >= 0)
				if (!(skip & (1UL << i)))
					unreg(&kp[i]);
			return ret;
		}
	}
	active_skip = skip;
	pr("\0016felfix: loaded (EL fetch in multi mode, core1a FEL bits, T7 EL routing, slave stream wp, EL realign, POC pairing, vendor realign off, "
	   "EL switch-flag clear; skip %lx)\n", skip);
	return 0;
}

void cleanup_module(void)
{
	unreg_fn unreg = (unreg_fn)felfix_addr[A_UNREGISTER_KPROBE];
	printk_fn pr = (printk_fn)felfix_addr[A_PRINTK];
	int i;

	for (i = 0; i < NKP; i++)
		if (!(active_skip & (1UL << i)))
			unreg(&kp[i]);
	pr("\0016felfix: unloaded; hits toggle=%lu swap0=%lu reg1=%lu route=%lu canvas=%lu slavewp=%lu miss=%lu retired=%lu "
	   "resets=%lu "
	   "pair match=%lu skip=%lu behind=%lu elerr=%lu nopoc=%lu vendor_skip=%lu switch_clear=%lu\n", hits[0], hits[1],
	   hits[2], hits[3], hits[4], hits[5], n_miss, n_retired, n_reset, n_pair_match, n_pair_skip, n_pair_behind,
	   n_pair_elerr, n_pair_nopoc, n_vendor_skip, n_switch_clear);
}

/* Clang CFI: the kernel checks indirect calls into module text against the module's __cfi_check. */
void __cfi_check(u64 id, void *ptr, void *diag) { }

/* struct module (5.4, this kernel's layout): name at 0x18, init at 0x150, exit at 0x340, size 0x380 */
struct module_5_4 {
	char pad0[0x18];
	char name[56];
	char pad1[0x150 - 0x18 - 56];
	int (*init)(void);
	char pad2[0x340 - 0x158];
	void (*exit)(void);
	char pad3[0x380 - 0x348];
};
__attribute__((section(".gnu.linkonce.this_module"), aligned(64), used))
struct module_5_4 __this_module = { .name = "felfix", .init = init_module, .exit = cleanup_module };

__attribute__((section(".modinfo"), used, aligned(1)))
static const char felfix_modinfo[] =
	"license=GPL\0"
	"description=FireOS FEL enable (kprobes)\0"
	"vermagic=5.4.125 SMP preempt mod_unload modversions aarch64\0"
	"name=felfix\0"
	"depends=";

/* __versions (module_layout CRC etc.) copied from a device module by the builder. */
#include "felfix_versions.h"

/* The arm64 module loader requires these sections to exist (it sizes them itself). */
__asm__(".section .plt,\"ax\",@nobits\n.space 1\n"
	".section .init.plt,\"ax\",@nobits\n.space 1\n"
	".section .text.ftrace_trampoline,\"ax\",@nobits\n.space 1\n"
	".previous\n");
