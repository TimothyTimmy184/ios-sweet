/*
 * MDP (SDE 5.x, sdmmagpie = SM7150) splash handling. Offsets are from mdp_phys
 * (sdmmagpie-sde.dtsi: qcom,sde-sspp-off / -ctl-off / -pp-off); register layout
 * from msm-4.14 sde_hw_sspp.c / sde_hw_ctl.c / sde_hw_pingpong.c.
 */
#include "panel.h"
#include "console.h"

#define MDP_BASE		0x0ae00000UL

#define SSPP_SRC_SIZE		0x00	/* h << 16 | w */
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC1_ADDR		0x18	/* plane 1, or rect 1 in multirect mode */
#define SSPP_SRC_YSTRIDE0	0x24	/* plane 0 pitch in bytes, low 16 bits */
#define SSPP_SRC_FORMAT		0x30	/* bits 10:9 = bytes per pixel - 1 */

#define CTL_TOP			0x14
#define CTL_START		0x1c

#define PP_TEAR_CHECK_EN	0x00
#define PP_AUTOREFRESH_CONFIG	0x30	/* bit 31 enable, low bits = TE count per frame */
#define PP_AUTOREFRESH_EN	(1U << 31)

#ifdef BOARD_FLAME
/* SM8150 (sm8150-sde.dtsi): VIG0-3, DMA0-3. */
static const uint32_t sspp_off[] = { 0x5000, 0x7000, 0x9000, 0xb000, 0x25000, 0x27000, 0x29000, 0x2b000 };
#else
static const uint32_t sspp_off[] = { 0x5000, 0x7000, 0x25000, 0x27000, 0x29000 };
#endif
static const uint32_t ctl_off[] = { 0x2000, 0x2200, 0x2400, 0x2600, 0x2800, 0x2a00 };
#ifdef BOARD_FLAME
static const uint32_t pp_off[] = { 0x71000, 0x71800, 0x72000, 0x72800, 0x73000, 0x73800 };
#else
static const uint32_t pp_off[] = { 0x71000, 0x71800, 0x72000, 0x72800 };
#endif

#define N(a) (int)(sizeof(a) / sizeof((a)[0]))

static inline uint32_t mdp_rd(uint32_t off) { return *(volatile uint32_t *)(MDP_BASE + off); }
static inline void mdp_wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(MDP_BASE + off) = v; }

void panel_detect(struct panel_info *pi)
{
	pi->sspp = pi->sspp2 = -1;
	for (int i = 0; i < N(sspp_off); i++) {
		uint32_t a = mdp_rd(sspp_off[i] + SSPP_SRC0_ADDR);
		if (a < FB_DEF_BASE || a >= FB_DEF_BASE + FB_MAX_BYTES)
			continue;
		pi->sspp = i;
		pi->src_addr = a;
		pi->src_size = mdp_rd(sspp_off[i] + SSPP_SRC_SIZE);
		pi->ystride = mdp_rd(sspp_off[i] + SSPP_SRC_YSTRIDE0) & 0xffff;
		pi->format = mdp_rd(sspp_off[i] + SSPP_SRC_FORMAT);
		break;
	}
	if (pi->sspp < 0)
		return;		/* keep board defaults */

	uint32_t w = pi->src_size & 0xffff, h = pi->src_size >> 16;
	uint32_t bpp = ((pi->format >> 9) & 3) + 1;
	/*
	 * Source split (Pixel 4): ABL scans one wide surface through two pipes, each
	 * w pixels, the second starting w * bpp bytes in with the same size and pitch.
	 * Treat the pair as one surface of width 2w.
	 */
	for (int i = 0; i < N(sspp_off); i++) {
		if (i == pi->sspp)
			continue;
		if (mdp_rd(sspp_off[i] + SSPP_SRC0_ADDR) == pi->src_addr + w * bpp &&
		    mdp_rd(sspp_off[i] + SSPP_SRC_SIZE) == pi->src_size &&
		    (mdp_rd(sspp_off[i] + SSPP_SRC_YSTRIDE0) & 0xffff) == pi->ystride) {
			pi->sspp2 = i;
			w *= 2;
			break;
		}
	}
#ifdef BOARD_FLAME
	/*
	 * No partner pipe found, but the pitch holds two (or more) widths: the
	 * surface is wider than this pipe's rectangle. Use the full pitch.
	 * sspp2 = -2 marks this fallback on the console.
	 */
	if (pi->sspp2 < 0 && pi->ystride >= 2 * w * bpp) {
		w = pi->ystride / bpp;
		pi->sspp2 = -2;
	}
#endif
	/* Only trust a sane, 3/4-byte surface that fits the splash carve-out. */
	if (w == 0 || h == 0 || (bpp != 3 && bpp != 4) || pi->ystride < w * bpp ||
	    (uint64_t)pi->ystride * h > FB_MAX_BYTES - (pi->src_addr - FB_DEF_BASE))
		return;
	fb_base = pi->src_addr;
	fb_w = w;
	fb_h = h;
	fb_stride = pi->ystride;
	fb_bpp = bpp;
	con_fb = (volatile uint32_t *)fb_base;
}

void panel_keep_refreshing(struct panel_info *pi)
{
	pi->pp_te = pi->pp_ar_before = pi->ctl_kicked = 0;
	for (int i = 0; i < N(pp_off); i++) {
		if (!(mdp_rd(pp_off[i] + PP_TEAR_CHECK_EN) & 1))
			continue;
		pi->pp_te |= 1U << i;
		if (mdp_rd(pp_off[i] + PP_AUTOREFRESH_CONFIG) & PP_AUTOREFRESH_EN)
			pi->pp_ar_before |= 1U << i;
		else
			mdp_wr(pp_off[i] + PP_AUTOREFRESH_CONFIG, PP_AUTOREFRESH_EN | 1);
	}
	if (!pi->pp_te)
		return;		/* video mode: the panel already scans memory */
	for (int i = 0; i < N(ctl_off); i++) {
		if (!mdp_rd(ctl_off[i] + CTL_TOP))
			continue;
		mdp_wr(ctl_off[i] + CTL_START, 1);
		pi->ctl_kicked |= 1U << i;
	}
}

void panel_dump(void)
{
	for (int i = 0; i < N(sspp_off); i++) {
		uint32_t a0 = mdp_rd(sspp_off[i] + SSPP_SRC0_ADDR);
		uint32_t a1 = mdp_rd(sspp_off[i] + SSPP_SRC1_ADDR);
		uint32_t sz = mdp_rd(sspp_off[i] + SSPP_SRC_SIZE);
		if (!a0 && !a1 && !sz)
			continue;
		con_hex2("PIPE", "off=", sspp_off[i], "a0=", a0);
		con_hex2(" ", "a1=", a1, "sz=", sz);
	}
}
