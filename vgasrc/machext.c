// QEMU IA-64 ATI mach64 (RAGE XL) VGABIOS extension.
//
// Written for the mach64-vga model of qemu-system-ia64 (hw/display/mach64.c).
// Register layouts follow the ATI RAGE XL Register Reference Guide
// (RRG-C04300); the AH=A0h interface follows the RAGE PRO Programmer's
// Guide (PRG-215R3) App. A.  Timings are VESA DMT.
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "biosvar.h" // GET_GLOBAL
#include "bregs.h" // struct bregs
#include "hw/pci.h" // pci_config_readl
#include "hw/pci_regs.h" // PCI_BASE_ADDRESS_0
#include "output.h" // dprintf
#include "std/vbe.h" // VBE_CAPABILITY_8BIT_DAC
#include "stdvga.h" // stdvga_set_mode
#include "string.h" // memcpy_far
#include "vgabios.h" // SET_VGA
#include "vgautil.h" // VBE_total_memory
#include "x86.h" // outl

#include "svgamodes.h"

// Block 0 register indices; block I/O reaches index N at BAR1 + 4 * N.
#define M64_CRTC_H_TOTAL_DISP       0x00
#define M64_CRTC_H_SYNC_STRT_WID    0x01
#define M64_CRTC_V_TOTAL_DISP       0x02
#define M64_CRTC_V_SYNC_STRT_WID    0x03
#define M64_CRTC_OFF_PITCH          0x05
#define M64_CRTC_GEN_CNTL           0x07
#define M64_SCRATCH_REG1            0x21
#define M64_CLOCK_CNTL              0x24
#define M64_LCD_INDEX               0x29
#define M64_LCD_DATA                0x2a
#define M64_MEM_CNTL                0x2c
#define M64_DAC_CNTL                0x31
#define M64_CONFIG_CNTL             0x37
#define M64_CONFIG_CHIP_ID          0x38
#define M64_CONFIG_STAT0            0x39
#define M64_DST_OFF_PITCH           0x40
#define M64_DST_Y_X                 0x43
#define M64_DST_HEIGHT_WIDTH        0x46
#define M64_DST_CNTL                0x4c
#define M64_SRC_CNTL                0x6d
#define M64_SC_LEFT_RIGHT           0xaa
#define M64_SC_TOP_BOTTOM           0xad
#define M64_DP_FRGD_CLR             0xb1
#define M64_DP_WRITE_MASK           0xb2
#define M64_DP_PIX_WIDTH            0xb4
#define M64_DP_MIX                  0xb5
#define M64_DP_SRC                  0xb6
#define M64_CLR_CMP_CNTL            0xc2
#define M64_FIFO_STAT               0xc4
#define M64_GUI_STAT                0xce

#define GEN_DBL_SCAN_EN         0x00000001
#define GEN_INTERLACE_EN        0x00000002
#define GEN_HSYNC_DIS           0x00000004
#define GEN_VSYNC_DIS           0x00000008
#define GEN_DISPLAY_DIS         0x00000040
#define GEN_PIX_WIDTH           0x00000700
#define GEN_EXT_DISP_EN         0x01000000
#define GEN_EN                  0x02000000
#define SYNC_POL                0x00200000
#define DAC_8BIT_EN             0x00000100
#define CFG_MEM_VGA_AP_EN       0x00000004
#define PLL_WR_EN               0x00000200
#define GUI_ACTIVE              0x00000001

// Internal PLL registers (RRG Table 4-6)
#define M64_PLL_REF_DIV             0x02
#define M64_VCLK_POST_DIV           0x06
#define M64_VCLK3_FB_DIV            0x0a
#define M64_PLL_EXT_CNTL            0x0b
#define ALT_VCLK3_POST          0x80

// LCD register 7 carries the CRT DDC lines of the RAGE XL.
#define LCD_DDC                 0x07
#define DDC_SDA                 0x20
#define DDC_SCL                 0x40
#define DDC_SDA_IN              0x00002000

#define PIX_8BPP                2
#define PIX_15BPP               3
#define PIX_16BPP               4
#define PIX_24BPP               5
#define PIX_32BPP               6

// The clock table of vgaentry.S; the POST does not reprogram the PLL.
#define MACH_REF_FREQ           2950
#define MACH_PCLK_MAX           23000

static u16 mach_io VAR16;

extern u16 mach_rom_pciloc, mach_rom_iobase;
extern u32 mach_rom_mmbase;
extern u8 mach_clock_table, mach_pclk_table;


/****************************************************************
 * Register access
 ****************************************************************/

static u32
mach_in(u8 reg)
{
    return inl(GET_GLOBAL(mach_io) + reg * 4);
}

static void
mach_out(u8 reg, u32 val)
{
    outl(val, GET_GLOBAL(mach_io) + reg * 4);
}

static void
mach_mask(u8 reg, u32 off, u32 on)
{
    mach_out(reg, (mach_in(reg) & ~off) | on);
}

static void
mach_pll_write(u8 reg, u8 val)
{
    u32 cc = mach_in(M64_CLOCK_CNTL) & 0x03;
    mach_out(M64_CLOCK_CNTL, cc | ((u32)reg << 10) | PLL_WR_EN | ((u32)val << 16));
    mach_out(M64_CLOCK_CNTL, cc);
}

static u8
mach_pll_read(u8 reg)
{
    u32 cc = mach_in(M64_CLOCK_CNTL) & 0x03;
    mach_out(M64_CLOCK_CNTL, cc | ((u32)reg << 10));
    return mach_in(M64_CLOCK_CNTL) >> 16;
}

// No emulator path waits here, so the bounds only guard real hardware.
static void
mach_wait_idle(void)
{
    int i;
    for (i = 0; i < 100000; i++)
        if (!(mach_in(M64_FIFO_STAT) & 0xffff) && !(mach_in(M64_GUI_STAT) & GUI_ACTIVE))
            return;
}


/****************************************************************
 * Modes
 ****************************************************************/

static int
is_mach_mode(struct vgamode_s *vmode_g)
{
    unsigned int mcount = GET_GLOBAL(svga_mcount);

    return (vmode_g >= &svga_modes[0].info &&
            vmode_g <= &svga_modes[mcount-1].info);
}

struct vgamode_s *
mach_find_mode(int mode)
{
    struct generic_svga_mode *m = svga_modes;
    unsigned int mcount = GET_GLOBAL(svga_mcount);

    if (GET_GLOBAL(mach_io))
        for (; m < &svga_modes[mcount]; m++)
            if (GET_GLOBAL(m->mode) == mode)
                return &m->info;
    return stdvga_find_mode(mode);
}

void
mach_list_modes(u16 seg, u16 *dest, u16 *last)
{
    struct generic_svga_mode *m = svga_modes;
    unsigned int mcount = GET_GLOBAL(svga_mcount);

    if (GET_GLOBAL(mach_io)) {
        for (; m < &svga_modes[mcount] && dest<last; m++) {
            u16 mode = GET_GLOBAL(m->mode);
            if (mode == 0xffff)
                continue;
            SET_FARVAR(seg, *dest, mode);
            dest++;
        }
    }
    stdvga_list_modes(seg, dest, last);
}

static u8
mach_pix_width(int depth)
{
    switch (depth) {
    case 8:  return PIX_8BPP;
    case 15: return PIX_15BPP;
    case 16: return PIX_16BPP;
    case 24: return PIX_24BPP;
    default: return PIX_32BPP;
    }
}

static int
mach_bytes(u8 pixw)
{
    switch (pixw) {
    case PIX_8BPP:  return 1;
    case PIX_24BPP: return 3;
    case PIX_32BPP: return 4;
    default:        return 2;
    }
}


/****************************************************************
 * Line length, display start, DAC width
 ****************************************************************/

static int
mach_cur_bytes(void)
{
    return mach_bytes((mach_in(M64_CRTC_GEN_CNTL) & GEN_PIX_WIDTH) >> 8);
}

int
mach_get_linelength(struct vgamode_s *curmode_g)
{
    if (!is_mach_mode(curmode_g))
        return stdvga_get_linelength(curmode_g);
    return (mach_in(M64_CRTC_OFF_PITCH) >> 22) * 8 * mach_cur_bytes();
}

int
mach_set_linelength(struct vgamode_s *curmode_g, int val)
{
    if (!is_mach_mode(curmode_g))
        return stdvga_set_linelength(curmode_g, val);
    u32 pitch = DIV_ROUND_UP(val, 8 * mach_cur_bytes());
    if (pitch > 0x3ff)
        return -1;
    mach_mask(M64_CRTC_OFF_PITCH, 0xffc00000, pitch << 22);
    return 0;
}

int
mach_get_displaystart(struct vgamode_s *curmode_g)
{
    if (!is_mach_mode(curmode_g))
        return stdvga_get_displaystart(curmode_g);
    return (mach_in(M64_CRTC_OFF_PITCH) & 0xfffff) * 8;
}

int
mach_set_displaystart(struct vgamode_s *curmode_g, int val)
{
    if (!is_mach_mode(curmode_g))
        return stdvga_set_displaystart(curmode_g, val);
    if (val < 0 || val / 8 > 0xfffff)
        return -1;
    mach_mask(M64_CRTC_OFF_PITCH, 0xfffff, val / 8);
    return 0;
}

int
mach_get_dacformat(struct vgamode_s *curmode_g)
{
    if (!is_mach_mode(curmode_g))
        return stdvga_get_dacformat(curmode_g);
    return (mach_in(M64_DAC_CNTL) & DAC_8BIT_EN) ? 8 : 6;
}

int
mach_set_dacformat(struct vgamode_s *curmode_g, int val)
{
    if (!is_mach_mode(curmode_g))
        return stdvga_set_dacformat(curmode_g, val);
    if (val != 6 && val != 8)
        return -1;
    mach_mask(M64_DAC_CNTL, DAC_8BIT_EN, val == 8 ? DAC_8BIT_EN : 0);
    return 0;
}


/****************************************************************
 * Save and restore
 ****************************************************************/

static u8 mach_sr_regs[] VAR16 = {
    M64_CRTC_H_TOTAL_DISP, M64_CRTC_H_SYNC_STRT_WID, M64_CRTC_V_TOTAL_DISP,
    M64_CRTC_V_SYNC_STRT_WID, M64_CRTC_OFF_PITCH, M64_DAC_CNTL, M64_CONFIG_CNTL,
    M64_CRTC_GEN_CNTL,
};

static void
mach_sr(int save, u16 seg, u32 *data)
{
    int i;
    for (i = 0; i < ARRAY_SIZE(mach_sr_regs); i++) {
        u8 reg = GET_GLOBAL(mach_sr_regs[i]);
        if (save)
            SET_FARVAR(seg, data[i], mach_in(reg));
        else
            mach_out(reg, GET_FARVAR(seg, data[i]));
    }
}

int
mach_save_restore(int cmd, u16 seg, void *data)
{
    int ret = stdvga_save_restore(cmd, seg, data);
    if (ret < 0 || !(cmd & SR_REGISTERS) || !GET_GLOBAL(mach_io))
        return ret;
    if (cmd & (SR_SAVE | SR_RESTORE))
        mach_sr(cmd & SR_SAVE, seg, data + ret);
    return ret + ARRAY_SIZE(mach_sr_regs) * 4;
}


/****************************************************************
 * Mode setting
 ****************************************************************/

// CRTC parameters in the layout of PRG Table A-9 (coprocessor form), which
// is what A002 receives (WSRV03/drivers/video/ms/ati/mini/init_cx.h:103).
struct mach_crtc {
    u16 res1;
    u16 mode_select;
    u16 flags;
    u8 h_total, h_disp;
    u8 h_sync_strt, h_sync_wid;
    u16 v_total, v_disp, v_sync_strt;
    u8 v_sync_wid, clock_cntl;
    u16 dot_clock;
    u16 h_overscan, v_overscan, overscan_8b, overscan_gr;
    u16 res2;
} PACKED;

#define CRTC_FLAG_DBLSCAN       0x0100
#define CRTC_FLAG_INTERLACE     0x0200
#define CRTC_FLAG_HPOL_NEG      0x4000
#define CRTC_FLAG_VPOL_NEG      0x8000

// VESA DMT: width, height, pixel clock (10 kHz), horizontal front porch,
// sync, back porch, vertical front porch, sync, back porch, flags.
struct mach_dmt {
    u16 w, h, clock;
    u16 hfp, hs, hbp;
    u8 vfp, vs, vbp;
    u16 flags;
};

static struct mach_dmt mach_dmt_modes[] VAR16 = {
    {  640,  480,  2518, 16,  96,  48, 10, 2, 33,
       CRTC_FLAG_HPOL_NEG | CRTC_FLAG_VPOL_NEG },
    {  800,  600,  4000, 40, 128,  88,  1, 4, 23, 0 },
    { 1024,  768,  6500, 24, 136, 160,  3, 6, 29,
      CRTC_FLAG_HPOL_NEG | CRTC_FLAG_VPOL_NEG },
    { 1152,  864, 10800, 64, 128, 256,  1, 3, 32, 0 },
    { 1280, 1024, 10800, 48, 112, 248,  1, 3, 38, 0 },
    { 1600, 1200, 16200, 64, 192, 304,  1, 3, 46, 0 },
};

// A DMT timing where one exists, otherwise blanking of a quarter of the
// width and a twentieth of the height at 60 Hz.
static void
mach_timing(struct mach_crtc *t, u16 w, u16 h)
{
    u16 hfp = 16, hs = w / 8, hbp = w / 8, vfp = 3, vs = 4, vbp = h / 20;
    u32 clock = 0;
    u16 flags = 0;
    int i;

    for (i = 0; i < ARRAY_SIZE(mach_dmt_modes); i++) {
        struct mach_dmt *d = &mach_dmt_modes[i];
        if (GET_GLOBAL(d->w) != w || GET_GLOBAL(d->h) != h)
            continue;
        hfp = GET_GLOBAL(d->hfp);
        hs = GET_GLOBAL(d->hs);
        hbp = GET_GLOBAL(d->hbp);
        vfp = GET_GLOBAL(d->vfp);
        vs = GET_GLOBAL(d->vs);
        vbp = GET_GLOBAL(d->vbp);
        clock = GET_GLOBAL(d->clock);
        flags = GET_GLOBAL(d->flags);
        break;
    }
    hs = ALIGN(hs, 8);
    u16 htotal = w + hfp + hs + ALIGN(hbp, 8);
    u16 vtotal = h + vfp + vs + vbp;
    if (!clock)
        clock = (u32)htotal * vtotal * 60 / 10000;

    memset(t, 0, sizeof(*t));
    t->flags = flags;
    t->h_total = htotal / 8 - 1;
    t->h_disp = w / 8 - 1;
    t->h_sync_strt = (w + hfp) / 8 - 1;
    t->h_sync_wid = hs / 8;
    t->v_total = vtotal - 1;
    t->v_disp = h - 1;
    t->v_sync_strt = h + vfp - 1;
    t->v_sync_wid = vs;
    t->clock_cntl = 0xff;
    t->dot_clock = clock;
}

// VCLK = 2 * XTALIN * FB / (M64_PLL_REF_DIV * post); take the largest post
// divider that keeps FB in 8 bits, for the highest VCO frequency.
static void
mach_set_clock(u16 clock)
{
    static u8 post_sel[] VAR16 = { 3, 2, 1, 0 };
    u32 refdiv = mach_pll_read(M64_PLL_REF_DIV);
    int i;

    if (!clock || clock > MACH_PCLK_MAX || !refdiv)
        return;
    for (i = 0; i < ARRAY_SIZE(post_sel); i++) {
        u8 sel = GET_GLOBAL(post_sel[i]);
        u32 fb = DIV_ROUND_CLOSEST((u32)clock * (1 << sel) * refdiv,
                                   2 * MACH_REF_FREQ);
        if (fb > 0xff)
            continue;
        u8 post = mach_pll_read(M64_VCLK_POST_DIV);
        mach_pll_write(M64_VCLK_POST_DIV, (post & 0x3f) | (sel << 6));
        mach_pll_write(M64_PLL_EXT_CNTL,
                       mach_pll_read(M64_PLL_EXT_CNTL) & ~ALT_VCLK3_POST);
        mach_pll_write(M64_VCLK3_FB_DIV, fb);
        mach_mask(M64_CLOCK_CNTL, 0x03, 0x03);
        return;
    }
}

// Fill with the drawing engine: INT 15h is unusable in the Windows and the
// zx1 x86 emulators, so the frame buffer is not written through memcpy_high.
static void
mach_fill(u32 offset, u16 pitch, u8 pixw, u16 width, u16 height)
{
    if (pixw == PIX_24BPP) {
        pitch *= 3;
        width *= 3;
        pixw = PIX_8BPP;
    }
    mach_wait_idle();
    mach_out(M64_DST_OFF_PITCH, offset / 8 | (u32)(pitch / 8) << 22);
    mach_out(M64_DP_PIX_WIDTH, pixw | pixw << 8 | pixw << 16);
    mach_out(M64_DP_WRITE_MASK, 0xffffffff);
    mach_out(M64_DP_FRGD_CLR, 0);
    mach_out(M64_DP_MIX, 0x00070007);
    mach_out(M64_DP_SRC, 0x00000100);
    mach_out(M64_CLR_CMP_CNTL, 0);
    mach_out(M64_SRC_CNTL, 0);
    mach_out(M64_SC_LEFT_RIGHT, 0x1fff0000);
    mach_out(M64_SC_TOP_BOTTOM, 0x7fff0000);
    mach_out(M64_DST_CNTL, 0x03);
    mach_out(M64_DST_Y_X, 0);
    mach_out(M64_DST_HEIGHT_WIDTH, (u32)width << 16 | height);
    mach_wait_idle();
}

// Program the accelerator CRTC from a PRG Table A-9 parameter set.
static void
mach_load_crtc(struct mach_crtc *t, u8 pixw, u16 pitch)
{
    u32 hpol = (t->flags & CRTC_FLAG_HPOL_NEG) ? SYNC_POL : 0;
    u32 vpol = (t->flags & CRTC_FLAG_VPOL_NEG) ? SYNC_POL : 0;
    u32 gen = mach_in(M64_CRTC_GEN_CNTL) & ~(GEN_PIX_WIDTH | GEN_DBL_SCAN_EN
                                         | GEN_INTERLACE_EN);

    mach_out(M64_CRTC_GEN_CNTL, gen | GEN_DISPLAY_DIS);
    mach_out(M64_CRTC_H_TOTAL_DISP, t->h_total | (u32)t->h_disp << 16);
    mach_out(M64_CRTC_H_SYNC_STRT_WID, t->h_sync_strt
             | (u32)(t->h_sync_wid & 0x1f) << 16 | hpol);
    mach_out(M64_CRTC_V_TOTAL_DISP, (t->v_total & 0x7ff)
             | (u32)(t->v_disp & 0x7ff) << 16);
    mach_out(M64_CRTC_V_SYNC_STRT_WID, (t->v_sync_strt & 0x7ff)
             | (u32)(t->v_sync_wid & 0x1f) << 16 | vpol);
    mach_out(M64_CRTC_OFF_PITCH, (u32)(pitch / 8) << 22);
    if (t->clock_cntl == 0xff)
        mach_set_clock(t->dot_clock);
    if (t->flags & CRTC_FLAG_DBLSCAN)
        gen |= GEN_DBL_SCAN_EN;
    if (t->flags & CRTC_FLAG_INTERLACE)
        gen |= GEN_INTERLACE_EN;
    mach_out(M64_CRTC_GEN_CNTL, gen | (u32)pixw << 8);
}

static void
mach_enable(int ext)
{
    u32 gen = mach_in(M64_CRTC_GEN_CNTL) & ~(GEN_DISPLAY_DIS | GEN_HSYNC_DIS
                                         | GEN_VSYNC_DIS);
    if (ext)
        gen |= GEN_EXT_DISP_EN | GEN_EN;
    else
        gen &= ~GEN_EXT_DISP_EN;
    mach_out(M64_CRTC_GEN_CNTL, gen);
}

static void
mach_leave_ext(void)
{
    mach_enable(0);
    mach_mask(M64_DAC_CNTL, DAC_8BIT_EN, 0);
    mach_mask(M64_CONFIG_CNTL, CFG_MEM_VGA_AP_EN, 0);
}

static int
mach_ext_mode(struct vgamode_s *vmode_g, int flags)
{
    u16 width = GET_GLOBAL(vmode_g->width);
    u16 height = GET_GLOBAL(vmode_g->height);
    u8 depth = GET_GLOBAL(vmode_g->depth);
    u8 pixw = mach_pix_width(depth);
    struct mach_crtc t;

    dprintf(1, "%s: %dx%d-%d\n", __func__, width, height, depth);
    if (GET_GLOBAL(vmode_g->memmodel) == MM_PACKED && !(flags & MF_NOPALETTE))
        stdvga_set_packed_palette();

    mach_timing(&t, width, height);
    mach_load_crtc(&t, pixw, width);
    mach_mask(M64_DAC_CNTL, DAC_8BIT_EN, 0);
    if (!(flags & MF_NOCLEARMEM))
        mach_fill(0, width, pixw, width, height);
    mach_enable(1);
    return 0;
}

int
mach_set_mode(struct vgamode_s *vmode_g, int flags)
{
    if (!GET_GLOBAL(mach_io))
        return stdvga_set_mode(vmode_g, flags);
    if (is_mach_mode(vmode_g))
        return mach_ext_mode(vmode_g, flags);
    mach_leave_ext();
    return stdvga_set_mode(vmode_g, flags);
}


/****************************************************************
 * ATI extended functions (INT 10h AH=A0h, PRG App. A)
 ****************************************************************/

// Built-in resolutions of functions 00h/02h, CH
static struct { u8 code; u16 w, h; } mach_res_codes[] VAR16 = {
    { 0x12, 640, 480 }, { 0x6a, 800, 600 }, { 0x55, 1024, 768 },
    { 0x83, 1280, 1024 }, { 0x84, 1600, 1200 }, { 0xe1, 640, 400 },
    { 0xe3, 320, 240 }, { 0xe4, 512, 384 }, { 0xe5, 400, 300 },
};

// Functions 00h and 02h.  A caller's table can lie in the VGA window
// (both miniports pass BF00:0000), so it is copied before the first
// register write.
static int
mach_a0_load(struct bregs *regs, int enable)
{
    struct mach_crtc t;
    // The depth codes of CL[2:0] are the CRTC_PIX_WIDTH codes.
    u8 pixw = regs->cl & 0x07;
    int i;

    if (pixw == 0 || pixw > PIX_32BPP)
        return -1;
    if (regs->ch == 0x81) {
        memcpy_far(GET_SEG(SS), &t, regs->dx, (void*)(u32)regs->bx, sizeof(t));
    } else {
        for (i = 0; i < ARRAY_SIZE(mach_res_codes); i++)
            if (GET_GLOBAL(mach_res_codes[i].code) == regs->ch)
                break;
        if (i == ARRAY_SIZE(mach_res_codes))
            return -1;
        mach_timing(&t, GET_GLOBAL(mach_res_codes[i].w),
                    GET_GLOBAL(mach_res_codes[i].h));
    }

    u16 width = (t.h_disp + 1) * 8;
    u16 pitch = width;
    if (!(regs->cl & 0xc0))
        pitch = 1024 > width ? 1024 : width;
    else if ((regs->cl & 0xc0) == 0x40)
        pitch = (mach_in(M64_CRTC_OFF_PITCH) >> 22) * 8;
    mach_load_crtc(&t, pixw, pitch);
    if (enable) {
        mach_mask(M64_DAC_CNTL, DAC_8BIT_EN,
                  (pixw > PIX_8BPP || (regs->cl & 0x10)) ? DAC_8BIT_EN : 0);
        mach_enable(1);
    }
    return 0;
}

// Function 09h: the query header of PRG Table A-6, straight into DX:BX; the
// caller's buffer may be in the VGA window, so the memory map stays as it is.
static void
mach_a0_query(struct bregs *regs)
{
    u16 seg = regs->dx;
    u8 *q = (void*)(u32)regs->bx;
    u32 kb = GET_GLOBAL(VBE_total_memory) / 1024;
    u8 memidx = kb <= 512 ? 0 : kb <= 1024 ? 1 : kb <= 2048 ? 2 : kb <= 4096 ? 3
        : kb <= 6144 ? 4 : kb <= 8192 ? 5 : kb <= 12288 ? 6 : 7;
    int i;

    for (i = 0; i < 0x20; i++)
        SET_FARVAR(seg, q[i], 0);
    SET_FARVAR(seg, *(u16*)&q[0x00], 0x20);
    SET_FARVAR(seg, q[0x02], 1);
    SET_FARVAR(seg, *(u16*)&q[0x04], 0x20);
    SET_FARVAR(seg, q[0x06], 0x24);
    SET_FARVAR(seg, q[0x07], 1);
    SET_FARVAR(seg, *(u16*)&q[0x08], mach_in(M64_CONFIG_CHIP_ID));
    SET_FARVAR(seg, q[0x0b], memidx);
    SET_FARVAR(seg, q[0x0d], mach_in(M64_CONFIG_STAT0) & 0x07);
    SET_FARVAR(seg, q[0x0e], 7);
    SET_FARVAR(seg, *(u16*)&q[0x10], GET_GLOBAL(VBE_framebuffer) >> 20);
    SET_FARVAR(seg, q[0x12], 2);
    SET_FARVAR(seg, q[0x13], 0x87);
    SET_FARVAR(seg, q[0x15], 1);
    SET_FARVAR(seg, *(u16*)&q[0x18], GET_GLOBAL(mach_io));
}

void
mach_a0(struct bregs *regs)
{
    u32 gen;

    if (!GET_GLOBAL(mach_io)) {
        regs->ah = 2;
        return;
    }
    switch (regs->al) {
    case 0x00:
    case 0x02:
        regs->ah = mach_a0_load(regs, regs->al == 0x02) ? 1 : 0;
        return;
    case 0x01:
        if (regs->cl & 1) {
            mach_mask(M64_DAC_CNTL, DAC_8BIT_EN, (regs->cl & 0x80) ? DAC_8BIT_EN : 0);
            mach_enable(1);
        } else {
            mach_leave_ext();
        }
        regs->cl &= ~0x20;
        break;
    case 0x05:
        // The linear aperture is always on (PCI BAR0).
        mach_mask(M64_CONFIG_CNTL, CFG_MEM_VGA_AP_EN,
                  (regs->cl & 0x04) ? CFG_MEM_VGA_AP_EN : 0);
        break;
    case 0x06:
        regs->al = 2;
        regs->bx = GET_GLOBAL(VBE_framebuffer) >> 20;
        regs->ch = 0x87;
        regs->cl = GET_GLOBAL(VBE_total_memory) >= 8 * 1024 * 1024 ? 5 : 3;
        regs->dx = mach_in(M64_CONFIG_CHIP_ID);
        break;
    case 0x08:
        regs->cx = 0x20;
        break;
    case 0x09:
        mach_a0_query(regs);
        break;
    case 0x0a:
        regs->cl = 4;
        regs->dx = get_global_seg();
        regs->bx = (u32)&mach_pclk_table;
        regs->cx = (u32)&mach_clock_table + 1;
        break;
    case 0x0c:
        switch (regs->cl) {
        case 1:  gen = GEN_HSYNC_DIS; break;
        case 2:  gen = GEN_VSYNC_DIS; break;
        case 3:  gen = GEN_HSYNC_DIS | GEN_VSYNC_DIS; break;
        case 4:  gen = GEN_DISPLAY_DIS; break;
        default: gen = 0; break;
        }
        mach_mask(M64_CRTC_GEN_CNTL, GEN_DISPLAY_DIS | GEN_HSYNC_DIS
                  | GEN_VSYNC_DIS, gen);
        break;
    case 0x0d:
        gen = mach_in(M64_CRTC_GEN_CNTL);
        regs->cl = ((gen & GEN_HSYNC_DIS) ? 1 : 0) | ((gen & GEN_VSYNC_DIS) ? 2 : 0);
        break;
    case 0x14:
        if (regs->cl == 0) {
            regs->cx = ARRAY_SIZE(mach_sr_regs) * 4;
            regs->bx = 0;
        } else if (regs->cl == 1 || regs->cl == 2) {
            mach_sr(regs->cl == 1, regs->dx, (void*)(u32)regs->di);
        } else {
            regs->ah = 1;
            return;
        }
        break;
    case 0x70:
        // No TV-out: the miniports look for BX = 5442h ("TB").
        regs->bx = 0;
        regs->ah = 2;
        return;
    case 0x84:
        if (regs->bh == 0) {
            regs->bh = 2;
            regs->cl = 2;
        }
        regs->bl = 2;
        break;
    default:
        // Also 16h: AL stays non-zero, so the caller finds no feature table.
        regs->ah = 2;
        return;
    }
    regs->ah = 0;
}


/****************************************************************
 * EDID
 ****************************************************************/

// SCL and SDA are open-drain: a line is driven low by making it an
// output (M64_LCD_DATA byte 3), with its state bit (byte 1) left at 0.
static void
mach_ddc_set(int scl, int sda)
{
    outb((scl ? 0 : DDC_SCL) | (sda ? 0 : DDC_SDA),
         GET_GLOBAL(mach_io) + M64_LCD_DATA * 4 + 3);
}

static int
mach_ddc_get_sda(void)
{
    return (mach_in(M64_LCD_DATA) & DDC_SDA_IN) != 0;
}

static void
mach_ddc_start(void)
{
    mach_ddc_set(1, 1);
    mach_ddc_set(1, 0);
    mach_ddc_set(0, 0);
}

static void
mach_ddc_stop(void)
{
    mach_ddc_set(0, 0);
    mach_ddc_set(1, 0);
    mach_ddc_set(1, 1);
}

// Returns 0 when the slave acknowledged.
static int
mach_ddc_send(u8 byte)
{
    int i;
    for (i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        mach_ddc_set(0, bit);
        mach_ddc_set(1, bit);
        mach_ddc_set(0, bit);
    }
    mach_ddc_set(0, 1);
    mach_ddc_set(1, 1);
    int nack = mach_ddc_get_sda();
    mach_ddc_set(0, 1);
    return nack;
}

static u8
mach_ddc_recv(int ack)
{
    u8 byte = 0;
    int i;
    for (i = 0; i < 8; i++) {
        mach_ddc_set(0, 1);
        mach_ddc_set(1, 1);
        byte = (byte << 1) | mach_ddc_get_sda();
    }
    mach_ddc_set(0, !ack);
    mach_ddc_set(1, !ack);
    mach_ddc_set(0, !ack);
    return byte;
}

static int
mach_read_edid(void)
{
    int i, ok = 0;

    mach_out(M64_LCD_INDEX, LCD_DDC);
    outb(0, GET_GLOBAL(mach_io) + M64_LCD_DATA * 4 + 1);
    mach_ddc_set(1, 1);
    mach_ddc_start();
    if (mach_ddc_send(0x50 << 1) || mach_ddc_send(0x00))
        goto out;
    mach_ddc_set(0, 1);
    mach_ddc_set(1, 1);
    mach_ddc_start();
    if (mach_ddc_send(0x50 << 1 | 1))
        goto out;
    for (i = 0; i < 128; i++)
        SET_VGA(VBE_edid[i], mach_ddc_recv(i < 127));
    ok = (GET_GLOBAL(VBE_edid[0]) == 0x00 && GET_GLOBAL(VBE_edid[1]) == 0xff);
out:
    mach_ddc_stop();
    return ok;
}


/****************************************************************
 * Init
 ****************************************************************/

// INT 1Ah B102h, for a POST entered without the location in AX.  The zx1
// interpreter leaves AH = B1h on success, so only CF counts.
static int
mach_pcibios_find(void)
{
    u16 ax = 0xb102, cx = 0x4752, dx = 0x1002;
    asm volatile("int $0x1a\n\t"
                 "movw %%bx, %%cx\n\t"
                 "pushfw\n\t"
                 "popw %%dx"
                 : "+a"(ax), "+c"(cx), "+d"(dx)
                 : "S"((u16)0)
                 : "cc", "memory");
    return (dx & 1) ? -1 : cx;
}

// M64_MEM_CNTL MEM_SIZE: half-MiB steps up to 7, MiB steps to 11, then 2 MiB.
static u32
mach_vram_size(void)
{
    u32 v = mach_in(M64_MEM_CNTL) & 0x0f;
    if (v < 8)
        return (v + 1) * 512 * 1024;
    if (v < 12)
        return (v - 3) * 1024 * 1024;
    return (v - 7) * 2 * 1024 * 1024;
}

int
mach_setup(void)
{
    int ret = stdvga_setup();
    if (ret)
        return ret;

    if (GET_GLOBAL(HaveRunInit))
        return 0;

    int bdf = GET_GLOBAL(VgaBDF);
    if (bdf < 0)
        bdf = mach_pcibios_find();
    if (!CONFIG_VGA_PCI || bdf < 0) {
        dprintf(1, "mach: no PCI location\n");
        return 0;
    }
    SET_VGA(VgaBDF, bdf);

    // Only dword configuration reads: the zx1 interpreter fails word reads.
    u32 lfb = pci_config_readl(bdf, PCI_BASE_ADDRESS_0) & PCI_BASE_ADDRESS_MEM_MASK;
    u16 io = pci_config_readl(bdf, PCI_BASE_ADDRESS_1) & PCI_BASE_ADDRESS_IO_MASK;
    u32 mmio = pci_config_readl(bdf, PCI_BASE_ADDRESS_2) & PCI_BASE_ADDRESS_MEM_MASK;

    SET_VGA(mach_io, io);
    u16 chip = io ? mach_in(M64_CONFIG_CHIP_ID) : 0;
    if (chip != 0x4752) {
        dprintf(1, "mach: no chip at io 0x%x (id 0x%x)\n", io, chip);
        SET_VGA(mach_io, 0);
        return 0;
    }

    u32 vram = mach_vram_size();
    dprintf(1, "mach: bdf %02x:%02x.%x, lfb 0x%x, %d KB, io 0x%x, mmio 0x%x\n",
            pci_bdf_to_bus(bdf), pci_bdf_to_dev(bdf), pci_bdf_to_fn(bdf),
            lfb, vram / 1024, io, mmio);

    SET_VGA(VBE_framebuffer, lfb);
    SET_VGA(VBE_total_memory, vram);
    SET_VGA(VBE_capabilities, VBE_CAPABILITY_8BIT_DAC);
    SET_VGA(mach_rom_pciloc, bdf);
    SET_VGA(mach_rom_iobase, io);
    SET_VGA(mach_rom_mmbase, mmio);
    mach_out(M64_SCRATCH_REG1, 0);      // ROM at C000:0 (PRG App. A.1)
    mach_leave_ext();

    struct generic_svga_mode *m = svga_modes;
    unsigned int mcount = GET_GLOBAL(svga_mcount);
    for (; m < &svga_modes[mcount]; m++) {
        u8 memmodel = GET_GLOBAL(m->info.memmodel);
        u8 depth = GET_GLOBAL(m->info.depth);
        u16 width = GET_GLOBAL(m->info.width);
        u16 height = GET_GLOBAL(m->info.height);
        u32 linelength = width * DIV_ROUND_UP(depth, 8);

        if (!((memmodel == MM_PACKED && depth == 8) || memmodel == MM_DIRECT) ||
            width % 8 != 0 ||
            width / 8 - 1 > 0x1ff ||
            height - 1 > 0x7ff ||
            width / 8 > 0x3ff ||
            linelength * height > vram) {
            dprintf(3, "mach: removing mode 0x%x\n", GET_GLOBAL(m->mode));
            SET_VGA(m->mode, 0xffff);
        }
    }

    int edid_ok = mach_read_edid();
    dprintf(1, "mach: edid %s\n", edid_ok ? "good" : "invalid");
    return 0;
}
