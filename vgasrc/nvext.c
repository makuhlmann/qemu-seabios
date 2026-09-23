// QEMU NVIDIA NV15 (GeForce2 / Quadro2 Pro) VGABIOS extension.
//
// Written for the nv15gl-vga model of qemu-system-ia64 (hw/display/geforce.c).
// Register layouts follow that model and xf86-video-nv (MIT).
//
// This file may be distributed under the terms of the GNU LGPLv3 license.

#include "biosvar.h" // GET_GLOBAL
#include "hw/pci.h" // pci_config_readl
#include "hw/pci_ids.h" // PCI_VENDOR_ID_NVIDIA
#include "hw/pci_regs.h" // PCI_BASE_ADDRESS_0
#include "output.h" // dprintf
#include "stdvga.h" // stdvga_crtc_write
#include "string.h" // memset
#include "vgabios.h" // SET_VGA
#include "vgafb.h" // memcpy_high
#include "vgautil.h" // VBE_total_memory
#include "x86.h" // outw

#include "svgamodes.h"

// Extended CRTC registers (geforce.c nv_update_mode, nv_svga_write_crtc)
#define NV_CR_REPAINT0      0x19 // [7:5] pitch bits 10:8, [4:0] start bits 20:16
#define NV_CR_EXTRA         0x25 // [1] vertical display end bit 10
#define NV_CR_PIXEL         0x28 // [1:0] 0 = VGA, 1 = 8 bpp, 2 = 16 bpp, 3 = 32 bpp
#define NV_CR_HEB           0x2d // [1] horizontal display end bit 8
#define NV_CR_LOCK          0x1f // 0x57 unlocks the extended registers
#define NV_CR_RMA           0x38 // [0] enable, [2:1] window function
#define NV_CR_VDE_EXT       0x41 // [2] vertical display end bit 11
#define NV_CR_PITCH_EXT     0x42 // [6] pitch bit 11
#define NV_CR_DDC_STATUS    0x3e // [3] SDA in, [2] SCL in
#define NV_CR_DDC_CTRL      0x3f // [5] SCL out, [4] SDA out

#define NV_DDC_SDA_IN       0x08
#define NV_DDC_SCL_OUT      0x20
#define NV_DDC_SDA_OUT      0x10

// Real-mode access window: MMIO through two 16-bit ports (geforce.c nv_svga_read)
#define NV_RMA_PORT_LO      0x3d0
#define NV_RMA_PORT_HI      0x3d2
#define NV_RMA_ADDRESS      0x03
#define NV_RMA_READ         0x05
#define NV_RMA_WRITE        0x07

#define NV_PFB_CSTATUS      0x0010020c // VRAM size in bytes
#define NV_PCRTC_START      0x00600800

#define NV_VRAM_FALLBACK    (16 * 1024 * 1024)

static int nv_found VAR16;

static int
is_nv_mode(struct vgamode_s *vmode_g)
{
    unsigned int mcount = GET_GLOBAL(svga_mcount);

    return (vmode_g >= &svga_modes[0].info &&
            vmode_g <= &svga_modes[mcount-1].info);
}

struct vgamode_s *
nv_find_mode(int mode)
{
    struct generic_svga_mode *m = svga_modes;
    unsigned int mcount = GET_GLOBAL(svga_mcount);

    if (GET_GLOBAL(nv_found))
        for (; m < &svga_modes[mcount]; m++)
            if (GET_GLOBAL(m->mode) == mode)
                return &m->info;
    return stdvga_find_mode(mode);
}

void
nv_list_modes(u16 seg, u16 *dest, u16 *last)
{
    struct generic_svga_mode *m = svga_modes;
    unsigned int mcount = GET_GLOBAL(svga_mcount);

    if (GET_GLOBAL(nv_found)) {
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


/****************************************************************
 * Register access
 ****************************************************************/

static u8
nv_crtc_read(u8 index)
{
    return stdvga_crtc_read(VGAREG_VGA_CRTC_ADDRESS, index);
}

static void
nv_crtc_write(u8 index, u8 value)
{
    stdvga_crtc_write(VGAREG_VGA_CRTC_ADDRESS, index, value);
}

static void
nv_crtc_mask(u8 index, u8 off, u8 on)
{
    stdvga_crtc_mask(VGAREG_VGA_CRTC_ADDRESS, index, off, on);
}

// The model decodes 8- and 16-bit accesses only (geforce.c nv_ioport_ops).
static void
nv_rma_select(u32 reg)
{
    nv_crtc_write(NV_CR_RMA, NV_RMA_ADDRESS);
    outw(reg, NV_RMA_PORT_LO);
    outw(reg >> 16, NV_RMA_PORT_HI);
}

static u32
nv_mmio_read(u32 reg)
{
    u8 cr38 = nv_crtc_read(NV_CR_RMA);
    nv_rma_select(reg);
    nv_crtc_write(NV_CR_RMA, NV_RMA_READ);
    u32 val = inw(NV_RMA_PORT_LO) | ((u32)inw(NV_RMA_PORT_HI) << 16);
    nv_crtc_write(NV_CR_RMA, cr38);
    return val;
}

static void
nv_mmio_write(u32 reg, u32 val)
{
    u8 cr38 = nv_crtc_read(NV_CR_RMA);
    nv_rma_select(reg);
    nv_crtc_write(NV_CR_RMA, NV_RMA_WRITE);
    outw(val, NV_RMA_PORT_LO);
    outw(val >> 16, NV_RMA_PORT_HI);
    nv_crtc_write(NV_CR_RMA, cr38);
}

static void
nv_unlock(void)
{
    nv_crtc_write(NV_CR_LOCK, 0x57);
}


/****************************************************************
 * Line length and display start
 ****************************************************************/

int
nv_get_linelength(struct vgamode_s *curmode_g)
{
    if (!is_nv_mode(curmode_g))
        return stdvga_get_linelength(curmode_g);
    u32 pitch = (nv_crtc_read(0x13)
                 | (nv_crtc_read(NV_CR_REPAINT0) >> 5) << 8
                 | ((nv_crtc_read(NV_CR_PITCH_EXT) >> 6) & 1) << 11);
    return pitch * 8;
}

int
nv_set_linelength(struct vgamode_s *curmode_g, int val)
{
    if (!is_nv_mode(curmode_g))
        return stdvga_set_linelength(curmode_g, val);
    u32 pitch = DIV_ROUND_UP(val, 8);
    if (pitch > 0xfff)
        return -1;
    nv_unlock();
    nv_crtc_write(0x13, pitch);
    nv_crtc_mask(NV_CR_REPAINT0, 0xe0, (pitch >> 3) & 0xe0);
    nv_crtc_mask(NV_CR_PITCH_EXT, 0x40, (pitch >> 5) & 0x40);
    return 0;
}

int
nv_get_displaystart(struct vgamode_s *curmode_g)
{
    if (!is_nv_mode(curmode_g))
        return stdvga_get_displaystart(curmode_g);
    u32 start = (nv_crtc_read(0x0d)
                 | nv_crtc_read(0x0c) << 8
                 | (nv_crtc_read(NV_CR_REPAINT0) & 0x1f) << 16);
    return start * 4;
}

int
nv_set_displaystart(struct vgamode_s *curmode_g, int val)
{
    if (!is_nv_mode(curmode_g))
        return stdvga_set_displaystart(curmode_g, val);
    // The CRTC start field holds 21 bits of dwords.
    u32 start = val / 4;
    if (val < 0 || start > 0x1fffff)
        return -1;
    nv_unlock();
    nv_crtc_write(0x0c, start >> 8);
    nv_crtc_write(0x0d, start);
    nv_crtc_mask(NV_CR_REPAINT0, 0x1f, (start >> 16) & 0x1f);
    return 0;
}


/****************************************************************
 * Save and restore
 ****************************************************************/

static u8 nv_ext_regs[] VAR16 = {
    NV_CR_REPAINT0, NV_CR_EXTRA, NV_CR_PIXEL, NV_CR_HEB,
    NV_CR_VDE_EXT, NV_CR_PITCH_EXT,
};

int
nv_save_restore(int cmd, u16 seg, void *data)
{
    int ret = stdvga_save_restore(cmd, seg, data);
    if (ret < 0 || !(cmd & SR_REGISTERS) || !GET_GLOBAL(nv_found))
        return ret;

    u8 *info = data + ret;
    int i;
    if (cmd & SR_SAVE)
        for (i = 0; i < ARRAY_SIZE(nv_ext_regs); i++)
            SET_FARVAR(seg, info[i], nv_crtc_read(GET_GLOBAL(nv_ext_regs[i])));
    if (cmd & SR_RESTORE) {
        nv_unlock();
        for (i = 0; i < ARRAY_SIZE(nv_ext_regs); i++)
            nv_crtc_write(GET_GLOBAL(nv_ext_regs[i]), GET_FARVAR(seg, info[i]));
    }
    return ret + ARRAY_SIZE(nv_ext_regs);
}


/****************************************************************
 * Mode setting
 ****************************************************************/

// Zero the start of the framebuffer, then double the zeroed area with
// framebuffer-to-framebuffer copies (int 1587 moves at most 64 KiB).
static void
nv_clear(u32 size)
{
    u8 zero[64];
    void *fb = (void*)GET_GLOBAL(VBE_framebuffer);
    u32 done = sizeof(zero), chunk;

    memset(zero, 0, sizeof(zero));
    memcpy_high(fb, MAKE_FLATPTR(GET_SEG(SS), zero), sizeof(zero));
    while (done < size) {
        chunk = done;
        if (chunk > 0x10000)
            chunk = 0x10000;
        if (chunk > size - done)
            chunk = size - done;
        memcpy_high(fb + done, fb, chunk);
        done += chunk;
    }
}

static void
nv_reset_ext(void)
{
    nv_unlock();
    nv_crtc_write(NV_CR_PIXEL, 0);
    nv_crtc_write(NV_CR_REPAINT0, 0);
    nv_crtc_write(NV_CR_EXTRA, 0);
    nv_crtc_write(NV_CR_HEB, 0);
    nv_crtc_write(NV_CR_VDE_EXT, 0);
    nv_crtc_write(NV_CR_PITCH_EXT, 0);
    nv_mmio_write(NV_PCRTC_START, 0);
}

static int
nv_ext_mode(struct vgamode_s *vmode_g, int flags)
{
    u8 memmodel = GET_GLOBAL(vmode_g->memmodel);
    u16 width = GET_GLOBAL(vmode_g->width);
    u16 height = GET_GLOBAL(vmode_g->height);
    u8 depth = GET_GLOBAL(vmode_g->depth);
    u8 pixel = depth == 8 ? 1 : (depth == 16 ? 2 : 3);
    u32 linelength = width * DIV_ROUND_UP(depth, 8);

    dprintf(1, "%s: %dx%d-%d\n", __func__, width, height, depth);

    if (memmodel == MM_PACKED && !(flags & MF_NOPALETTE))
        stdvga_set_packed_palette();

    nv_reset_ext();

    // VGA core: graphics, 256-colour, unchained (as bochsvga_set_mode)
    u16 crtc_addr = VGAREG_VGA_CRTC_ADDRESS;
    stdvga_crtc_write(crtc_addr, 0x11, 0x00);
    stdvga_crtc_write(crtc_addr, 0x09, 0x00);
    stdvga_crtc_mask(crtc_addr, 0x17, 0x00, 0x03);
    stdvga_crtc_mask(crtc_addr, 0x14, 0x00, 0x40);
    stdvga_attr_mask(0x10, 0x00, 0x41);
    stdvga_grdc_write(0x06, 0x05);
    stdvga_grdc_mask(0x05, 0x20, 0x40);
    stdvga_sequ_write(0x02, 0x0f);
    stdvga_sequ_mask(0x04, 0x00, 0x08);

    // Display size (geforce.c nv_update_mode)
    u16 hde = width / 8 - 1;
    u16 vde = height - 1;
    stdvga_crtc_write(crtc_addr, 0x01, hde);
    nv_crtc_mask(NV_CR_HEB, 0x02, (hde >> 7) & 0x02);
    stdvga_set_vertical_size(height);
    nv_crtc_mask(NV_CR_EXTRA, 0x02, (vde >> 9) & 0x02);
    nv_crtc_mask(NV_CR_VDE_EXT, 0x04, (vde >> 9) & 0x04);

    nv_set_linelength(vmode_g, linelength);
    nv_set_displaystart(vmode_g, 0);

    if (!(flags & MF_NOCLEARMEM))
        nv_clear(linelength * height);

    // Scanout switches to the extended format last.
    nv_crtc_write(NV_CR_PIXEL, pixel);
    stdvga_attrindex_write(0x20);
    return 0;
}

int
nv_set_mode(struct vgamode_s *vmode_g, int flags)
{
    if (!GET_GLOBAL(nv_found))
        return stdvga_set_mode(vmode_g, flags);
    if (is_nv_mode(vmode_g))
        return nv_ext_mode(vmode_g, flags);
    nv_reset_ext();
    return stdvga_set_mode(vmode_g, flags);
}


/****************************************************************
 * EDID
 ****************************************************************/

// The model latches CR3E on every CR3F write, SCL before SDA
// (geforce.c nv_svga_write_crtc); change one line per write.
static void
nv_ddc_set(int scl, int sda)
{
    nv_crtc_write(NV_CR_DDC_CTRL, 0x01 | (scl ? NV_DDC_SCL_OUT : 0)
                  | (sda ? NV_DDC_SDA_OUT : 0));
}

static int
nv_ddc_get_sda(void)
{
    return (nv_crtc_read(NV_CR_DDC_STATUS) & NV_DDC_SDA_IN) != 0;
}

static void
nv_ddc_start(void)
{
    nv_ddc_set(0, 1);
    nv_ddc_set(1, 1);
    nv_ddc_set(1, 0);
    nv_ddc_set(0, 0);
}

static void
nv_ddc_stop(void)
{
    nv_ddc_set(0, 0);
    nv_ddc_set(1, 0);
    nv_ddc_set(1, 1);
}

// Returns 0 when the slave acknowledged.
static int
nv_ddc_send(u8 byte)
{
    int i;
    for (i = 7; i >= 0; i--) {
        int bit = (byte >> i) & 1;
        nv_ddc_set(0, bit);
        nv_ddc_set(1, bit);
        nv_ddc_set(0, bit);
    }
    nv_ddc_set(0, 1);
    nv_ddc_set(1, 1);
    int nack = nv_ddc_get_sda();
    nv_ddc_set(0, 1);
    return nack;
}

static u8
nv_ddc_recv(int ack)
{
    u8 byte = 0;
    int i;
    for (i = 0; i < 8; i++) {
        nv_ddc_set(0, 1);
        nv_ddc_set(1, 1);
        byte = (byte << 1) | nv_ddc_get_sda();
    }
    nv_ddc_set(0, 1);
    nv_ddc_set(0, !ack);
    nv_ddc_set(1, !ack);
    nv_ddc_set(0, !ack);
    return byte;
}

static int
nv_read_edid(void)
{
    int i, ok = 0;

    nv_ddc_set(1, 1);
    nv_ddc_start();
    if (nv_ddc_send(0x50 << 1) || nv_ddc_send(0x00))
        goto out;
    nv_ddc_set(0, 1);
    nv_ddc_start();
    if (nv_ddc_send(0x50 << 1 | 1))
        goto out;
    for (i = 0; i < 128; i++)
        SET_VGA(VBE_edid[i], nv_ddc_recv(i < 127));
    ok = (GET_GLOBAL(VBE_edid[0]) == 0x00 && GET_GLOBAL(VBE_edid[1]) == 0xff);
out:
    nv_ddc_stop();
    return ok;
}


/****************************************************************
 * Init
 ****************************************************************/

int
nv_setup(void)
{
    int ret = stdvga_setup();
    if (ret)
        return ret;

    if (GET_GLOBAL(HaveRunInit))
        return 0;

    int bdf = GET_GLOBAL(VgaBDF);
    if (!CONFIG_VGA_PCI || bdf < 0)
        return 0;
    if (pci_config_readw(bdf, PCI_VENDOR_ID) != PCI_VENDOR_ID_NVIDIA) {
        dprintf(1, "nv: bdf %02x:%02x.%x is not an NVIDIA device\n",
                pci_bdf_to_bus(bdf), pci_bdf_to_dev(bdf), pci_bdf_to_fn(bdf));
        return 0;
    }

    u32 mmio = pci_config_readl(bdf, PCI_BASE_ADDRESS_0) & PCI_BASE_ADDRESS_MEM_MASK;
    u32 lfb = pci_config_readl(bdf, PCI_BASE_ADDRESS_1) & PCI_BASE_ADDRESS_MEM_MASK;

    nv_unlock();
    u32 vram = nv_mmio_read(NV_PFB_CSTATUS) & 0xfff00000;
    if (!vram || vram > 256 * 1024 * 1024) {
        dprintf(1, "nv: PFB_CSTATUS gives no VRAM size, assuming %d MB\n",
                NV_VRAM_FALLBACK / (1024 * 1024));
        vram = NV_VRAM_FALLBACK;
    }

    dprintf(1, "nv: bdf %02x:%02x.%x, lfb 0x%x, %d MB, mmio 0x%x\n",
            pci_bdf_to_bus(bdf), pci_bdf_to_dev(bdf), pci_bdf_to_fn(bdf),
            lfb, vram / (1024 * 1024), mmio);

    SET_VGA(VBE_framebuffer, lfb);
    SET_VGA(VBE_total_memory, vram);

    // The scanout knows 8 bpp indexed, RGB565 and XRGB8888 only (CR28 1-3);
    // NV has no 15 bpp or packed 24 bpp scanout on CR28 alone.
    struct generic_svga_mode *m = svga_modes;
    unsigned int mcount = GET_GLOBAL(svga_mcount);
    for (; m < &svga_modes[mcount]; m++) {
        u8 memmodel = GET_GLOBAL(m->info.memmodel);
        u8 depth = GET_GLOBAL(m->info.depth);
        u16 width = GET_GLOBAL(m->info.width);
        u16 height = GET_GLOBAL(m->info.height);
        u32 linelength = width * DIV_ROUND_UP(depth, 8);

        if (!((memmodel == MM_PACKED && depth == 8) ||
              (memmodel == MM_DIRECT && (depth == 16 || depth == 32))) ||
            width % 8 != 0 ||
            width / 8 - 1 > 0x1ff ||
            height - 1 > 0xfff ||
            linelength / 8 > 0xfff ||
            linelength * height > vram) {
            dprintf(3, "nv: removing mode 0x%x\n", GET_GLOBAL(m->mode));
            SET_VGA(m->mode, 0xffff);
        }
    }
    SET_VGA(nv_found, 1);

    dprintf(1, "nv: edid %s\n", nv_read_edid() ? "good" : "invalid");
    return 0;
}
