/*S3 emulation*/
#include <stdlib.h>
#include "ibm.h"
#include "device.h"
#include "io.h"
#include "mem.h"
#include "pci.h"
#include "rom.h"
#include "thread.h"
#include "video.h"
#include "vid_s3.h"
#include "vid_svga.h"
#include "vid_svga_render.h"
#include "vid_sdac_ramdac.h"

enum
{
        S3_VISION864,
        S3_TRIO32,
        S3_TRIO64
};

enum
{
        VRAM_4MB = 0,
        VRAM_8MB = 3,
        VRAM_2MB = 4,
        VRAM_1MB = 6,
        VRAM_512KB = 7
};

#define FIFO_SIZE 65536
#define FIFO_MASK (FIFO_SIZE - 1)
#define FIFO_ENTRY_SIZE (1 << 31)

#define FIFO_ENTRIES (s3->fifo_write_idx - s3->fifo_read_idx)
#define FIFO_FULL    ((s3->fifo_write_idx - s3->fifo_read_idx) >= FIFO_SIZE)
#define FIFO_EMPTY   (s3->fifo_read_idx == s3->fifo_write_idx)

#define FIFO_TYPE 0xff000000
#define FIFO_ADDR 0x00ffffff

enum
{
        FIFO_INVALID     = (0x00 << 24),
        FIFO_WRITE_BYTE  = (0x01 << 24),
        FIFO_WRITE_WORD  = (0x02 << 24),
        FIFO_WRITE_DWORD = (0x03 << 24),
        FIFO_OUT_BYTE    = (0x04 << 24),
        FIFO_OUT_WORD    = (0x05 << 24),
        FIFO_OUT_DWORD   = (0x06 << 24)
};

typedef struct
{
        uint32_t addr_type;
        uint32_t val;
} fifo_entry_t;

typedef struct s3_t
{
        mem_mapping_t linear_mapping;
        mem_mapping_t mmio_mapping;
        
        rom_t bios_rom;
        
        svga_t svga;
        sdac_ramdac_t ramdac;

        uint8_t bank;
        uint8_t ma_ext;
        int width;
        int bpp;

        int chip;
        
        uint8_t id, id_ext, id_ext_pci;
        
        uint8_t int_line;
        
        int packed_mmio;

        uint32_t linear_base, linear_size;
        
        uint8_t pci_regs[256];
        int card;

        uint32_t vram_mask;
        
        float (*getclock)(int clock, void *p);
        void *getclock_p;

        struct
        {
                uint8_t subsys_cntl;
                uint8_t setup_md;
                uint8_t advfunc_cntl;
                uint16_t cur_y, cur_y2;
                uint16_t cur_x, cur_x2;
                uint16_t x2;
                 int16_t desty_axstp, desty_axstp2;
                 int16_t destx_distp;
                 int16_t err_term, err_term2;
                 int16_t maj_axis_pcnt, maj_axis_pcnt2;
                uint16_t cmd;
                uint16_t short_stroke;
                uint32_t bkgd_color;
                uint32_t frgd_color;
                uint32_t wrt_mask;
                uint32_t rd_mask;
                uint32_t color_cmp;
                uint8_t bkgd_mix;
                uint8_t frgd_mix;
                uint16_t multifunc_cntl;
                uint16_t multifunc[16];
                uint8_t pix_trans[4];
        
                int cx, cy;
                int sx, sy;
                int dx, dy;
                uint32_t src, dest, pattern;
                int pix_trans_count;
                
                int poly_cx, poly_cx2;
                int poly_cy, poly_cy2;
                int point_1_updated, point_2_updated;
                int poly_dx1, poly_dx2;
                int poly_x;
        
                uint32_t dat_buf;
                int dat_count;
        } accel;

        fifo_entry_t fifo[FIFO_SIZE];
        volatile int fifo_read_idx, fifo_write_idx;

        volatile int fifo_thread_state;
        thread_t *fifo_thread;
        event_t *wake_fifo_thread;
        event_t *fifo_not_full_event;
        
        int blitter_busy;
        uint64_t blitter_time;
        uint64_t status_time;
        
        uint8_t subsys_cntl, subsys_stat;
        int vblank_irq;
        
        uint32_t hwc_fg_col, hwc_bg_col;
        int hwc_col_stack_pos;

        volatile int force_busy;
} s3_t;

#define INT_VSY      (1 << 0)
#define INT_GE_BSY   (1 << 1)
#define INT_FIFO_OVR (1 << 2)
#define INT_FIFO_EMP (1 << 3)
#define INT_MASK     0xf

void s3_updatemapping(s3_t*);

void s3_accel_write(uint32_t addr, uint8_t val, void *p);
void s3_accel_write_w(uint32_t addr, uint16_t val, void *p);
void s3_accel_write_l(uint32_t addr, uint32_t val, void *p);
uint8_t s3_accel_read(uint32_t addr, void *p);

static inline void wake_fifo_thread(s3_t *s3)
{
        thread_set_event(s3->wake_fifo_thread); /*Wake up FIFO thread if moving from idle*/
}

static void s3_wait_fifo_idle(s3_t *s3)
{
        while (!FIFO_EMPTY)
        {
                wake_fifo_thread(s3);
                thread_wait_event(s3->fifo_not_full_event, 1);
        }
}

static int s3_vga_vsync_enabled(s3_t *s3)
{
    if ((s3->svga.crtc[0x32] & 0x10) && !(s3->svga.crtc[0x11] & 0x20) && s3->vblank_irq > 0)
        return 1;
    return 0;
}

static void s3_update_irqs(s3_t *s3)
{
        int enabled = s3_vga_vsync_enabled(s3);
        if (((s3->subsys_cntl & s3->subsys_stat & INT_MASK) && (s3->svga.crtc[0x32] & 0x10)) || (enabled && (s3->subsys_stat & INT_VSY)))
                pci_set_irq(s3->card, PCI_INTA, NULL);
        else
                pci_clear_irq(s3->card, PCI_INTA, NULL);

        if ((s3->subsys_stat & INT_VSY) && !(s3->subsys_cntl & INT_VSY) && !enabled) {
            s3->subsys_stat &= ~INT_VSY;
        }
}

static void s3_update_irqs_thread(s3_t* s3, int mask)
{
    if ((s3->subsys_cntl & s3->subsys_stat & mask) && (s3->svga.crtc[0x32] & 0x10))
        pci_set_irq(s3->card, PCI_INTA, NULL);
}

void s3_accel_start(int count, int cpu_input, uint32_t mix_dat, uint32_t cpu_dat, s3_t *s3);

#define WRITE8(addr, var, val)  switch ((addr) & 3)                                             \
                                {                                                               \
                                        case 0: var = (var & 0xffffff00) | (val);         break;  \
                                        case 1: var = (var & 0xffff00ff) | ((val) << 8);  break;  \
                                        case 2: var = (var & 0xff00ffff) | ((val) << 16); break;  \
                                        case 3: var = (var & 0x00ffffff) | ((val) << 24); break;  \
                                }

static void s3_accel_out_fifo(s3_t *s3, uint16_t port, uint8_t val)
{
        switch (port)
        {
                case 0x82e8:
                s3->accel.cur_y = (s3->accel.cur_y & 0xf00) | val;
                s3->accel.poly_cy = s3->accel.cur_y;
                break;
                case 0x82e9:
                s3->accel.cur_y = (s3->accel.cur_y & 0xff) | ((val & 0x1f) << 8);
                s3->accel.poly_cy = s3->accel.cur_y;
                break;
                case 0x82ea:
                s3->accel.cur_y2 = (s3->accel.cur_y2 & 0xf00) | val;
                s3->accel.poly_cy2 = s3->accel.cur_y2;
                break;
                case 0x82eb:
                s3->accel.cur_y2 = (s3->accel.cur_y2 & 0xff) | ((val & 0x1f) << 8);
                s3->accel.poly_cy2 = s3->accel.cur_y2;
                break;
                
                case 0x86e8:
                s3->accel.cur_x = (s3->accel.cur_x & 0xf00) | val;
                s3->accel.poly_cx = s3->accel.cur_x << 20;
                s3->accel.poly_x = s3->accel.poly_cx >> 20;
                break;
                case 0x86e9:
                s3->accel.cur_x = (s3->accel.cur_x & 0xff) | ((val & 0x1f) << 8);
                s3->accel.poly_cx = s3->accel.poly_x = s3->accel.cur_x << 20;
                s3->accel.poly_x = s3->accel.poly_cx >> 20;
                break;
                case 0x86ea:
                s3->accel.cur_x2 = (s3->accel.cur_x2 & 0xf00) | val;
                s3->accel.poly_cx2 = s3->accel.cur_x2 << 20;
                break;
                case 0x86eb:
                s3->accel.cur_x2 = (s3->accel.cur_x2 & 0xff) | ((val & 0x1f) << 8);
                s3->accel.poly_cx2 = s3->accel.cur_x2 << 20;
                break;
                
                case 0x8ae8:
                s3->accel.desty_axstp = (s3->accel.desty_axstp & 0x3f00) | val;
                s3->accel.point_1_updated = 1;
                break;
                case 0x8ae9:
                s3->accel.desty_axstp = (s3->accel.desty_axstp & 0xff) | ((val & 0x3f) << 8);
                if (val & 0x20)
                        s3->accel.desty_axstp |= ~0x3fff;
                s3->accel.point_1_updated = 1;
                break;
                case 0x8aea:
                s3->accel.desty_axstp2 = (s3->accel.desty_axstp2 & 0x3f00) | val;
                s3->accel.point_2_updated = 1;
                break;
                case 0x8aeb:
                s3->accel.desty_axstp2 = (s3->accel.desty_axstp2 & 0xff) | ((val & 0x3f) << 8);
                if (val & 0x20)
                        s3->accel.desty_axstp2 |= ~0x3fff;
                s3->accel.point_2_updated = 1;
                break;
                
                case 0x8ee8:
                s3->accel.destx_distp = (s3->accel.destx_distp & 0x3f00) | val;
                s3->accel.point_1_updated = 1;
                break;
                case 0x8ee9:
                s3->accel.destx_distp = (s3->accel.destx_distp & 0xff) | ((val & 0x3f) << 8);
                if (val & 0x20)
                        s3->accel.destx_distp |= ~0x3fff;
                s3->accel.point_1_updated = 1;
                break;
                case 0x8eea:
                s3->accel.x2 = (s3->accel.x2 & 0xf00) | val;
                s3->accel.point_2_updated = 1;
                break;
                case 0x8eeb:
                s3->accel.x2 = (s3->accel.x2 & 0xff) | ((val & 0xf) << 8);
                s3->accel.point_2_updated = 1;
                break;
                
                case 0x92e8:
                s3->accel.err_term = (s3->accel.err_term & 0x3f00) | val;
                break;
                case 0x92e9:
                s3->accel.err_term = (s3->accel.err_term & 0xff) | ((val & 0x3f) << 8);
                if (val & 0x20)
                   s3->accel.err_term |= ~0x3fff;
                break;
                case 0x92ea:
                s3->accel.err_term2 = (s3->accel.err_term2 & 0x3f00) | val;
                break;
                case 0x92eb:
                s3->accel.err_term2 = (s3->accel.err_term2 & 0xff) | ((val & 0x3f) << 8);
                if (val & 0x20)
                        s3->accel.err_term2 |= ~0x3fff;
                break;

                case 0x96e8:
                s3->accel.maj_axis_pcnt = (s3->accel.maj_axis_pcnt & 0x3f00) | val;
                break;
                case 0x96e9:
                s3->accel.maj_axis_pcnt = (s3->accel.maj_axis_pcnt & 0xff) | ((val & 0x0f) << 8);
                if (val & 0x08)
                   s3->accel.maj_axis_pcnt |= ~0x0fff;
                break;
                case 0x96ea:
                s3->accel.maj_axis_pcnt2 = (s3->accel.maj_axis_pcnt2 & 0xf00) | val;
                break;
                case 0x96eb:
                s3->accel.maj_axis_pcnt2 = (s3->accel.maj_axis_pcnt2 & 0xff) | ((val & 0x0f) << 8);
                if (val & 0x08)
                        s3->accel.maj_axis_pcnt2 |= ~0x0fff;
                break;

                case 0x9ae8:
                s3->accel.cmd = (s3->accel.cmd & 0xff00) | val;
                break;
                case 0x9ae9:
                s3->accel.cmd = (s3->accel.cmd & 0xff) | (val << 8);
                s3_accel_start(-1, 0, 0xffffffff, 0, s3);
                s3->accel.pix_trans_count = 0;
                s3->accel.multifunc[0xe] &= ~0x10; /*hack*/
                break;

                case 0x9ee8:
                s3->accel.short_stroke = (s3->accel.short_stroke & 0xff00) | val;
                break;
                case 0x9ee9:
                s3->accel.short_stroke = (s3->accel.short_stroke & 0xff) | (val << 8);
                break;

                case 0xa2e8:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0x00ff0000) | (val << 16);
                else
                        s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0x000000ff) | val;
                break;
                case 0xa2e9:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0xff000000) | (val << 24);
                else
                        s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0x0000ff00) | (val << 8);
                if (!(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.multifunc[0xe] ^= 0x10;
                break;
                case 0xa2ea:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0x00ff0000) | (val << 16);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0x00ff0000) | (val << 16);
                        else
                                s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0x000000ff) | val;
                }
                break;
                case 0xa2eb:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0xff000000) | (val << 24);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0xff000000) | (val << 24);
                        else
                                s3->accel.bkgd_color = (s3->accel.bkgd_color & ~0x0000ff00) | (val << 8);
                        s3->accel.multifunc[0xe] ^= 0x10;
                }
                break;

                case 0xa6e8:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.frgd_color = (s3->accel.frgd_color & ~0x00ff0000) | (val << 16);
                else
                        s3->accel.frgd_color = (s3->accel.frgd_color & ~0x000000ff) | val;
                break;
                case 0xa6e9:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.frgd_color = (s3->accel.frgd_color & ~0xff000000) | (val << 24);
                else
                        s3->accel.frgd_color = (s3->accel.frgd_color & ~0x0000ff00) | (val << 8);
                if (!(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.multifunc[0xe] ^= 0x10;
                break;
                case 0xa6ea:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.frgd_color = (s3->accel.frgd_color & ~0x00ff0000) | (val << 16);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.frgd_color = (s3->accel.frgd_color & ~0x00ff0000) | (val << 16);
                        else
                                s3->accel.frgd_color = (s3->accel.frgd_color & ~0x000000ff) | val;
                }
                break;
                case 0xa6eb:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.frgd_color = (s3->accel.frgd_color & ~0xff000000) | (val << 24);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.frgd_color = (s3->accel.frgd_color & ~0xff000000) | (val << 24);
                        else
                                s3->accel.frgd_color = (s3->accel.frgd_color & ~0x0000ff00) | (val << 8);
                        s3->accel.multifunc[0xe] ^= 0x10;
                }
                break;

                case 0xaae8:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0x00ff0000) | (val << 16);
                else
                        s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0x000000ff) | val;
                break;
                case 0xaae9:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0xff000000) | (val << 24);
                else
                        s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0x0000ff00) | (val << 8);
                if (!(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.multifunc[0xe] ^= 0x10;
                break;
                case 0xaaea:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0x00ff0000) | (val << 16);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0x00ff0000) | (val << 16);
                        else
                                s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0x000000ff) | val;
                }
                break;
                case 0xaaeb:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0xff000000) | (val << 24);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0xff000000) | (val << 24);
                        else
                                s3->accel.wrt_mask = (s3->accel.wrt_mask & ~0x0000ff00) | (val << 8);
                        s3->accel.multifunc[0xe] ^= 0x10;
                }
                break;

                case 0xaee8:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.rd_mask = (s3->accel.rd_mask & ~0x00ff0000) | (val << 16);
                else
                        s3->accel.rd_mask = (s3->accel.rd_mask & ~0x000000ff) | val;
                break;
                case 0xaee9:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.rd_mask = (s3->accel.rd_mask & ~0xff000000) | (val << 24);
                else
                        s3->accel.rd_mask = (s3->accel.rd_mask & ~0x0000ff00) | (val << 8);
                if (!(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.multifunc[0xe] ^= 0x10;
                break;
                case 0xaeea:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.rd_mask = (s3->accel.rd_mask & ~0x00ff0000) | (val << 16);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.rd_mask = (s3->accel.rd_mask & ~0x00ff0000) | (val << 16);
                        else
                                s3->accel.rd_mask = (s3->accel.rd_mask & ~0x000000ff) | val;
                }
                break;
                case 0xaeeb:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.rd_mask = (s3->accel.rd_mask & ~0xff000000) | (val << 24);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.rd_mask = (s3->accel.rd_mask & ~0xff000000) | (val << 24);
                        else
                                s3->accel.rd_mask = (s3->accel.rd_mask & ~0x0000ff00) | (val << 8);
                        s3->accel.multifunc[0xe] ^= 0x10;
                }
                break;

                case 0xb2e8:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.color_cmp = (s3->accel.color_cmp & ~0x00ff0000) | (val << 16);
                else
                        s3->accel.color_cmp = (s3->accel.color_cmp & ~0x000000ff) | val;
                break;
                case 0xb2e9:
                if (s3->bpp == 3 && s3->accel.multifunc[0xe] & 0x10 && !(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.color_cmp = (s3->accel.color_cmp & ~0xff000000) | (val << 24);
                else
                        s3->accel.color_cmp = (s3->accel.color_cmp & ~0x0000ff00) | (val << 8);
                if (!(s3->accel.multifunc[0xe] & 0x200))
                        s3->accel.multifunc[0xe] ^= 0x10;
                break;
                case 0xb2ea:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.color_cmp = (s3->accel.color_cmp & ~0x00ff0000) | (val << 16);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.color_cmp = (s3->accel.color_cmp & ~0x00ff0000) | (val << 16);
                        else
                                s3->accel.color_cmp = (s3->accel.color_cmp & ~0x000000ff) | val;
                }
                break;
                case 0xb2eb:
                if (s3->accel.multifunc[0xe] & 0x200)
                        s3->accel.color_cmp = (s3->accel.color_cmp & ~0xff000000) | (val << 24);
                else if (s3->bpp == 3)
                {
                        if (s3->accel.multifunc[0xe] & 0x10)
                                s3->accel.color_cmp = (s3->accel.color_cmp & ~0xff000000) | (val << 24);
                        else
                                s3->accel.color_cmp = (s3->accel.color_cmp & ~0x0000ff00) | (val << 8);
                        s3->accel.multifunc[0xe] ^= 0x10;
                }
                break;

                case 0xb6e8:
                s3->accel.bkgd_mix = val;
                break;

                case 0xbae8:
                s3->accel.frgd_mix = val;
                break;
                
                case 0xbee8:
                case 0xbeea:
                s3->accel.multifunc_cntl = (s3->accel.multifunc_cntl & 0xff00) | val;
                break;
                case 0xbee9:
                case 0xbeeb:
                s3->accel.multifunc_cntl = (s3->accel.multifunc_cntl & 0xff) | (val << 8);
                s3->accel.multifunc[s3->accel.multifunc_cntl >> 12] = s3->accel.multifunc_cntl & 0xfff;
                break;

                case 0xe2e8:
                s3->accel.pix_trans[0] = val;
                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80 && !(s3->accel.cmd & 0x600) && (s3->accel.cmd & 0x100))
                        s3_accel_start(8, 1, s3->accel.pix_trans[0], 0, s3);
                else if (!(s3->accel.cmd & 0x600) && (s3->accel.cmd & 0x100))
                        s3_accel_start(1, 1, 0xffffffff, s3->accel.pix_trans[0], s3);
                break;
                case 0xe2e9:
                s3->accel.pix_trans[1] = val;
                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80 && (s3->accel.cmd & 0x600) == 0x200 && (s3->accel.cmd & 0x100))
                {
                        if (s3->accel.cmd & 0x1000) s3_accel_start(16, 1, s3->accel.pix_trans[1] | (s3->accel.pix_trans[0] << 8), 0, s3);
                        else                        s3_accel_start(16, 1, s3->accel.pix_trans[0] | (s3->accel.pix_trans[1] << 8), 0, s3);
                }
                else if ((s3->accel.cmd & 0x600) == 0x200 && (s3->accel.cmd & 0x100))
                {
                        if (s3->accel.cmd & 0x1000) s3_accel_start(2, 1, 0xffffffff, s3->accel.pix_trans[1] | (s3->accel.pix_trans[0] << 8), s3);
                        else                        s3_accel_start(2, 1, 0xffffffff, s3->accel.pix_trans[0] | (s3->accel.pix_trans[1] << 8), s3);
                }
                break;
                case 0xe2ea:
                s3->accel.pix_trans[2] = val;
                break;
                case 0xe2eb:
                s3->accel.pix_trans[3] = val;
                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80 && (s3->accel.cmd & 0x600) == 0x600 && (s3->accel.cmd & 0x100) && s3->chip == S3_TRIO32)
                {
                        s3_accel_start(8, 1, s3->accel.pix_trans[3], 0, s3);
                        s3_accel_start(8, 1, s3->accel.pix_trans[2], 0, s3);
                        s3_accel_start(8, 1, s3->accel.pix_trans[1], 0, s3);
                        s3_accel_start(8, 1, s3->accel.pix_trans[0], 0, s3);
                }
                else if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80 && (s3->accel.cmd & 0x400) == 0x400 && (s3->accel.cmd & 0x100))
                        s3_accel_start(32, 1, s3->accel.pix_trans[0] | (s3->accel.pix_trans[1] << 8) | (s3->accel.pix_trans[2] << 16) | (s3->accel.pix_trans[3] << 24), 0, s3);
                else if ((s3->accel.cmd & 0x600) == 0x400 && (s3->accel.cmd & 0x100))
                        s3_accel_start(4, 1, 0xffffffff, s3->accel.pix_trans[0] | (s3->accel.pix_trans[1] << 8) | (s3->accel.pix_trans[2] << 16) | (s3->accel.pix_trans[3] << 24), s3);
                break;
        }
}

static void s3_accel_out_fifo_w(s3_t *s3, uint16_t port, uint16_t val)
{
//        pclog("Accel out w %04X %04X\n", port, val);
        if (s3->accel.cmd & 0x100)
        {
                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80)
                {
                        if (s3->accel.cmd & 0x1000)
                                val = (val >> 8) | (val << 8);
                        if ((s3->accel.cmd & 0x600) == 0x600 && s3->chip == S3_TRIO32)
                        {
                                s3_accel_start(8, 1, (val >> 8) & 0xff, 0, s3);
                                s3_accel_start(8, 1,  val       & 0xff, 0, s3);
                        }
                        if ((s3->accel.cmd & 0x600) == 0x000)
                                s3_accel_start(8, 1, val | (val << 16), 0, s3);
                        else
                                s3_accel_start(16, 1, val | (val << 16), 0, s3);
                }
                else
                {
                        if ((s3->accel.cmd & 0x600) == 0x000)
                                s3_accel_start(1, 1, 0xffffffff, val | (val << 16), s3);
                        else
                                s3_accel_start(2, 1, 0xffffffff, val | (val << 16), s3);
                }
        }
}

static void s3_accel_out_fifo_l(s3_t *s3, uint16_t port, uint32_t val)
{
//        pclog("Accel out l %04X %08X\n", port, val);
        if (s3->accel.cmd & 0x100)
        {
                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80)
                {
                        if ((s3->accel.cmd & 0x600) == 0x600 && s3->chip == S3_TRIO32)
                        {
                                if (s3->accel.cmd & 0x1000)
                                        val = ((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24);
                                s3_accel_start(8, 1, (val >> 24) & 0xff, 0, s3);
                                s3_accel_start(8, 1, (val >> 16) & 0xff, 0, s3);
                                s3_accel_start(8, 1, (val >> 8)  & 0xff, 0, s3);
                                s3_accel_start(8, 1,  val        & 0xff, 0, s3);
                        }
                        else if (s3->accel.cmd & 0x400)
                        {
                                if (s3->accel.cmd & 0x1000)
                                        val = ((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24);
                                s3_accel_start(32, 1, val, 0, s3);
                        }
                        else if ((s3->accel.cmd & 0x600) == 0x200)
                        {
                                if (s3->accel.cmd & 0x1000)
                                        val = ((val & 0xff00ff00) >> 8) | ((val & 0x00ff00ff) << 8);
                                s3_accel_start(16, 1, val, 0, s3);
                                s3_accel_start(16, 1, val >> 16, 0, s3);
                        }
                        else
                        {
                                if (s3->accel.cmd & 0x1000)
                                        val = ((val & 0xff00ff00) >> 8) | ((val & 0x00ff00ff) << 8);
                                s3_accel_start(8, 1, val, 0, s3);
                                s3_accel_start(8, 1, val >> 16, 0, s3);
                        }
                }
                else
                {
                        if (s3->accel.cmd & 0x400)
                                s3_accel_start(4, 1, 0xffffffff, val, s3);
                        else if ((s3->accel.cmd & 0x600) == 0x200)
                        {
                                s3_accel_start(2, 1, 0xffffffff, val, s3);
                                s3_accel_start(2, 1, 0xffffffff, val >> 16, s3);
                        }
                        else
                        {
                                s3_accel_start(1, 1, 0xffffffff, val, s3);
                                s3_accel_start(1, 1, 0xffffffff, val >> 16, s3);
                        }
                }
        }
}

static void s3_accel_write_fifo(s3_t *s3, uint32_t addr, uint8_t val)
{
//        pclog("Write S3 accel %08X %02X\n", addr, val);
        if (s3->packed_mmio)
        {
                int addr_lo = addr & 1;
                switch (addr & 0xfffe)
                {
                        case 0x8100: addr = 0x82e8; break; /*ALT_CURXY*/
                        case 0x8102: addr = 0x86e8; break;
                        
                        case 0x8104: addr = 0x82ea; break; /*ALT_CURXY2*/
                        case 0x8106: addr = 0x86ea; break;
                        
                        case 0x8108: addr = 0x8ae8; break; /*ALT_STEP*/
                        case 0x810a: addr = 0x8ee8; break;
                        
                        case 0x810c: addr = 0x8aea; break; /*ALT_STEP2*/
                        case 0x810e: addr = 0x8eea; break;

                        case 0x8110: addr = 0x92e8; break; /*ALT_ERR*/
                        case 0x8112: addr = 0x92ee; break;

                        case 0x8118: addr = 0x9ae8; break; /*ALT_CMD*/
                        case 0x811a: addr = 0x9aea; break;
                        
                        case 0x811c: addr = 0x9ee8; break; /*SHORT_STROKE*/
                        
                        case 0x8120: case 0x8122:          /*BKGD_COLOR*/
                        WRITE8(addr, s3->accel.bkgd_color, val);
                        return;
                        
                        case 0x8124: case 0x8126:          /*FRGD_COLOR*/
                        WRITE8(addr, s3->accel.frgd_color, val);
                        return;

                        case 0x8128: case 0x812a:          /*WRT_MASK*/
                        WRITE8(addr, s3->accel.wrt_mask, val);
                        return;

                        case 0x812c: case 0x812e:          /*RD_MASK*/
                        WRITE8(addr, s3->accel.rd_mask, val);
                        return;

                        case 0x8130: case 0x8132:          /*COLOR_CMP*/
                        WRITE8(addr, s3->accel.color_cmp, val);
                        return;

                        case 0x8134: addr = 0xb6e8; break; /*ALT_MIX*/
                        case 0x8136: addr = 0xbae8; break;
                        
                        case 0x8138:                       /*SCISSORS_T*/
                        WRITE8(addr & 1, s3->accel.multifunc[1], val);
                        return;
                        case 0x813a:                       /*SCISSORS_L*/
                        WRITE8(addr & 1, s3->accel.multifunc[2], val);
                        return;
                        case 0x813c:                       /*SCISSORS_B*/
                        WRITE8(addr & 1, s3->accel.multifunc[3], val);
                        return;
                        case 0x813e:                       /*SCISSORS_R*/
                        WRITE8(addr & 1, s3->accel.multifunc[4], val);
                        return;

                        case 0x8140:                       /*PIX_CNTL*/
                        WRITE8(addr & 1, s3->accel.multifunc[0xa], val);
                        return;
                        case 0x8142:                       /*MULT_MISC2*/
                        WRITE8(addr & 1, s3->accel.multifunc[0xd], val);
                        return;
                        case 0x8144:                       /*MULT_MISC*/
                        WRITE8(addr & 1, s3->accel.multifunc[0xe], val);
                        return;
                        case 0x8146:                       /*READ_SEL*/
                        WRITE8(addr & 1, s3->accel.multifunc[0xf], val);
                        return;

                        case 0x8148:                       /*ALT_PCNT*/
                        WRITE8(addr & 1, s3->accel.multifunc[0], val);
                        return;
                        case 0x814a: addr = 0x96e8; break;
                        case 0x814c: addr = 0x96ea; break;

                        case 0x8168: addr = 0xeae8; break;
                        case 0x816a: addr = 0xeaea; break;
                }
                addr |= addr_lo;
        }
        

        if (addr & 0x8000)
        {
                s3_accel_out_fifo(s3, addr & 0xffff, val);
        }
        else
        {
                if (s3->accel.cmd & 0x100)
                {
                        if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80)
                                s3_accel_start(8, 1, val | (val << 8) | (val << 16) | (val << 24), 0, s3);
                        else
                                s3_accel_start(1, 1, 0xffffffff, val | (val << 8) | (val << 16) | (val << 24), s3);
                }
        }
}

static void s3_accel_write_fifo_w(s3_t *s3, uint32_t addr, uint16_t val)
{
//        pclog("Write S3 accel w %08X %04X\n", addr, val);
        if (addr & 0x8000)
        {
                s3_accel_write_fifo(s3, addr,     val);
                s3_accel_write_fifo(s3, addr + 1, val >> 8);
        }
        else
        {
                if (s3->accel.cmd & 0x100)
                {
                        if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80)
                        {
                                if (s3->accel.cmd & 0x1000)
                                        val = (val >> 8) | (val << 8);
                                if ((s3->accel.cmd & 0x600) == 0x600 && s3->chip == S3_TRIO32)
                                {
                                        s3_accel_start(8, 1, (val >> 8) & 0xff, 0, s3);
                                        s3_accel_start(8, 1,  val       & 0xff, 0, s3);
                                }
                                else if ((s3->accel.cmd & 0x600) == 0x000)
                                        s3_accel_start(8, 1, val | (val << 16), 0, s3);
                                else
                                        s3_accel_start(16, 1, val | (val << 16), 0, s3);
                        }
                        else
                        {
                                if ((s3->accel.cmd & 0x600) == 0x000)
                                        s3_accel_start(1, 1, 0xffffffff, val | (val << 16), s3);
                                else
                                        s3_accel_start(2, 1, 0xffffffff, val | (val << 16), s3);
                        }
                }
        }
}

static void s3_accel_write_fifo_l(s3_t *s3, uint32_t addr, uint32_t val)
{
//        pclog("Write S3 accel l %08X %08X\n", addr, val);
        if (addr & 0x8000)
        {
                s3_accel_write_fifo(s3, addr,     val);
                s3_accel_write_fifo(s3, addr + 1, val >> 8);
                s3_accel_write_fifo(s3, addr + 2, val >> 16);
                s3_accel_write_fifo(s3, addr + 3, val >> 24);
        }
        else
        {
                if (s3->accel.cmd & 0x100)
                {
                        if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80)
                        {
                                if ((s3->accel.cmd & 0x600) == 0x600 && s3->chip == S3_TRIO32)
                                {
                                        if (s3->accel.cmd & 0x1000)
                                                val = ((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24);
                                        s3_accel_start(8, 1, (val >> 24) & 0xff, 0, s3);
                                        s3_accel_start(8, 1, (val >> 16) & 0xff, 0, s3);
                                        s3_accel_start(8, 1, (val >> 8)  & 0xff, 0, s3);
                                        s3_accel_start(8, 1,  val        & 0xff, 0, s3);
                                }
                                else if (s3->accel.cmd & 0x400)
                                {
                                        if (s3->accel.cmd & 0x1000)
                                                val = ((val & 0xff000000) >> 24) | ((val & 0x00ff0000) >> 8) | ((val & 0x0000ff00) << 8) | ((val & 0x000000ff) << 24);
                                        s3_accel_start(32, 1, val, 0, s3);
                                }
                                else if ((s3->accel.cmd & 0x600) == 0x200)
                                {
                                        if (s3->accel.cmd & 0x1000)
                                                val = ((val & 0xff00ff00) >> 8) | ((val & 0x00ff00ff) << 8);
                                        s3_accel_start(16, 1, val, 0, s3);
                                        s3_accel_start(16, 1, val >> 16, 0, s3);
                                }
                                else
                                {
                                        if (s3->accel.cmd & 0x1000)
                                                val = ((val & 0xff00ff00) >> 8) | ((val & 0x00ff00ff) << 8);
                                        s3_accel_start(8, 1, val, 0, s3);
                                        s3_accel_start(8, 1, val >> 16, 0, s3);
                                }
                        }
                        else
                        {
                                if (s3->accel.cmd & 0x400)
                                        s3_accel_start(4, 1, 0xffffffff, val, s3);
                                else if ((s3->accel.cmd & 0x600) == 0x200)
                                {
                                        s3_accel_start(2, 1, 0xffffffff, val, s3);
                                        s3_accel_start(2, 1, 0xffffffff, val >> 16, s3);
                                }
                                else
                                {
                                        s3_accel_start(1, 1, 0xffffffff, val, s3);
                                        s3_accel_start(1, 1, 0xffffffff, val >> 16, s3);
                                }
                        }
                }
        }
}

static void fifo_thread(void *param)
{
        s3_t *s3 = (s3_t *)param;
        
        s3->fifo_thread_state = 1;
        while (s3->fifo_thread_state > 0)
        {
                thread_set_event(s3->fifo_not_full_event);
                thread_wait_event(s3->wake_fifo_thread, -1);
                thread_reset_event(s3->wake_fifo_thread);
                s3->blitter_busy = 1;
                while (!FIFO_EMPTY)
                {
                        uint64_t start_time = timer_read();
                        uint64_t end_time;
                        fifo_entry_t *fifo = &s3->fifo[s3->fifo_read_idx & FIFO_MASK];

                        switch (fifo->addr_type & FIFO_TYPE)
                        {
                                case FIFO_WRITE_BYTE:
                                s3_accel_write_fifo(s3, fifo->addr_type & FIFO_ADDR, fifo->val);
                                break;
                                case FIFO_WRITE_WORD:
                                s3_accel_write_fifo_w(s3, fifo->addr_type & FIFO_ADDR, fifo->val);
                                break;
                                case FIFO_WRITE_DWORD:
                                s3_accel_write_fifo_l(s3, fifo->addr_type & FIFO_ADDR, fifo->val);
                                break;
                                case FIFO_OUT_BYTE:
                                s3_accel_out_fifo(s3, fifo->addr_type & FIFO_ADDR, fifo->val);
                                break;
                                case FIFO_OUT_WORD:
                                s3_accel_out_fifo_w(s3, fifo->addr_type & FIFO_ADDR, fifo->val);
                                break;
                                case FIFO_OUT_DWORD:
                                s3_accel_out_fifo_l(s3, fifo->addr_type & FIFO_ADDR, fifo->val);
                                break;
                        }
                                                
                        s3->fifo_read_idx++;
                        fifo->addr_type = FIFO_INVALID;

                        if (FIFO_ENTRIES > 0xe000)
                                thread_set_event(s3->fifo_not_full_event);

                        end_time = timer_read();
                        s3->blitter_time += end_time - start_time;
                }
                s3->blitter_busy = 0;
                s3->subsys_stat |= INT_FIFO_EMP;
                s3_update_irqs_thread(s3, INT_FIFO_EMP);
        }
        s3->fifo_thread_state = 0;
}

static void s3_vblank_start(svga_t *svga)
{
        s3_t *s3 = (s3_t *)svga->priv;
        if (s3->vblank_irq >= 0) {
            s3->vblank_irq = 1;
        }
        if ((s3->subsys_cntl & INT_VSY) || s3_vga_vsync_enabled(s3)) {
                s3->subsys_stat |= INT_VSY;
                s3_update_irqs(s3);
        }
}

static void s3_queue(s3_t *s3, uint32_t addr, uint32_t val, uint32_t type)
{
        fifo_entry_t *fifo = &s3->fifo[s3->fifo_write_idx & FIFO_MASK];

        if (FIFO_FULL)
        {
                thread_reset_event(s3->fifo_not_full_event);
                if (FIFO_FULL)
                {
                        thread_wait_event(s3->fifo_not_full_event, -1); /*Wait for room in ringbuffer*/
                }
        }

        fifo->val = val;
        fifo->addr_type = (addr & FIFO_ADDR) | type;

        s3->fifo_write_idx++;
        
        if (FIFO_ENTRIES > 0xe000 || FIFO_ENTRIES < 8)
                wake_fifo_thread(s3);
}

static void s3_io_set(s3_t*s3);

void s3_out(uint16_t addr, uint8_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
        svga_t *svga = &s3->svga;
        uint8_t old;

        if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1)) 
                addr ^= 0x60;
        
//        pclog("S3 out %04X %02X %04x:%08x\n", addr, val, CS, pc);

        switch (addr)
        {
                case 0x3c5:
                if (svga->seqaddr >= 0x10 && svga->seqaddr < 0x20)
                {
                        svga->seqregs[svga->seqaddr] = val;
                        switch (svga->seqaddr)
                        {
                                case 0x12: case 0x13:
                                svga_recalctimings(svga);
                                return;
                        }
                }
                if (svga->seqaddr == 4) /*Chain-4 - update banking*/
                {
                        if (val & 8) svga->write_bank = svga->read_bank = s3->bank << 16;
                        else         svga->write_bank = svga->read_bank = s3->bank << 14;
                }
                else if (svga->seqaddr == 9) {
                    svga->seqregs[9] = val & 0x80;
                    s3_io_set(s3);
                    return;
                } else if (svga->seqaddr == 0xa) {
                    svga->seqregs[0xa] = val & 0x80;
                    return;
                } else if (s3->chip >= S3_TRIO32) {
                    if (svga->seqaddr == 8) {
                        svga->seqregs[8] = val & 0x0f;
                        return;
                    } else if ((svga->seqaddr == 0xd) && (svga->seqregs[8] == 0x06)) {
                        svga->seqregs[0xd] = val;
                        svga->dpms = (svga->crtc[0x56] & ((s3->chip >= S3_TRIO32) ? 0x06 : 0x20));
                        svga_recalctimings(svga);
                        return;
                    }
                }
                break;
                
                case 0x3C6: case 0x3C7: case 0x3C8: case 0x3C9:
//                pclog("Write RAMDAC %04X %02X %04X:%04X\n", addr, val, CS, pc);
                if (s3->chip == S3_TRIO32 || s3->chip == S3_TRIO64)
                        svga_out(addr, val, svga);
                else
                {
                        if ((svga->crtc[0x55] & 1) || (svga->crtc[0x43] & 2))
                                sdac_ramdac_out((addr & 3) | 4, val, &s3->ramdac, svga);
                        else
                                sdac_ramdac_out(addr & 3, val, &s3->ramdac, svga);
                }
                return;

                case 0x3D4:
                svga->crtcreg = val & 0x7f;
                return;
                case 0x3D5:
                if ((svga->crtcreg < 7) && (svga->crtc[0x11] & 0x80))
                        return;
                if ((svga->crtcreg == 7) && (svga->crtc[0x11] & 0x80))
                        val = (svga->crtc[7] & ~0x10) | (val & 0x10);
                if (svga->crtcreg >= 0x20 && svga->crtcreg != 0x38 && svga->crtcreg != 0x39 && (svga->crtc[0x38] & 0xcc) != 0x48)
                {
                        if (!((svga->crtc[0x39] & 0xe0) == 0xa0 && svga->crtcreg >= 0x40 && svga->crtcreg <= 0x6d))
                                return;
                }
                old = svga->crtc[svga->crtcreg];
                svga->crtc[svga->crtcreg] = val;
                switch (svga->crtcreg)
                {
                        case 0x11:
                        if (!(val & 0x10)) {
                            if (s3->vblank_irq > 0)
                                s3->vblank_irq = -1;
                        } else if (s3->vblank_irq < 0) {
                            s3->vblank_irq = 0;
                        }
                        s3_update_irqs(s3);
                        if ((val & ~0x30) == (old & ~0x30))
                            old = val;
                        break;

                        case 0x31:
                        s3->ma_ext = (s3->ma_ext & 0x1c) | ((val & 0x30) >> 4);
                        svga->force_dword_mode = val & 0x08;
                        break;
                        case 0x32:
                        svga->vram_display_mask = (val & 0x40) ? 0x3ffff : s3->vram_mask;
                        s3_update_irqs(s3);
                        break;
                                                
                        case 0x50:
                        switch (svga->crtc[0x50] & 0xc1)
                        {
                                case 0x00: s3->width = (svga->crtc[0x31] & 2) ? 2048 : 1024; break;
                                case 0x01: s3->width = 1152; break;
                                case 0x40: s3->width = 640;  break;
                                case 0x80: s3->width = 800;  break;
                                case 0x81: s3->width = 1600; break;
                                case 0xc0: s3->width = 1280; break;
                        }
                        s3->bpp = (svga->crtc[0x50] >> 4) & 3;
                        break;
                        case 0x69:
                        s3->ma_ext = val & 0x1f;
                        break;
                        
                        case 0x35:
                        s3->bank = (s3->bank & 0x70) | (val & 0xf);
//                        pclog("CRTC write R35 %02X\n", val);
                        if (svga->chain4) svga->write_bank = svga->read_bank = s3->bank << 16;
                        else              svga->write_bank = svga->read_bank = s3->bank << 14;
                        break;
                        case 0x51:
                        s3->bank = (s3->bank & 0x4f) | ((val & 0xc) << 2);
//                        pclog("CRTC write R51 %02X\n", val);
                        if (svga->chain4) svga->write_bank = svga->read_bank = s3->bank << 16;
                        else              svga->write_bank = svga->read_bank = s3->bank << 14;
                        s3->ma_ext = (s3->ma_ext & ~0xc) | ((val & 3) << 2);
                        break;
                        case 0x6a:
                        s3->bank = val;
//                        pclog("CRTC write R6a %02X\n", val);
                        if (svga->chain4) svga->write_bank = svga->read_bank = s3->bank << 16;
                        else              svga->write_bank = svga->read_bank = s3->bank << 14;
                        break;
                        
                        case 0x3a:
                        if (val & 0x10) 
                                svga->gdcreg[5] |= 0x40; /*Horrible cheat*/
                        break;
                        
                        case 0x45:
                        svga->hwcursor.ena = val & 1;
                        break;
                        case 0x48:
                        svga->hwcursor.x = ((svga->crtc[0x46] << 8) | svga->crtc[0x47]) & 0x7ff;
                        if (svga->bpp == 32) svga->hwcursor.x >>= 1;
                        svga->hwcursor.y = ((svga->crtc[0x48] << 8) | svga->crtc[0x49]) & 0x7ff;
                        svga->hwcursor.xoff = svga->crtc[0x4e] & 63;
                        svga->hwcursor.yoff = svga->crtc[0x4f] & 63;
                        svga->hwcursor.addr = ((((svga->crtc[0x4c] << 8) | svga->crtc[0x4d]) & 0xfff) * 1024) + (svga->hwcursor.yoff * 16);
                        if ((s3->chip == S3_TRIO32 || s3->chip == S3_TRIO64) && svga->bpp == 32)
                                svga->hwcursor.x <<= 1;
                        break;
                        
                        case 0x4a:
                        switch (s3->hwc_col_stack_pos)
                        {
                                case 0:
                                s3->hwc_fg_col = (s3->hwc_fg_col & 0xffff00) | val;
                                break;
                                case 1:
                                s3->hwc_fg_col = (s3->hwc_fg_col & 0xff00ff) | (val << 8);
                                break;
                                case 2:
                                s3->hwc_fg_col = (s3->hwc_fg_col & 0x00ffff) | (val << 16);
                                break;
                        }
                        s3->hwc_col_stack_pos = (s3->hwc_col_stack_pos + 1) % 3;
                        break;
                        case 0x4b:
                        switch (s3->hwc_col_stack_pos)
                        {
                                case 0:
                                s3->hwc_bg_col = (s3->hwc_bg_col & 0xffff00) | val;
                                break;
                                case 1:
                                s3->hwc_bg_col = (s3->hwc_bg_col & 0xff00ff) | (val << 8);
                                break;
                                case 2:
                                s3->hwc_bg_col = (s3->hwc_bg_col & 0x00ffff) | (val << 16);
                                break;
                        }
                        s3->hwc_col_stack_pos = (s3->hwc_col_stack_pos + 1) % 3;
                        break;

                        case 0x53:
                        case 0x58: case 0x59: case 0x5a:
                        s3_updatemapping(s3);
                        break;
                        
                        case 0x56:
                            svga->dpms = (svga->crtc[0x56] & ((s3->chip >= S3_TRIO32) ? 0x06 : 0x20));
                            old = ~val; /* force recalc */
                        break;

                        case 0x67:
                        if (s3->chip == S3_TRIO32 || s3->chip == S3_TRIO64)
                        {
                                switch (val >> 4)
                                {
                                        case 3:  svga->bpp = 15; break;
                                        case 5:  svga->bpp = 16; break;
                                        case 7:  svga->bpp = 24; break;
                                        case 13: svga->bpp = 32; break;
                                        default: svga->bpp = 8;  break;
                                }
                        }
                        break;
                        //case 0x55: case 0x43:
//                                pclog("Write CRTC R%02X %02X\n", crtcreg, val);
                }
                if (old != val)
                {
                        if (svga->crtcreg < 0xe || svga->crtcreg > 0x10)
                        {
                                svga->fullchange = changeframecount;
                                svga_recalctimings(svga);
                        }
                }
                break;
        }
        svga_out(addr, val, svga);
}

uint8_t s3_in(uint16_t addr, void *p)
{
        s3_t *s3 = (s3_t *)p;
        svga_t *svga = &s3->svga;
        uint8_t ret;

        if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1)) 
                addr ^= 0x60;

//        if (addr != 0x3da) pclog("S3 in %04X %08x:%02x\n", addr, CS, pc);
        switch (addr)
        {
                case 0x3c1:
                if (svga->attraddr > 0x14)
                        return 0xff;
                break;
                        
                /* UAE UPDATE */
                case 0x3c2:
                    ret = svga_in(addr, svga);
                    ret |= (s3->subsys_stat & INT_VSY) ? 0x80 : 0x00;
                    ret |= s3->vblank_irq > 0 ? 0x80 : 0x00;
                    return ret;

                case 0x3c5:
                if (svga->seqaddr >= 0x10 && svga->seqaddr < 0x20)
                        return svga->seqregs[svga->seqaddr];
                break;
                
                case 0x3c6: case 0x3c7: case 0x3c8: case 0x3c9:
//                pclog("Read RAMDAC %04X  %04X:%04X\n", addr, CS, pc);
                if (s3->chip == S3_TRIO32 || s3->chip == S3_TRIO64)
                        return svga_in(addr, svga);
                if ((svga->crtc[0x55] & 1) || (svga->crtc[0x43] & 2))
                        return sdac_ramdac_in((addr & 3) | 4, &s3->ramdac, svga);
                return sdac_ramdac_in(addr & 3, &s3->ramdac, svga);

                case 0x3d4:
                return svga->crtcreg;
                case 0x3d5:
//                pclog("Read CRTC R%02X %02x %04X:%04X\n", svga->crtcreg, svga->crtc[svga->crtcreg], CS, pc);
                switch (svga->crtcreg)
                {
                        case 0x2d: return 0x88;       /*Extended chip ID*/
                        case 0x2e: return s3->id_ext; /*New chip ID*/
                        case 0x2f: return 0;          /*Revision level*/
                        case 0x30: return s3->id;     /*Chip ID*/
                        case 0x31: return (svga->crtc[0x31] & 0xcf) | ((s3->ma_ext & 3) << 4);
                        case 0x35: return (svga->crtc[0x35] & 0xf0) | (s3->bank & 0xf);
                        case 0x45: s3->hwc_col_stack_pos = 0; break;
                        case 0x51: return (svga->crtc[0x51] & 0xf0) | ((s3->bank >> 2) & 0xc) | ((s3->ma_ext >> 2) & 3);
                        case 0x69: return s3->ma_ext;
                        case 0x6a: return s3->bank;
                }
                return svga->crtc[svga->crtcreg];
        }
        return svga_in(addr, svga);
}

void s3_recalctimings(svga_t *svga)
{
        s3_t *s3 = (s3_t *)svga->priv;
        svga->hdisp = svga->hdisp_old;
        int clk_sel = (svga->miscout >> 2) & 3;

        if (clk_sel == 3 && s3->chip == S3_VISION864)
                clk_sel = svga->crtc[0x42] & 0xf;

//        pclog("%i %i\n", svga->hdisp, svga->hdisp_time);
//        pclog("recalctimings\n");
        svga->ma_latch |= (s3->ma_ext << 16);
//        pclog("SVGA_MA %08X\n", svga_ma);
        if (svga->crtc[0x5d] & 0x01) svga->htotal     += 0x100;
        if (svga->crtc[0x5d] & 0x02) 
        {
                svga->hdisp_time += 0x100;
                svga->hdisp += 0x100 * ((svga->seqregs[1] & 8) ? 16 : 8);
        }
        if (svga->crtc[0x5e] & 0x01) svga->vtotal      += 0x400;
        if (svga->crtc[0x5e] & 0x02) svga->dispend     += 0x400;
        if (svga->crtc[0x5e] & 0x04) svga->vblankstart += 0x400;
        if (svga->crtc[0x5e] & 0x10) svga->vsyncstart  += 0x400;
        if (svga->crtc[0x5e] & 0x40) svga->split       += 0x400;
        if (svga->crtc[0x51] & 0x30)      svga->rowoffset  += (svga->crtc[0x51] & 0x30) << 4;
        else if (svga->crtc[0x43] & 0x04) svga->rowoffset  += 0x100;
        if (!svga->rowoffset) svga->rowoffset = 256;
        svga->interlace = svga->crtc[0x42] & 0x20;
        //svga->clock = (cpuclock * (float)(1ull << 32)) / s3->getclock(clk_sel, s3->getclock_p);

        switch (svga->crtc[0x67] >> 4)
        {
                case 3: case 5: case 7:
                svga->clock /= 2;
                break;
        }                

        svga->lowres = !((svga->gdcreg[5] & 0x40) && (svga->crtc[0x3a] & 0x10));
        if ((svga->gdcreg[5] & 0x40) && (svga->crtc[0x3a] & 0x10))
        {
                switch (svga->bpp)
                {
                        case 8: 
                        svga->render = svga_render_8bpp_highres;
                        break;
                        case 15: 
                        svga->render = svga_render_15bpp_highres;
                        svga->hdisp /= 2;
                        break;
                        case 16: 
                        svga->render = svga_render_16bpp_highres;
                        svga->hdisp /= 2;
                        break;
                        case 24: 
                        svga->render = svga_render_24bpp_highres;
                        svga->hdisp /= 3;
                        break;
                        case 32:
                        svga->render = svga_render_32bpp_highres;
                        if (s3->chip != S3_TRIO32 && s3->chip != S3_TRIO64)
                                svga->hdisp /= 4;
                        break;
                }
        }

        svga->horizontal_linedbl = svga->dispend * 9 / 10 >= svga->hdisp;
}

void s3_updatemapping(s3_t *s3)
{
        svga_t *svga = &s3->svga;
        
//        video_write_a000_w = video_write_a000_l = NULL;

        if (!(s3->pci_regs[PCI_REG_COMMAND] & PCI_COMMAND_MEM))
        {
//                pclog("Update mapping - PCI disabled\n");
                mem_mapping_disablex(&svga->mapping);
                mem_mapping_disablex(&s3->linear_mapping);
                mem_mapping_disablex(&s3->mmio_mapping);
                return;
        }

//        pclog("Update mapping - bank %02X ", svga->gdcreg[6] & 0xc);
        /*Banked framebuffer*/
        if (svga->crtc[0x31] & 0x08) /*Enhanced mode mappings*/
        {
                /*Enhanced mapping forces 64kb at 0xa0000*/
                mem_mapping_set_addrx(&svga->mapping, 0xa0000, 0x10000);
                svga->banked_mask = 0xffff;
        }
        else switch (svga->gdcreg[6] & 0xc) /*VGA mapping*/
        {
                case 0x0: /*128k at A0000*/
                mem_mapping_set_addrx(&svga->mapping, 0xa0000, 0x20000);
                svga->banked_mask = 0xffff;
                break;
                case 0x4: /*64k at A0000*/
                mem_mapping_set_addrx(&svga->mapping, 0xa0000, 0x10000);
                svga->banked_mask = 0xffff;
                break;
                case 0x8: /*32k at B0000*/
                mem_mapping_set_addrx(&svga->mapping, 0xb0000, 0x08000);
                svga->banked_mask = 0x7fff;
                break;
                case 0xC: /*32k at B8000*/
                mem_mapping_set_addrx(&svga->mapping, 0xb8000, 0x08000);
                svga->banked_mask = 0x7fff;
                break;
        }
        
//        pclog("Linear framebuffer %02X ", svga->crtc[0x58] & 0x10);
        if (svga->crtc[0x58] & 0x10) /*Linear framebuffer*/
        {
                mem_mapping_disablex(&svga->mapping);
                
                s3->linear_base = (svga->crtc[0x5a] << 16) | (svga->crtc[0x59] << 24);
                switch (svga->crtc[0x58] & 3)
                {
                        case 0: /*64k*/
                        s3->linear_size = 0x10000;
                        break;
                        case 1: /*1mb*/
                        s3->linear_size = 0x100000;
                        break;
                        case 2: /*2mb*/
                        s3->linear_size = 0x200000;
                        break;
                        case 3: /*8mb*/
                        s3->linear_size = 0x800000;
                        break;
                }
                s3->linear_base &= ~(s3->linear_size - 1);
//                pclog("%08X %08X  %02X %02X %02X\n", linear_base, linear_size, crtc[0x58], crtc[0x59], crtc[0x5a]);
//                pclog("Linear framebuffer at %08X size %08X\n", s3->linear_base, s3->linear_size);
                if (s3->linear_base == 0xa0000)
                {
                        mem_mapping_disablex(&s3->linear_mapping);
                        if (!(svga->crtc[0x53] & 0x10))
                        {
                                mem_mapping_set_addrx(&svga->mapping, 0xa0000, 0x10000);
                                svga->banked_mask = 0xffff;
                        }
//                        mem_mapping_set_addrx(&s3->linear_mapping, 0xa0000, 0x10000);
                }
                else
                        mem_mapping_set_addrx(&s3->linear_mapping, s3->linear_base, s3->linear_size);
        }
        else
                mem_mapping_disablex(&s3->linear_mapping);
        
//        pclog("Memory mapped IO %02X\n", svga->crtc[0x53] & 0x10);
        if (svga->crtc[0x53] & 0x10) /*Memory mapped IO*/
        {
                mem_mapping_disablex(&svga->mapping);
                mem_mapping_enablex(&s3->mmio_mapping);
        }
        else
                mem_mapping_disablex(&s3->mmio_mapping);
}

static float s3_trio64_getclock(int clock, void *p)
{
        s3_t *s3 = (s3_t *)p;
        svga_t *svga = &s3->svga;
        float t;
        int m, n1, n2;
//        pclog("Trio64_getclock %i %02X %02X\n", clock, svga->seqregs[0x13], svga->seqregs[0x12]);
        if (clock == 0) return 25175000.0;
        if (clock == 1) return 28322000.0;
        m  = svga->seqregs[0x13] + 2;
        n1 = (svga->seqregs[0x12] & 0x1f) + 2;
        n2 = ((svga->seqregs[0x12] >> 5) & 0x07);
        t = (14318184.0 * ((float)m / (float)n1)) / (float)(1 << n2);
//        pclog("TRIO64 clock %i %i %i %f  %f %i\n", m, n1, n2, t, 14318184.0 * ((float)m / (float)n1), 1 << n2);
        return t;
}


void s3_accel_out(uint16_t port, uint8_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
//        pclog("Accel out %04X %02X\n", port, val);
        
        if (port >= 0x8000)
        {
                s3_queue(s3, port, val, FIFO_OUT_BYTE);
        }
        else switch (port)
        {
                case 0x42e8:
                s3->subsys_stat &= ~val;
                s3_update_irqs(s3);
                break;
                case 0x42e9:
                s3->subsys_cntl = val;
                s3_update_irqs(s3);
                break;
                case 0x46e8:
                s3->accel.setup_md = val;
                break;
                case 0x4ae8:
                s3->accel.advfunc_cntl = val;
                break;
        }
}

void s3_accel_out_w(uint16_t port, uint16_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
//        pclog("Accel out w %04X %04X\n", port, val);        
        s3_queue(s3, port, val, FIFO_OUT_WORD);
}

void s3_accel_out_l(uint16_t port, uint32_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
//        pclog("Accel out l %04X %08X\n", port, val);                
        s3_queue(s3, port, val, FIFO_OUT_DWORD);
}

uint8_t s3_accel_in(uint16_t port, void *p)
{
        s3_t *s3 = (s3_t *)p;
        int temp;
//        pclog("Accel in  %04X\n", port);
        switch (port)
        {
                case 0x42e8:
                return s3->subsys_stat;
                case 0x42e9:
                return s3->subsys_cntl;

                case 0x82e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.cur_y & 0xff;
                case 0x82e9:
                s3_wait_fifo_idle(s3);
                return s3->accel.cur_y  >> 8;

                case 0x86e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.cur_x & 0xff;
                case 0x86e9:
                s3_wait_fifo_idle(s3);
                return s3->accel.cur_x  >> 8;

                case 0x8ae8:
                s3_wait_fifo_idle(s3);
                return s3->accel.desty_axstp & 0xff;
                case 0x8ae9:
                s3_wait_fifo_idle(s3);
                return s3->accel.desty_axstp >> 8;

                case 0x8ee8:
                s3_wait_fifo_idle(s3);
                return s3->accel.destx_distp & 0xff;
                case 0x8ee9:
                s3_wait_fifo_idle(s3);
                return s3->accel.destx_distp >> 8;

                case 0x92e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.err_term & 0xff;
                case 0x92e9:
                s3_wait_fifo_idle(s3);
                return s3->accel.err_term >> 8;

                case 0x96e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.maj_axis_pcnt & 0xff;
                case 0x96e9:
                s3_wait_fifo_idle(s3);
                return s3->accel.maj_axis_pcnt >> 8;

                case 0x9ae8:
                if (!s3->blitter_busy)
                        wake_fifo_thread(s3);
                if (FIFO_FULL)
                        return 0xff; /*FIFO full*/
                return 0;    /*FIFO empty*/
                case 0x9ae9:
                if (!s3->blitter_busy)
                        wake_fifo_thread(s3);
                temp = 0;
                if (!FIFO_EMPTY || s3->force_busy)
                        temp |= 0x02; /*Hardware busy*/
                else
                        temp |= 0x04; /*FIFO empty*/
                s3->force_busy = 0;
                if (FIFO_FULL)
                        temp |= 0xf8; /*FIFO full*/
                return temp;

                case 0xa2e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.bkgd_color & 0xff;
                case 0xa2e9:
                s3_wait_fifo_idle(s3);
                return s3->accel.bkgd_color >> 8;
                case 0xa2ea:
                s3_wait_fifo_idle(s3);
                return s3->accel.bkgd_color >> 16;
                case 0xa2eb:
                s3_wait_fifo_idle(s3);
                return s3->accel.bkgd_color >> 24;

                case 0xa6e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.frgd_color & 0xff;
                case 0xa6e9:
                s3_wait_fifo_idle(s3);
                return s3->accel.frgd_color >> 8;
                case 0xa6ea:
                s3_wait_fifo_idle(s3);
                return s3->accel.frgd_color >> 16;
                case 0xa6eb:
                s3_wait_fifo_idle(s3);
                return s3->accel.frgd_color >> 24;

                case 0xaae8:
                s3_wait_fifo_idle(s3);
                return s3->accel.wrt_mask & 0xff;
                case 0xaae9:
                s3_wait_fifo_idle(s3);
                return s3->accel.wrt_mask >> 8;
                case 0xaaea:
                s3_wait_fifo_idle(s3);
                return s3->accel.wrt_mask >> 16;
                case 0xaaeb:
                s3_wait_fifo_idle(s3);
                return s3->accel.wrt_mask >> 24;

                case 0xaee8:
                s3_wait_fifo_idle(s3);
                return s3->accel.rd_mask & 0xff;
                case 0xaee9:
                s3_wait_fifo_idle(s3);
                return s3->accel.rd_mask >> 8;
                case 0xaeea:
                s3_wait_fifo_idle(s3);
                return s3->accel.rd_mask >> 16;
                case 0xaeeb:
                s3_wait_fifo_idle(s3);
                return s3->accel.rd_mask >> 24;

                case 0xb2e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.color_cmp & 0xff;
                case 0xb2e9:
                s3_wait_fifo_idle(s3);
                return s3->accel.color_cmp >> 8;
                case 0xb2ea:
                s3_wait_fifo_idle(s3);
                return s3->accel.color_cmp >> 16;
                case 0xb2eb:
                s3_wait_fifo_idle(s3);
                return s3->accel.color_cmp >> 24;

                case 0xb6e8:
                s3_wait_fifo_idle(s3);
                return s3->accel.bkgd_mix;

                case 0xbae8:
                s3_wait_fifo_idle(s3);
                return s3->accel.frgd_mix;

                case 0xbee8:
                s3_wait_fifo_idle(s3);
                temp = s3->accel.multifunc[0xf] & 0xf;
                switch (temp)
                {
                        case 0x0: return s3->accel.multifunc[0x0] & 0xff;
                        case 0x1: return s3->accel.multifunc[0x1] & 0xff;
                        case 0x2: return s3->accel.multifunc[0x2] & 0xff;
                        case 0x3: return s3->accel.multifunc[0x3] & 0xff;
                        case 0x4: return s3->accel.multifunc[0x4] & 0xff;
                        case 0x5: return s3->accel.multifunc[0xa] & 0xff;
                        case 0x6: return s3->accel.multifunc[0xe] & 0xff;
                        case 0x7: return s3->accel.cmd            & 0xff;
                        case 0x8: return s3->accel.subsys_cntl    & 0xff;
                        case 0x9: return s3->accel.setup_md       & 0xff;
                        case 0xa: return s3->accel.multifunc[0xd] & 0xff;
                }
                return 0xff;
                case 0xbee9:
                s3_wait_fifo_idle(s3);
                temp = s3->accel.multifunc[0xf] & 0xf;
                s3->accel.multifunc[0xf]++;
                switch (temp)
                {
                        case 0x0: return  s3->accel.multifunc[0x0] >> 8;
                        case 0x1: return  s3->accel.multifunc[0x1] >> 8;
                        case 0x2: return  s3->accel.multifunc[0x2] >> 8;
                        case 0x3: return  s3->accel.multifunc[0x3] >> 8;
                        case 0x4: return  s3->accel.multifunc[0x4] >> 8;
                        case 0x5: return  s3->accel.multifunc[0xa] >> 8;
                        case 0x6: return  s3->accel.multifunc[0xe] >> 8;
                        case 0x7: return  s3->accel.cmd            >> 8;
                        case 0x8: return (s3->accel.subsys_cntl    >> 8) & ~0xe000;
                        case 0x9: return (s3->accel.setup_md       >> 8) & ~0xf000;
                        case 0xa: return  s3->accel.multifunc[0xd] >> 8;
                }
                return 0xff;

                case 0xe2e8: case 0xe2e9: case 0xe2ea: case 0xe2eb: /*PIX_TRANS*/
                break;
        }
        return 0;
}

void s3_accel_write(uint32_t addr, uint8_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
//        pclog("s3_accel_write %08x %02x\n", addr, val);        
        s3_queue(s3, addr & 0xffff, val, FIFO_WRITE_BYTE);
}
void s3_accel_write_w(uint32_t addr, uint16_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
//        pclog("s3_accel_write_w %08x %04x\n", addr, val);        
        s3_queue(s3, addr & 0xffff, val, FIFO_WRITE_WORD);
}
void s3_accel_write_l(uint32_t addr, uint32_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
//        pclog("s3_accel_write_l %08x %08x\n", addr, val);        
        s3_queue(s3, addr & 0xffff, val, FIFO_WRITE_DWORD);
}

uint8_t s3_accel_read(uint32_t addr, void *p)
{
        if (addr & 0x8000)
           return s3_accel_in(addr & 0xffff, p);
        return 0;
}

static void polygon_setup(s3_t *s3)
{
        if (s3->accel.point_1_updated)
        {
                int start_x = s3->accel.poly_cx;
                int start_y = s3->accel.poly_cy;
                int end_x = s3->accel.destx_distp << 20;
                int end_y = s3->accel.desty_axstp;
                                
                if (end_y - start_y)
                        s3->accel.poly_dx1 = (end_x - start_x) / (end_y - start_y);
                else
                        s3->accel.poly_dx1 = 0;
                        
                s3->accel.point_1_updated = 0;

                if (end_y == s3->accel.poly_cy)
                {
                        s3->accel.poly_cx = end_x;
                        s3->accel.poly_x = end_x >> 20;
                }
        }
        if (s3->accel.point_2_updated)
        {
                int start_x = s3->accel.poly_cx2;
                int start_y = s3->accel.poly_cy2;
                int end_x = s3->accel.x2 << 20;
                int end_y = s3->accel.desty_axstp2;

                if (end_y - start_y)                                
                        s3->accel.poly_dx2 = (end_x - start_x) / (end_y - start_y);
                else
                        s3->accel.poly_dx2 = 0;

                s3->accel.point_2_updated = 0;
                                
                if (end_y == s3->accel.poly_cy)
                        s3->accel.poly_cx2 = end_x;
        }
}

/*Remap address for chain-4/doubleword style layout*/
static inline uint32_t dword_remap(uint32_t in_addr)
{
    return in_addr;
}
static inline uint32_t dword_remap_w(uint32_t in_addr)
{
    return in_addr;
}
static inline uint32_t dword_remap_l(uint32_t in_addr)
{
    return in_addr;
}

#define READ_SRC(addr, dat) if (s3->bpp == 0)      dat = svga->vram[  dword_remap(addr) & s3->vram_mask]; \
                            else if (s3->bpp == 1) dat = vram_w[dword_remap_w(addr) & (s3->vram_mask >> 1)]; \
                            else                   dat = vram_l[dword_remap_l(addr) & (s3->vram_mask >> 2)]; \
                            if (vram_mask)                                           \
                                    dat = ((dat & rd_mask) == rd_mask);

#define READ_DST(addr, dat) if (s3->bpp == 0)      dat = svga->vram[  dword_remap(addr) & s3->vram_mask]; \
                            else if (s3->bpp == 1) dat = vram_w[dword_remap_w(addr) & (s3->vram_mask >> 1)]; \
                            else                   dat = vram_l[dword_remap_l(addr) & (s3->vram_mask >> 2)];

#define MIX     {                                                                                               \
                        uint32_t old_dest_dat = dest_dat;                                                       \
                        switch ((mix_dat & mix_mask) ? (s3->accel.frgd_mix & 0xf) : (s3->accel.bkgd_mix & 0xf)) \
                        {                                                                                       \
                                case 0x0: dest_dat =             ~dest_dat;  break;                             \
                                case 0x1: dest_dat =  0;                     break;                             \
                                case 0x2: dest_dat = ~0;                     break;                             \
                                case 0x3: dest_dat =              dest_dat;  break;                             \
                                case 0x4: dest_dat =  ~src_dat;              break;                             \
                                case 0x5: dest_dat =   src_dat ^  dest_dat;  break;                             \
                                case 0x6: dest_dat = ~(src_dat ^  dest_dat); break;                             \
                                case 0x7: dest_dat =   src_dat;              break;                             \
                                case 0x8: dest_dat = ~(src_dat &  dest_dat); break;                             \
                                case 0x9: dest_dat =  ~src_dat |  dest_dat;  break;                             \
                                case 0xa: dest_dat =   src_dat | ~dest_dat;  break;                             \
                                case 0xb: dest_dat =   src_dat |  dest_dat;  break;                             \
                                case 0xc: dest_dat =   src_dat &  dest_dat;  break;                             \
                                case 0xd: dest_dat =   src_dat & ~dest_dat;  break;                             \
                                case 0xe: dest_dat =  ~src_dat &  dest_dat;  break;                             \
                                case 0xf: dest_dat = ~(src_dat |  dest_dat); break;                             \
                        }                                                                                       \
                        dest_dat = (dest_dat & s3->accel.wrt_mask) | (old_dest_dat & ~s3->accel.wrt_mask);      \
                }


#define WRITE(addr)     if (s3->bpp == 0)                                                                               \
                        {                                                                                               \
                                svga->vram[dword_remap(addr) & s3->vram_mask] = dest_dat;                                          \
                                svga->changedvram[(dword_remap(addr) & s3->vram_mask) >> 12] = changeframecount;                   \
                        }                                                                                               \
                        else if (s3->bpp == 1)                                                                          \
                        {                                                                                               \
                                vram_w[dword_remap_w(addr) & (s3->vram_mask >> 1)] = dest_dat;                                       \
                                svga->changedvram[(dword_remap_w(addr) & (s3->vram_mask >> 1)) >> 11] = changeframecount;            \
                        }                                                                                               \
                        else                                                                                            \
                        {                                                                                               \
                                vram_l[dword_remap_l(addr) & (s3->vram_mask >> 2)] = dest_dat;                                       \
                                svga->changedvram[(dword_remap_l(addr) & (s3->vram_mask >> 2)) >> 10] = changeframecount;            \
                        }

void s3_accel_start(int count, int cpu_input, uint32_t mix_dat, uint32_t cpu_dat, s3_t *s3)
{
        svga_t *svga = &s3->svga;
        uint32_t src_dat, dest_dat;
        int frgd_mix, bkgd_mix;
        int clip_t = s3->accel.multifunc[1] & 0xfff;
        int clip_l = s3->accel.multifunc[2] & 0xfff;
        int clip_b = s3->accel.multifunc[3] & 0xfff;
        int clip_r = s3->accel.multifunc[4] & 0xfff;
        int vram_mask = (s3->accel.multifunc[0xa] & 0xc0) == 0xc0;
        uint32_t mix_mask = 0;
        uint16_t *vram_w = (uint16_t *)svga->vram;
        uint32_t *vram_l = (uint32_t *)svga->vram;
        uint32_t compare = s3->accel.color_cmp;
        int compare_mode = (s3->accel.multifunc[0xe] >> 7) & 3;
        uint32_t rd_mask = s3->accel.rd_mask;
        int cmd = s3->accel.cmd >> 13;
        uint32_t srcbase, dstbase;
        
        if ((s3->chip == S3_TRIO64) && (s3->accel.cmd & (1 << 11)))
                cmd |= 8;

        // SRC-BASE/DST-BASE
        if ((s3->accel.multifunc[13] >> 4) & 7) {
            srcbase = 0x100000 * ((s3->accel.multifunc[13] >> 4) & 3);
        } else {
            srcbase = 0x100000 * ((s3->accel.multifunc[14] >> 2) & 3);
        }
        if ((s3->accel.multifunc[13] >> 0) & 7) {
            dstbase = 0x100000 * ((s3->accel.multifunc[13] >> 0) & 3);
        } else {
            dstbase = 0x100000 * ((s3->accel.multifunc[14] >> 0) & 3);
        }
        if (s3->bpp == 1) {
            srcbase >>= 1;
            dstbase >>= 1;
        } else if (s3->bpp == 3) {
            srcbase >>= 2;
            dstbase >>= 2;
        }

        // Amiga CGX4 driver sets all clipping values to FFF.
        if (clip_t == 0xfff)
            clip_t = 0;
        if (clip_l == 0xfff)
            clip_l = 0;

        s3->force_busy = 1;
        //return;
        //if (!cpu_input) pclog("Start S3 command %i  %i, %i  %i, %i (clip %i, %i to %i, %i  %i)\n", s3->accel.cmd >> 13, s3->accel.cur_x, s3->accel.cur_y, s3->accel.maj_axis_pcnt & 0xfff, s3->accel.multifunc[0]  & 0xfff, clip_l, clip_t, clip_r, clip_b, s3->accel.multifunc[0xe] & 0x20);
        //else            pclog("      S3 command %i, %i, %08x %08x\n", s3->accel.cmd >> 13, count, mix_dat, cpu_dat);

        if (!cpu_input) s3->accel.dat_count = 0;
        if (cpu_input && (s3->accel.multifunc[0xa] & 0xc0) != 0x80)
        {
                if (s3->bpp == 3 && count == 2)
                {
                        if (s3->accel.dat_count)
                        {
                                cpu_dat = ((cpu_dat & 0xffff) << 16) | s3->accel.dat_buf;
                                count = 4;
                                s3->accel.dat_count = 0;
                        }
                        else
                        {
                                s3->accel.dat_buf = cpu_dat & 0xffff;
                                s3->accel.dat_count = 1;
                        }
                }
                if (s3->bpp == 1) count >>= 1;
                if (s3->bpp == 3) count >>= 2;
        }

        if (s3->bpp == 0)
                rd_mask &= 0xff;
        else if (s3->bpp == 1)
                rd_mask &= 0xffff;
        
        switch (s3->accel.cmd & 0x600)
        {
                case 0x000: mix_mask = 0x80; break;
                case 0x200: mix_mask = 0x8000; break;
                case 0x400: mix_mask = 0x80000000; break;
                case 0x600: mix_mask = (s3->chip == S3_TRIO32) ? 0x80 : 0x80000000; break;
        }

        if (s3->bpp == 0) compare &=   0xff;
        if (s3->bpp == 1) compare &= 0xffff;
        switch (cmd)
        {
                case 1: /*Draw line*/
                if (!cpu_input) /*!cpu_input is trigger to start operation*/
                {
                        s3->accel.cx   = s3->accel.cur_x;
                        if (s3->accel.cur_x & 0x1000) s3->accel.cx |= ~0xfff;
                        s3->accel.cy   = s3->accel.cur_y;
                        if (s3->accel.cur_y & 0x1000) s3->accel.cy |= ~0xfff;
                        
                        s3->accel.sy = s3->accel.maj_axis_pcnt;
                }
                if ((s3->accel.cmd & 0x100) && !cpu_input) return; /*Wait for data from CPU*/

                frgd_mix = (s3->accel.frgd_mix >> 5) & 3;
                bkgd_mix = (s3->accel.bkgd_mix >> 5) & 3;

                if (s3->accel.cmd & 8) /*Radial*/
                {
                        while (count-- && s3->accel.sy >= 0)
                        {
                                if ((s3->accel.cx & 0xfff) >= clip_l && (s3->accel.cx & 0xfff) <= clip_r &&
                                    (s3->accel.cy & 0xfff) >= clip_t && (s3->accel.cy & 0xfff) <= clip_b)
                                {
                                        switch ((mix_dat & mix_mask) ? frgd_mix : bkgd_mix)
                                        {
                                                case 0: src_dat = s3->accel.bkgd_color; break;
                                                case 1: src_dat = s3->accel.frgd_color; break;
                                                case 2: src_dat = cpu_dat; break;
                                                case 3: src_dat = 0; break;
                                        }

                                        if ((compare_mode == 2 && src_dat != compare) ||
                                            (compare_mode == 3 && src_dat == compare) ||
                                             compare_mode < 2)
                                        {
                                                READ_DST((s3->accel.cy * s3->width) + s3->accel.cx, dest_dat);

                                                MIX

                                                WRITE((s3->accel.cy * s3->width) + s3->accel.cx);
                                        }
                                }

                                mix_dat <<= 1;
                                mix_dat |= 1;
                                if (s3->bpp == 0) cpu_dat >>= 8;
                                else              cpu_dat >>= 16;
                                if (!s3->accel.sy)
                                        break;

                                switch (s3->accel.cmd & 0xe0)
                                {
                                        case 0x00: s3->accel.cx++;                 break;
                                        case 0x20: s3->accel.cx++; s3->accel.cy--; break;
                                        case 0x40:                 s3->accel.cy--; break;
                                        case 0x60: s3->accel.cx--; s3->accel.cy--; break;
                                        case 0x80: s3->accel.cx--;                 break;
                                        case 0xa0: s3->accel.cx--; s3->accel.cy++; break;
                                        case 0xc0:                 s3->accel.cy++; break;
                                        case 0xe0: s3->accel.cx++; s3->accel.cy++; break;
                                }
                                s3->accel.sy--;
                        }
                        s3->accel.cur_x = s3->accel.cx;
                        s3->accel.cur_y = s3->accel.cy;
                }
                else /*Bresenham*/
                {
                        while (count-- && s3->accel.sy >= 0)
                        {
                                if ((s3->accel.cx & 0xfff) >= clip_l && (s3->accel.cx & 0xfff) <= clip_r &&
                                    (s3->accel.cy & 0xfff) >= clip_t && (s3->accel.cy & 0xfff) <= clip_b)
                                {
                                        switch ((mix_dat & mix_mask) ? frgd_mix : bkgd_mix)
                                        {
                                                case 0: src_dat = s3->accel.bkgd_color; break;
                                                case 1: src_dat = s3->accel.frgd_color; break;
                                                case 2: src_dat = cpu_dat; break;
                                                case 3: src_dat = 0; break;
                                        }

                                        if ((compare_mode == 2 && src_dat != compare) ||
                                            (compare_mode == 3 && src_dat == compare) ||
                                             compare_mode < 2)
                                        {
                                                READ_DST((s3->accel.cy * s3->width) + s3->accel.cx, dest_dat);

//                                        pclog("Line : %04i, %04i (%06X) - %02X (%02X %04X %05X) %02X (%02X %02X)  ", s3->accel.cx, s3->accel.cy, s3->accel.dest + s3->accel.cx, src_dat, vram[s3->accel.src + s3->accel.cx], mix_dat & mix_mask, s3->accel.src + s3->accel.cx, dest_dat, s3->accel.frgd_color, s3->accel.bkgd_color);
                                        
                                                MIX

//                                        pclog("%02X\n", dest_dat);

                                                WRITE((s3->accel.cy * s3->width) + s3->accel.cx);
                                        }
                                }

                                mix_dat <<= 1;
                                mix_dat |= 1;
                                if (s3->bpp == 0) cpu_dat >>= 8;
                                else             cpu_dat >>= 16;

//                                pclog("%i, %i - %i %i  %i %i\n", s3->accel.cx, s3->accel.cy, s3->accel.err_term, s3->accel.maj_axis_pcnt, s3->accel.desty_axstp, s3->accel.destx_distp);

                                if (!s3->accel.sy)
                                        break;

                                if (s3->accel.err_term >= s3->accel.maj_axis_pcnt)
                                {
                                        s3->accel.err_term += s3->accel.destx_distp;
                                        /*Step minor axis*/
                                        switch (s3->accel.cmd & 0xe0)
                                        {
                                                case 0x00: s3->accel.cy--; break;
                                                case 0x20: s3->accel.cy--; break;
                                                case 0x40: s3->accel.cx--; break;
                                                case 0x60: s3->accel.cx++; break;
                                                case 0x80: s3->accel.cy++; break;
                                                case 0xa0: s3->accel.cy++; break;
                                                case 0xc0: s3->accel.cx--; break;
                                                case 0xe0: s3->accel.cx++; break;
                                        }
                                }
                                else
                                   s3->accel.err_term += s3->accel.desty_axstp;

                                /*Step major axis*/
                                switch (s3->accel.cmd & 0xe0)
                                {
                                        case 0x00: s3->accel.cx--; break;
                                        case 0x20: s3->accel.cx++; break;
                                        case 0x40: s3->accel.cy--; break;
                                        case 0x60: s3->accel.cy--; break;
                                        case 0x80: s3->accel.cx--; break;
                                        case 0xa0: s3->accel.cx++; break;
                                        case 0xc0: s3->accel.cy++; break;
                                        case 0xe0: s3->accel.cy++; break;
                                }
                                s3->accel.sy--;
                        }
                        s3->accel.cur_x = s3->accel.cx;
                        s3->accel.cur_y = s3->accel.cy;
                }
                break;
                
                case 2: /*Rectangle fill*/
                if (!cpu_input) /*!cpu_input is trigger to start operation*/
                {
                        s3->accel.sx   = s3->accel.maj_axis_pcnt & 0xfff;
                        s3->accel.sy   = s3->accel.multifunc[0]  & 0xfff;
                        s3->accel.cx   = s3->accel.cur_x;
                        if (s3->accel.cur_x & 0x1000) s3->accel.cx |= ~0xfff;
                        s3->accel.cy   = s3->accel.cur_y;
                        if (s3->accel.cur_y & 0x1000) s3->accel.cy |= ~0xfff;
                        
                        s3->accel.dest = dstbase + s3->accel.cy * s3->width;

//                        pclog("Dest %08X  (%i, %i) %04X %04X\n", s3->accel.dest, s3->accel.cx, s3->accel.cy, s3->accel.cur_x, s3->accel.cur_x & 0x1000);
                }
                if ((s3->accel.cmd & 0x100) && !cpu_input) return; /*Wait for data from CPU*/
//                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80 && !cpu_input) /*Mix data from CPU*/
//                   return;

                frgd_mix = (s3->accel.frgd_mix >> 5) & 3;
                bkgd_mix = (s3->accel.bkgd_mix >> 5) & 3;
                
                while (count-- && s3->accel.sy >= 0)
                {
                        if ((s3->accel.cx & 0xfff) >= clip_l && (s3->accel.cx & 0xfff) <= clip_r &&
                            (s3->accel.cy & 0xfff) >= clip_t && (s3->accel.cy & 0xfff) <= clip_b)
                        {
                                switch ((mix_dat & mix_mask) ? frgd_mix : bkgd_mix)
                                {
                                        case 0: src_dat = s3->accel.bkgd_color; break;
                                        case 1: src_dat = s3->accel.frgd_color; break;
                                        case 2: src_dat = cpu_dat; break;
                                        case 3: src_dat = 0; break;
                                }

                                if ((compare_mode == 2 && src_dat != compare) ||
                                    (compare_mode == 3 && src_dat == compare) ||
                                     compare_mode < 2)
                                {
                                        READ_DST(s3->accel.dest + s3->accel.cx, dest_dat);
                                

//                                if (CS != 0xc000) pclog("Write %05X  %02X %02X  %04X (%02X %02X)  ", s3->accel.dest + s3->accel.cx, src_dat, dest_dat, mix_dat, s3->accel.frgd_mix, s3->accel.bkgd_mix);

                                        MIX
                                
//                                if (CS != 0xc000) pclog("%02X\n", dest_dat);
                                
                                        WRITE(s3->accel.dest + s3->accel.cx);
                                }
                        }
                
                        mix_dat <<= 1;
                        mix_dat |= 1;
                        if (s3->bpp == 0) cpu_dat >>= 8;
                        else             cpu_dat >>= 16;
                        
                        if (s3->accel.cmd & 0x20) s3->accel.cx++;
                        else                     s3->accel.cx--;
                        s3->accel.sx--;
                        if (s3->accel.sx < 0)
                        {
                                if (s3->accel.cmd & 0x20) s3->accel.cx   -= (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                else                     s3->accel.cx   += (s3->accel.maj_axis_pcnt & 0xfff) + 1;
//                                s3->accel.dest -= (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                s3->accel.sx    = s3->accel.maj_axis_pcnt & 0xfff;
                                
//                                s3->accel.dest  += s3_width;
                                if (s3->accel.cmd & 0x80) s3->accel.cy++;
                                else                     s3->accel.cy--;
                                
                                s3->accel.dest = dstbase + s3->accel.cy * s3->width;
                                s3->accel.sy--;

                                if (cpu_input/* && (s3->accel.multifunc[0xa] & 0xc0) == 0x80*/) return;
                                if (s3->accel.sy < 0)
                                {
                                        s3->accel.cur_x = s3->accel.cx;
                                        s3->accel.cur_y = s3->accel.cy;
                                        return;
                                }
                        }
                }
                break;

                case 6: /*BitBlt*/
                if (!cpu_input) /*!cpu_input is trigger to start operation*/
                {
                        s3->accel.sx   = s3->accel.maj_axis_pcnt & 0xfff;
                        s3->accel.sy   = s3->accel.multifunc[0]  & 0xfff;

                        s3->accel.dx   = s3->accel.destx_distp & 0xfff;
                        if (s3->accel.destx_distp & 0x1000) s3->accel.dx |= ~0xfff;
                        s3->accel.dy   = s3->accel.desty_axstp & 0xfff;
                        if (s3->accel.desty_axstp & 0x1000) s3->accel.dy |= ~0xfff;

                        s3->accel.cx   = s3->accel.cur_x & 0xfff;
                        if (s3->accel.cur_x & 0x1000) s3->accel.cx |= ~0xfff;
                        s3->accel.cy   = s3->accel.cur_y & 0xfff;
                        if (s3->accel.cur_y & 0x1000) s3->accel.cy |= ~0xfff;

                        s3->accel.src  = srcbase + s3->accel.cy * s3->width;
                        s3->accel.dest = dstbase + s3->accel.dy * s3->width;
                        
//                        pclog("Source %08X Dest %08X  (%i, %i) - (%i, %i)\n", s3->accel.src, s3->accel.dest, s3->accel.cx, s3->accel.cy, s3->accel.dx, s3->accel.dy);
                }
                if ((s3->accel.cmd & 0x100) && !cpu_input) return; /*Wait for data from CPU*/
//                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80 && !cpu_input) /*Mix data from CPU*/
//                  return;

                if (s3->accel.sy < 0)
                   return;

                frgd_mix = (s3->accel.frgd_mix >> 5) & 3;
                bkgd_mix = (s3->accel.bkgd_mix >> 5) & 3;
                
                if (!cpu_input && frgd_mix == 3 && !vram_mask && !compare_mode &&
                    (s3->accel.cmd & 0xa0) == 0xa0 && (s3->accel.frgd_mix & 0xf) == 7) 
                {
                        while (1)
                        {
                                if ((s3->accel.dx & 0xfff) >= clip_l && (s3->accel.dx & 0xfff) <= clip_r &&
                                    (s3->accel.dy & 0xfff) >= clip_t && (s3->accel.dy & 0xfff) <= clip_b)
                                {
                                        READ_SRC(s3->accel.src + s3->accel.cx, src_dat);
                                        READ_DST(s3->accel.dest + s3->accel.dx, dest_dat);  
                                        
                                        dest_dat = (src_dat & s3->accel.wrt_mask) | (dest_dat & ~s3->accel.wrt_mask);
                                        
                                        WRITE(s3->accel.dest + s3->accel.dx);
                                }
        
                                s3->accel.cx++;
                                s3->accel.dx++;
                                s3->accel.sx--;
                                if (s3->accel.sx < 0)
                                {
                                        s3->accel.cx -= (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                        s3->accel.dx -= (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                        s3->accel.sx  =  s3->accel.maj_axis_pcnt & 0xfff;
        
                                        s3->accel.cy++;
                                        s3->accel.dy++;
        
                                        s3->accel.src  = srcbase + s3->accel.cy * s3->width;
                                        s3->accel.dest = dstbase + s3->accel.dy * s3->width;
        
                                        s3->accel.sy--;
        
                                        if (s3->accel.sy < 0)
                                                return;
                                }
                        }
                }
                else
                {                     
                        while (count-- && s3->accel.sy >= 0)
                        {
                                if ((s3->accel.dx & 0xfff) >= clip_l && (s3->accel.dx & 0xfff) <= clip_r &&
                                    (s3->accel.dy & 0xfff) >= clip_t && (s3->accel.dy & 0xfff) <= clip_b)
                                {
                                        if (vram_mask)
                                        {
                                                READ_SRC(s3->accel.src + s3->accel.cx, mix_dat)

                                                mix_dat = mix_dat ? mix_mask : 0;
                                        }
                                        switch ((mix_dat & mix_mask) ? frgd_mix : bkgd_mix)
                                        {
                                                case 0: src_dat = s3->accel.bkgd_color;                  break;
                                                case 1: src_dat = s3->accel.frgd_color;                  break;
                                                case 2: src_dat = cpu_dat;                              break;
                                                case 3: READ_SRC(s3->accel.src + s3->accel.cx, src_dat);      break;
                                        }

                                        if ((compare_mode == 2 && src_dat != compare) ||
                                            (compare_mode == 3 && src_dat == compare) ||
                                             compare_mode < 2)
                                        {
                                                READ_DST(s3->accel.dest + s3->accel.dx, dest_dat);
                                                                                
//                                pclog("BitBlt : %04i, %04i (%06X) - %02X (%02X %04X %05X) %02X   ", s3->accel.dx, s3->accel.dy, s3->accel.dest + s3->accel.dx, src_dat, vram[s3->accel.src + s3->accel.cx], mix_dat, s3->accel.src + s3->accel.cx, dest_dat);
                                
                                                MIX

//                                pclog("%02X\n", dest_dat);

                                                WRITE(s3->accel.dest + s3->accel.dx);
                                        }
                                }

                                mix_dat <<= 1;
                                mix_dat |= 1;
                                if (s3->bpp == 0) cpu_dat >>= 8;
                                else             cpu_dat >>= 16;

                                if (s3->accel.cmd & 0x20)
                                {
                                        s3->accel.cx++;
                                        s3->accel.dx++;
                                }
                                else
                                {
                                        s3->accel.cx--;
                                        s3->accel.dx--;
                                }
                                s3->accel.sx--;
                                if (s3->accel.sx < 0)
                                {
                                        if (s3->accel.cmd & 0x20)
                                        {
                                                s3->accel.cx -= (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                                s3->accel.dx -= (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                        }
                                        else
                                        {
                                                s3->accel.cx += (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                                s3->accel.dx += (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                        }
                                        s3->accel.sx    =  s3->accel.maj_axis_pcnt & 0xfff;

                                        if (s3->accel.cmd & 0x80)
                                        {
                                                s3->accel.cy++;
                                                s3->accel.dy++;
                                        }
                                        else
                                        {
                                                s3->accel.cy--;
                                                s3->accel.dy--;
                                        }

                                        s3->accel.src  = srcbase + s3->accel.cy * s3->width;
                                        s3->accel.dest = dstbase + s3->accel.dy * s3->width;

                                        s3->accel.sy--;

                                        if (cpu_input/* && (s3->accel.multifunc[0xa] & 0xc0) == 0x80*/) return;
                                        if (s3->accel.sy < 0)
                                                return;
                                }
                        }
                }
                break;

                case 7: /*Pattern fill - BitBlt but with source limited to 8x8*/
                if (!cpu_input) /*!cpu_input is trigger to start operation*/
                {
                        s3->accel.sx   = s3->accel.maj_axis_pcnt & 0xfff;
                        s3->accel.sy   = s3->accel.multifunc[0]  & 0xfff;

                        s3->accel.dx   = s3->accel.destx_distp & 0xfff;
                        if (s3->accel.destx_distp & 0x1000) s3->accel.dx |= ~0xfff;
                        s3->accel.dy   = s3->accel.desty_axstp & 0xfff;
                        if (s3->accel.desty_axstp & 0x1000) s3->accel.dy |= ~0xfff;

                        s3->accel.cx   = s3->accel.cur_x & 0xfff;
                        if (s3->accel.cur_x & 0x1000) s3->accel.cx |= ~0xfff;
                        s3->accel.cy   = s3->accel.cur_y & 0xfff;
                        if (s3->accel.cur_y & 0x1000) s3->accel.cy |= ~0xfff;
                        
                        /*Align source with destination*/
//                        s3->accel.cx = (s3->accel.cx & ~7) | (s3->accel.dx & 7);
//                        s3->accel.cy = (s3->accel.cy & ~7) | (s3->accel.dy & 7);

                        s3->accel.pattern  = (s3->accel.cy * s3->width) + s3->accel.cx;
                        s3->accel.dest     = dstbase + s3->accel.dy * s3->width;
                        
                        s3->accel.cx = s3->accel.dx & 7;
                        s3->accel.cy = s3->accel.dy & 7;
                        
                        s3->accel.src  = srcbase + s3->accel.pattern + (s3->accel.cy * s3->width);

//                        pclog("Source %08X Dest %08X  (%i, %i) - (%i, %i)\n", s3->accel.src, s3->accel.dest, s3->accel.cx, s3->accel.cy, s3->accel.dx, s3->accel.dy);
//                        dumpregs();
//                        exit(-1);
                }
                if ((s3->accel.cmd & 0x100) && !cpu_input) return; /*Wait for data from CPU*/
//                if ((s3->accel.multifunc[0xa] & 0xc0) == 0x80 && !cpu_input) /*Mix data from CPU*/
//                   return;

                frgd_mix = (s3->accel.frgd_mix >> 5) & 3;
                bkgd_mix = (s3->accel.bkgd_mix >> 5) & 3;

                while (count-- && s3->accel.sy >= 0)
                {
                        if ((s3->accel.dx & 0xfff) >= clip_l && (s3->accel.dx & 0xfff) <= clip_r &&
                            (s3->accel.dy & 0xfff) >= clip_t && (s3->accel.dy & 0xfff) <= clip_b)
                        {
                                if (vram_mask)
                                {
                                        READ_SRC(s3->accel.src + s3->accel.cx, mix_dat)
                                        mix_dat = mix_dat ? mix_mask : 0;
                                }
                                switch ((mix_dat & mix_mask) ? frgd_mix : bkgd_mix)
                                {
                                        case 0: src_dat = s3->accel.bkgd_color;                  break;
                                        case 1: src_dat = s3->accel.frgd_color;                  break;
                                        case 2: src_dat = cpu_dat;                              break;
                                        case 3: READ_SRC(s3->accel.src + s3->accel.cx, src_dat);      break;
                                }

                                if ((compare_mode == 2 && src_dat != compare) ||
                                    (compare_mode == 3 && src_dat == compare) ||
                                     compare_mode < 2)
                                {
                                        READ_DST(s3->accel.dest + s3->accel.dx, dest_dat);
                                
//                                pclog("Pattern fill : %04i, %04i (%06X) - %02X (%02X %04X %05X) %02X   ", s3->accel.dx, s3->accel.dy, s3->accel.dest + s3->accel.dx, src_dat, vram[s3->accel.src + s3->accel.cx], mix_dat, s3->accel.src + s3->accel.cx, dest_dat);

                                        MIX

//                                pclog("%02X\n", dest_dat);

                                        WRITE(s3->accel.dest + s3->accel.dx);
                                }
                        }

                        mix_dat <<= 1;
                        mix_dat |= 1;
                        if (s3->bpp == 0) cpu_dat >>= 8;
                        else             cpu_dat >>= 16;

                        if (s3->accel.cmd & 0x20)
                        {
                                s3->accel.cx = ((s3->accel.cx + 1) & 7) | (s3->accel.cx & ~7);
                                s3->accel.dx++;
                        }
                        else
                        {
                                s3->accel.cx = ((s3->accel.cx - 1) & 7) | (s3->accel.cx & ~7);
                                s3->accel.dx--;
                        }
                        s3->accel.sx--;
                        if (s3->accel.sx < 0)
                        {
                                if (s3->accel.cmd & 0x20)
                                {
                                        s3->accel.cx = ((s3->accel.cx - ((s3->accel.maj_axis_pcnt & 0xfff) + 1)) & 7) | (s3->accel.cx & ~7);
                                        s3->accel.dx -= (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                }
                                else
                                {
                                        s3->accel.cx = ((s3->accel.cx + ((s3->accel.maj_axis_pcnt & 0xfff) + 1)) & 7) | (s3->accel.cx & ~7);
                                        s3->accel.dx += (s3->accel.maj_axis_pcnt & 0xfff) + 1;
                                }
                                s3->accel.sx    =  s3->accel.maj_axis_pcnt & 0xfff;

                                if (s3->accel.cmd & 0x80)
                                {
                                        s3->accel.cy = ((s3->accel.cy + 1) & 7) | (s3->accel.cy & ~7);
                                        s3->accel.dy++;
                                }
                                else
                                {
                                        s3->accel.cy = ((s3->accel.cy - 1) & 7) | (s3->accel.cy & ~7);
                                        s3->accel.dy--;
                                }

                                s3->accel.src  = srcbase + s3->accel.pattern + (s3->accel.cy * s3->width);
                                s3->accel.dest = dstbase + s3->accel.dy * s3->width;

                                s3->accel.sy--;

                                if (cpu_input/* && (s3->accel.multifunc[0xa] & 0xc0) == 0x80*/) return;
                                if (s3->accel.sy < 0)
                                        return;
                        }
                }
                break;
                
                case 3: /*Polygon Fill Solid (Trio64 only)*/
                {                      
                        int end_y1, end_y2;
                          
                        if (s3->chip != S3_TRIO64)
                                break;

                        polygon_setup(s3);

                        if ((s3->accel.cmd & 0x100) && !cpu_input) return; /*Wait for data from CPU*/
                        
                        end_y1 = s3->accel.desty_axstp;
                        end_y2 = s3->accel.desty_axstp2;

                        frgd_mix = (s3->accel.frgd_mix >> 5) & 3;
                        

                        while ((s3->accel.poly_cy < end_y1) && (s3->accel.poly_cy2 < end_y2))
                        {
                                int y = s3->accel.poly_cy;
                                int x_count = ABS((s3->accel.poly_cx2 >> 20) - s3->accel.poly_x) + 1;

                                s3->accel.dest = dstbase + y * s3->width;
                                
                                while (x_count-- && count--)
                                {
                                        if ((s3->accel.poly_x & 0xfff) >= clip_l && (s3->accel.poly_x & 0xfff) <= clip_r &&
                                            (s3->accel.poly_cy & 0xfff) >= clip_t && (s3->accel.poly_cy & 0xfff) <= clip_b)
                                        {
                                                switch (frgd_mix)
                                                {
                                                        case 0: src_dat = s3->accel.bkgd_color; break;
                                                        case 1: src_dat = s3->accel.frgd_color; break;
                                                        case 2: src_dat = cpu_dat;              break;
                                                        case 3: src_dat = 0; /*Nor supported?*/ break;
                                                }

                                                if ((compare_mode == 2 && src_dat != compare) ||
                                                    (compare_mode == 3 && src_dat == compare) ||
                                                     compare_mode < 2)
                                                {
                                                        READ_DST(s3->accel.dest + s3->accel.poly_x, dest_dat);
                                
                                                        MIX

                                                        WRITE(s3->accel.dest + s3->accel.poly_x);
                                                }
                                        }
                                        if (s3->bpp == 0) cpu_dat >>= 8;
                                        else              cpu_dat >>= 16;

                                        if (s3->accel.poly_x < (s3->accel.poly_cx2 >> 20))
                                                s3->accel.poly_x++;
                                        else
                                                s3->accel.poly_x--;
                                }
                                
                                s3->accel.poly_cx += s3->accel.poly_dx1;
                                s3->accel.poly_cx2 += s3->accel.poly_dx2;
                                s3->accel.poly_x = s3->accel.poly_cx >> 20;
                                                                
                                s3->accel.poly_cy++;
                                s3->accel.poly_cy2++;
                                
                                if (!count)
                                        break;
                        }
                
                        s3->accel.cur_x = s3->accel.poly_cx & 0xfff;
                        s3->accel.cur_y = s3->accel.poly_cy & 0xfff;
                        s3->accel.cur_x2 = s3->accel.poly_cx2 & 0xfff;
                        s3->accel.cur_y2 = s3->accel.poly_cy & 0xfff;
                }
                break;

                case 11: /*Polygon Fill Pattern (Trio64 only)*/
                {                      
                        int end_y1, end_y2;
                          
                        if (s3->chip != S3_TRIO64)
                                break;

                        polygon_setup(s3);

                        if ((s3->accel.cmd & 0x100) && !cpu_input) return; /*Wait for data from CPU*/
                        
                        end_y1 = s3->accel.desty_axstp;
                        end_y2 = s3->accel.desty_axstp2;

                        frgd_mix = (s3->accel.frgd_mix >> 5) & 3;
                        bkgd_mix = (s3->accel.bkgd_mix >> 5) & 3;
                        
                        while ((s3->accel.poly_cy < end_y1) && (s3->accel.poly_cy2 < end_y2))
                        {
                                int y = s3->accel.poly_cy;
                                int x_count = ABS((s3->accel.poly_cx2 >> 20) - s3->accel.poly_x) + 1;

                                s3->accel.src  = srcbase + s3->accel.pattern + ((y & 7) * s3->width);
                                s3->accel.dest = dstbase + y * s3->width;
                                
                                while (x_count-- && count--)
                                {
                                        int pat_x = s3->accel.poly_x & 7;
                                        
                                        if ((s3->accel.poly_x & 0xfff) >= clip_l && (s3->accel.poly_x & 0xfff) <= clip_r &&
                                            (s3->accel.poly_cy & 0xfff) >= clip_t && (s3->accel.poly_cy & 0xfff) <= clip_b)
                                        {
                                                if (vram_mask)
                                                {
                                                        READ_SRC(s3->accel.src + pat_x, mix_dat)
                                                        mix_dat = mix_dat ? mix_mask : 0;
                                                }
                                                switch ((mix_dat & mix_mask) ? frgd_mix : bkgd_mix)
                                                {
                                                        case 0: src_dat = s3->accel.bkgd_color; break;
                                                        case 1: src_dat = s3->accel.frgd_color; break;
                                                        case 2: src_dat = cpu_dat;              break;
                                                        case 3: READ_SRC(s3->accel.src + pat_x, src_dat); break;
                                                }

                                                if ((compare_mode == 2 && src_dat != compare) ||
                                                    (compare_mode == 3 && src_dat == compare) ||
                                                     compare_mode < 2)
                                                {
                                                        READ_DST(s3->accel.dest + s3->accel.poly_x, dest_dat);
                                
                                                        MIX

                                                        WRITE(s3->accel.dest + s3->accel.poly_x);
                                                }
                                        }
                                        if (s3->bpp == 0) cpu_dat >>= 8;
                                        else              cpu_dat >>= 16;

                                        mix_dat <<= 1;
                                        mix_dat |= 1;

                                        if (s3->accel.poly_x < (s3->accel.poly_cx2 >> 20))
                                                s3->accel.poly_x++;
                                        else
                                                s3->accel.poly_x--;
                                }
                                
                                s3->accel.poly_cx += s3->accel.poly_dx1;
                                s3->accel.poly_cx2 += s3->accel.poly_dx2;
                                s3->accel.poly_x = s3->accel.poly_cx >> 20;
                                                                
                                s3->accel.poly_cy++;
                                s3->accel.poly_cy2++;
                                
                                if (!count)
                                        break;
                        }
                
                        s3->accel.cur_x = s3->accel.poly_cx & 0xfff;
                        s3->accel.cur_y = s3->accel.poly_cy & 0xfff;
                        s3->accel.cur_x2 = s3->accel.poly_cx2 & 0xfff;
                        s3->accel.cur_y2 = s3->accel.poly_cy & 0xfff;
                }
                break;
        }
}

void s3_hwcursor_draw(svga_t *svga, int displine)
{
        s3_t *s3 = (s3_t *)svga->priv;
        int x;
        uint16_t dat[2];
        int xx;
        int offset = svga->hwcursor_latch.x - svga->hwcursor_latch.xoff;
        uint32_t fg = 0, bg = 0;

        offset <<= svga->horizontal_linedbl;

        switch (svga->bpp)
        {               
                case 15:
                fg = video_15to32[s3->hwc_fg_col & 0xffff];
                bg = video_15to32[s3->hwc_bg_col & 0xffff];
                break;
                
                case 16:
                fg = video_16to32[s3->hwc_fg_col & 0xffff];
                bg = video_16to32[s3->hwc_bg_col & 0xffff];
                break;
                
                case 24: case 32:
                fg = s3->hwc_fg_col;
                bg = s3->hwc_bg_col;
                break;

                default:
                if (s3->chip == S3_TRIO32 || s3->chip == S3_TRIO64)
                {
                        fg = svga->pallook[s3->hwc_fg_col & 0xff];
                        bg = svga->pallook[s3->hwc_bg_col & 0xff];
                }
                else
                {
                        fg = svga->pallook[svga->crtc[0xe]];
                        bg = svga->pallook[svga->crtc[0xf]];
                }
                break;
        }

        if (svga->interlace && svga->hwcursor_oddeven)
                svga->hwcursor_latch.addr += 16;

//        pclog("HWcursor %i %i\n", svga->hwcursor_latch.x, svga->hwcursor_latch.y);
        for (x = 0; x < 64; x += 16)
        {
                uint32_t remapped_addr = dword_remap(svga->hwcursor_latch.addr);

                dat[0] = (svga->vram[remapped_addr]     << 8) | svga->vram[remapped_addr + 1];
                dat[1] = (svga->vram[remapped_addr + 2] << 8) | svga->vram[remapped_addr + 3];
                for (xx = 0; xx < 16; xx++)
                {
                        if (offset >= svga->hwcursor_latch.x)
                        {
                                if (!(dat[0] & 0x8000))
                                   ((uint32_t *)buffer32->line[displine])[offset + 32]  = (dat[1] & 0x8000) ? fg : bg;
                                else if (dat[1] & 0x8000)
                                   ((uint32_t *)buffer32->line[displine])[offset + 32] ^= 0xffffff;
//                                pclog("Plot %i, %i (%i %i) %04X %04X\n", offset, displine, x+xx, svga_hwcursor_on, dat[0], dat[1]);
                        }
                           
                        offset++;
                        dat[0] <<= 1;
                        dat[1] <<= 1;
                }
                svga->hwcursor_latch.addr += 4;
        }
        if (svga->interlace && !svga->hwcursor_oddeven)
                svga->hwcursor_latch.addr += 16;
}


static void s3_io_remove(s3_t *s3)
{
        io_removehandlerx(0x03c0, 0x0020, s3_in, NULL, NULL, s3_out, NULL, NULL, s3);
        
        io_removehandlerx(0x42e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x46e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x4ae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x82e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x86e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x8ae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x8ee8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x92e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x96e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x9ae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0x9ee8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0xa2e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0xa6e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0xaae8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0xaee8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0xb2e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0xb6e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_removehandlerx(0xbae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL, s3);
        io_removehandlerx(0xbee8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL, s3);
        io_removehandlerx(0xe2e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, s3_accel_out_w, s3_accel_out_l,  s3);
}

static void s3_io_set(s3_t *s3)
{
        s3_io_remove(s3);

        io_sethandlerx(0x03c0, 0x0020, s3_in, NULL, NULL, s3_out, NULL, NULL, s3);
        
        io_sethandlerx(0x42e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0x46e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0x4ae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        if (s3->chip == S3_TRIO64)
        {
                io_sethandlerx(0x82e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x86e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x8ae8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x8ee8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x92e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x96e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        }
        else
        {
                io_sethandlerx(0x82e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x86e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x8ae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x8ee8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x92e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
                io_sethandlerx(0x96e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        }
        io_sethandlerx(0x9ae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0x9ee8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xa2e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xa6e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xaae8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xaee8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xb2e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xb6e8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xbae8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xbee8, 0x0002, s3_accel_in, NULL, NULL, s3_accel_out, NULL, NULL,  s3);
        io_sethandlerx(0xe2e8, 0x0004, s3_accel_in, NULL, NULL, s3_accel_out, s3_accel_out_w, s3_accel_out_l,  s3);
}

        
uint8_t s3_pci_read(int func, int addr, void *p)
{
        s3_t *s3 = (s3_t *)p;
        svga_t *svga = &s3->svga;
//        pclog("S3 PCI read %08X\n", addr);
        switch (addr)
        {
                case 0x00: return 0x33; /*'S3'*/
                case 0x01: return 0x53;
                
                case 0x02: return s3->id_ext_pci;
                case 0x03: return 0x88;
                
                case PCI_REG_COMMAND:
                return s3->pci_regs[PCI_REG_COMMAND]; /*Respond to IO and memory accesses*/

                case 0x07: return 1 << 1; /*Medium DEVSEL timing*/
                
                case 0x08: return 0; /*Revision ID*/
                case 0x09: return 0; /*Programming interface*/
                
                case 0x0a: return 0x00; /*Supports VGA interface*/
                case 0x0b: return 0x03;
                
                case 0x10: return 0x00; /*Linear frame buffer address*/
                case 0x11: return 0x00;
                case 0x12: return svga->crtc[0x5a] & 0x80;
                case 0x13: return svga->crtc[0x59];

                case 0x30: return s3->pci_regs[0x30] & 0x01; /*BIOS ROM address*/
                case 0x31: return 0x00;
                case 0x32: return s3->pci_regs[0x32];
                case 0x33: return s3->pci_regs[0x33];
                
                case 0x3c: return s3->int_line;
                case 0x3d: return PCI_INTA;
        }
        return 0;
}

void s3_pci_write(int func, int addr, uint8_t val, void *p)
{
        s3_t *s3 = (s3_t *)p;
        svga_t *svga = &s3->svga;
//        pclog("s3_pci_write: addr=%02x val=%02x\n", addr, val);
        switch (addr)
        {
                case PCI_REG_COMMAND:
                s3->pci_regs[PCI_REG_COMMAND] = val & 0x23;
                if (val & PCI_COMMAND_IO)
                        s3_io_set(s3);
                else
                        s3_io_remove(s3);
                s3_updatemapping(s3);
                break;
                
                case 0x12: 
                svga->crtc[0x5a] = val & 0x80; 
                s3_updatemapping(s3); 
                break;
                case 0x13: 
                svga->crtc[0x59] = val;        
                s3_updatemapping(s3); 
                break;                

                case 0x30: case 0x32: case 0x33:
                s3->pci_regs[addr] = val;
                if (s3->pci_regs[0x30] & 0x01)
                {
                        uint32_t addr = (s3->pci_regs[0x32] << 16) | (s3->pci_regs[0x33] << 24);
//                        pclog("S3 bios_rom enabled at %08x\n", addr);
                        mem_mapping_set_addrx(&s3->bios_rom.mapping, addr, 0x8000);
                }
                else
                {
//                        pclog("S3 bios_rom disabled\n");
                        mem_mapping_disablex(&s3->bios_rom.mapping);
                }
                return;
                
                case 0x3c:
                s3->int_line = val;
                return;
        }
}

static int vram_sizes[] =
{
        7, /*512 kB*/
        6, /*1 MB*/
        4, /*2 MB*/
        0,
        0, /*4 MB*/
        0,
        0,
        0,
        3 /*8 MB*/
};

static void *s3_init(char *bios_fn, int chip)
{
        s3_t *s3 = (s3_t*)malloc(sizeof(s3_t));
        svga_t *svga = &s3->svga;
        int vram;
        uint32_t vram_size;
        
        memset(s3, 0, sizeof(s3_t));
        
        vram = device_get_config_int("memory");
        if (vram)
                vram_size = vram << 20;
        else
                vram_size = 512 << 10;
        s3->vram_mask = vram_size - 1;

        rom_init(&s3->bios_rom, bios_fn, 0xc0000, 0x8000, 0x7fff, 0, MEM_MAPPING_EXTERNAL);
        if (PCI)
                mem_mapping_disablex(&s3->bios_rom.mapping);

        mem_mapping_addx(&s3->linear_mapping, 0,       0,       svga_read_linear, svga_readw_linear, svga_readl_linear, svga_write_linear, svga_writew_linear, svga_writel_linear, NULL, MEM_MAPPING_EXTERNAL, &s3->svga);
        mem_mapping_addx(&s3->mmio_mapping,   0xa0000, 0x10000, s3_accel_read, NULL, NULL, s3_accel_write, s3_accel_write_w, s3_accel_write_l, NULL, MEM_MAPPING_EXTERNAL, s3);
        mem_mapping_disablex(&s3->mmio_mapping);

        svga_init(NULL, &s3->svga, s3, vram_size, /*4mb - 864 supports 8mb but buggy VESA driver reports 0mb*/
                   s3_recalctimings,
                   s3_in, s3_out,
                   s3_hwcursor_draw,
                   NULL);

        svga->decode_mask = (4 << 20) - 1;
        switch (vram)
        {
                case 0: /*512kb*/
                svga->vram_mask = (1 << 19) - 1;
                svga->vram_max = 2 << 20;
                break;
                case 1: /*1MB*/
                /*VRAM in first MB, mirrored in 2nd MB, 3rd and 4th MBs are open bus*/
                /*This works with the #9 9FX BIOS, and matches how my real Trio64 behaves,
                  but does not work with the Phoenix EDO BIOS. Possibly an FPM/EDO difference?*/
                svga->vram_mask = (1 << 20) - 1;
                svga->vram_max = 2 << 20;
                break;
                case 2: default: /*2MB*/
                /*VRAM in first 2 MB, 3rd and 4th MBs are open bus*/
                svga->vram_mask = (2 << 20) - 1;
                svga->vram_max = 2 << 20;
                break;
                case 4: /*4MB*/
                svga->vram_mask = (4 << 20) - 1;
                svga->vram_max = 4 << 20;
                break;
        }
                
        if (PCI)
                svga->crtc[0x36] = 2 | (3 << 2) | (1 << 4) | (vram_sizes[vram] << 5);
        else
                svga->crtc[0x36] = 1 | (3 << 2) | (1 << 4) | (vram_sizes[vram] << 5);
        svga->crtc[0x37] = 1 | (7 << 5);
        
        svga->vsync_callback = s3_vblank_start;

        s3_io_set(s3);

        s3->card = pci_add(s3_pci_read, s3_pci_write, s3);
        
        s3->pci_regs[0x04] = 7;
        
        s3->pci_regs[0x30] = 0x00;
        s3->pci_regs[0x32] = 0x0c;
        s3->pci_regs[0x33] = 0x00;
        
        s3->chip = chip;

        s3->wake_fifo_thread = thread_create_event();
        s3->fifo_not_full_event = thread_create_event();
        s3->fifo_thread = thread_create(fifo_thread, s3);
        
        s3->int_line = 0;
 
        return s3;
}

#ifndef UAE
void *s3_bahamas64_init()
{
        s3_t *s3 = s3_init("bahamas64.bin", S3_VISION864);

        s3->id = 0xc0; /*Vision864P*/
        s3->id_ext = s3->id_ext_pci = 0xc0;
        s3->packed_mmio = 0;
        
        s3->getclock = sdac_getclock;
        s3->getclock_p = &s3->ramdac;
        sdac_init(&s3->ramdac);

        return s3;
}

int s3_bahamas64_available()
{
        return rom_present("bahamas64.bin");
}

void *s3_9fx_init()
{
        s3_t *s3 = s3_init("s3_764.bin", S3_TRIO64);

        s3->id = 0xe1; /*Trio64*/
        s3->id_ext = s3->id_ext_pci = 0x11;
        s3->packed_mmio = 1;

        s3->getclock = s3_trio64_getclock;
        s3->getclock_p = s3;

        return s3;
}

int s3_9fx_available()
{
        return rom_present("s3_764.bin");
}

void *s3_phoenix_trio32_init()
{
        s3_t *s3 = s3_init("86c732p.bin", S3_TRIO32);

        s3->id = 0xe1; /*Trio32*/
        s3->id_ext = 0x10;
        s3->id_ext_pci = 0x11;
        s3->packed_mmio = 1;

        s3->getclock = s3_trio64_getclock;
        s3->getclock_p = s3;

        return s3;
}

int s3_phoenix_trio32_available()
{
        return rom_present("86c732p.bin");
}

void *s3_phoenix_trio64_init()
{
        s3_t *s3 = s3_init("86c764x1.bin", S3_TRIO64);

        s3->id = 0xe1; /*Trio64*/
        s3->id_ext = s3->id_ext_pci = 0x11;
        s3->packed_mmio = 1;
        if (device_get_config_int("memory") == 1)
                s3->svga.vram_max = 1 << 20; /*Phoenix BIOS does not expect VRAM to be mirrored*/

        s3->getclock = s3_trio64_getclock;
        s3->getclock_p = s3;

        return s3;
}

int s3_phoenix_trio64_available()
{
        return rom_present("86c764x1.bin");
}

#endif

void *s3_trio64_init(const device_t *info)
{
    s3_t *s3 = (s3_t*)s3_init("86c764x1.bin", S3_TRIO64);

    s3->id = 0xe1; /*Trio64*/
    s3->id_ext = s3->id_ext_pci = 0x11;
    s3->packed_mmio = 1;

    s3->getclock = s3_trio64_getclock;
    s3->getclock_p = s3;
    s3->svga.fb_only = -1;

    return s3;
}


void *s3_cybervision_init(const device_t *info)
{
    s3_t *s3 = (s3_t*)s3_init("86c764x1.bin", S3_TRIO64);

    s3->id = 0xe1; /*Trio64*/
    s3->packed_mmio = 1;

    s3->getclock = s3_trio64_getclock;
    s3->getclock_p = s3;
    s3->svga.fb_only = -1;

    return s3;
}

void s3_close(void *p)
{
        s3_t *s3 = (s3_t *)p;

        svga_close(&s3->svga);
        
        if (s3->fifo_thread_state) {
            s3->fifo_thread_state = -1;
            wake_fifo_thread(s3);
            while (s3->fifo_thread_state == -1) {
                thread_sleep(1);
            }
            thread_kill(s3->fifo_thread);
        }
        thread_destroy_event(s3->wake_fifo_thread);
        thread_destroy_event(s3->fifo_not_full_event);

        free(s3);
}

void s3_speed_changed(void *p)
{
        s3_t *s3 = (s3_t *)p;
        
        svga_recalctimings(&s3->svga);
}

void s3_force_redraw(void *p)
{
        s3_t *s3 = (s3_t *)p;

        s3->svga.fullchange = changeframecount;
}

void s3_add_status_info(char *s, int max_len, void *p)
{
        s3_t *s3 = (s3_t *)p;
        char temps[256];
        uint64_t new_time = timer_read();
        uint64_t status_diff = new_time - s3->status_time;
        s3->status_time = new_time;

        if (!status_diff)
                status_diff = 1;
        
        svga_add_status_info(s, max_len, &s3->svga);
        sprintf(temps, "%f%% CPU\n%f%% CPU (real)\n\n", ((double)s3->blitter_time * 100.0) / timer_freq, ((double)s3->blitter_time * 100.0) / status_diff);
        strncat(s, temps, max_len);

        s3->blitter_time = 0;
}

#ifndef UAE

static device_config_t s3_bahamas64_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "1 MB",
                                .value = 1
                        },
                        {
                                .description = "2 MB",
                                .value = 2
                        },
                        /*Vision864 also supports 4 and 8 MB, however the Paradise
                          BIOS is buggy (VESA modes don't work correctly), and UNIVBE and
                          OS/2 Warp misdetect the VRAM size. Bahamas 64 only supports
                          up to 2 MB on the board anyway. */
                        {
                                .description = ""
                        }
                },
                .default_int = 4
        },
        {
                .type = -1
        }
};

static device_config_t s3_9fx_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "1 MB",
                                .value = 1
                        },
                        {
                                .description = "2 MB",
                                .value = 2
                        },
                        /*Trio64 also supports 4 MB, however the Number Nine BIOS does not*/
                        {
                                .description = ""
                        }
                },
                .default_int = 2
        },
        {
                .type = -1
        }
};

static device_config_t s3_phoenix_trio32_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "512 KB",
                                .value = 0
                        },
                        {
                                .description = "1 MB",
                                .value = 1
                        },
                        {
                                .description = "2 MB",
                                .value = 2
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 2
        },
        {
                .type = -1
        }
};

static device_config_t s3_phoenix_trio64_config[] =
{
        {
                .name = "memory",
                .description = "Memory size",
                .type = CONFIG_SELECTION,
                .selection =
                {
                        {
                                .description = "1 MB",
                                .value = 1
                        },
                        {
                                .description = "2 MB",
                                .value = 2
                        },
                        {
                                .description = "4 MB",
                                .value = 4
                        },
                        {
                                .description = ""
                        }
                },
                .default_int = 2
        },
        {
                .type = -1
        }
};

device_t s3_bahamas64_device =
{
        "Paradise Bahamas 64 (S3 Vision864)",
        0, 0,
        s3_bahamas64_init,
        s3_close,
        s3_bahamas64_available,
        s3_speed_changed,
        s3_force_redraw,
        s3_add_status_info,
        s3_bahamas64_config
};

device_t s3_9fx_device =
{
        "Number 9 9FX (S3 Trio64)",
        0, 0,
        s3_9fx_init,
        s3_close,
        s3_9fx_available,
        s3_speed_changed,
        s3_force_redraw,
        s3_add_status_info,
        s3_9fx_config
};

device_t s3_phoenix_trio32_device =
{
        "Phoenix S3 Trio32",
        0, 0,
        s3_phoenix_trio32_init,
        s3_close,
        s3_phoenix_trio32_available,
        s3_speed_changed,
        s3_force_redraw,
        s3_add_status_info,
        s3_phoenix_trio32_config
};

device_t s3_phoenix_trio64_device =
{
        "Phoenix S3 Trio64",
        0, 0,
        s3_phoenix_trio64_init,
        s3_close,
        s3_phoenix_trio64_available,
        s3_speed_changed,
        s3_force_redraw,
        s3_add_status_info,
        s3_phoenix_trio64_config
};

#else

device_t s3_cybervision_trio64_device =
{
    "CyberVision64", NULL,
    0, 0,
    s3_cybervision_init,
    s3_close,
    NULL,
    NULL,
    s3_speed_changed,
    s3_force_redraw,
    NULL
};

device_t s3_trio64_device =
{
    "S3Trio64", NULL,
    0, 0,
    s3_trio64_init,
    s3_close,
    NULL,
    NULL,
    s3_speed_changed,
    s3_force_redraw,
    NULL
};

#endif
