/* =====================================================================
   PAPEROS  -  32-bit protected-mode kernel + GUI
   Built with MinGW GCC, no standard library, no downloads, no Linux.
   Features: VGA mode 13h, 256-colour palette, PS/2 mouse + keyboard,
   window manager (drag/minimise/send-behind/close, click to raise), start menu,
   taskbar, clock, and 12 apps: Terminal, Paint, Notepad, Snake, Settings,
   File Manager, Calculator, System Monitor, Minesweeper, 2048, Brick Drop,
   and a real text-only HTTP browser over NE2000.
   ===================================================================== */

typedef unsigned char  u8;
typedef signed char    i8;
typedef unsigned short u16;
typedef signed short   i16;
typedef unsigned int   u32;
typedef signed int     i32;

#include "wallpaper.h"
#include "cursor.h"
#include "boot_images.h"

/* Network entry points (implemented near the end of this file). */
static void net_init(void);
static void net_poll(void);
static void net_cmd_ip(void);
static void net_cmd_ping(const char *arg);
static u32 net_ip, net_gw, net_mask, net_dns;
static int net_up;
static u8 net_mac[6], net_gwmac[6];
static void ip_str(u32 ip, char *out);
static void hex_byte(u8 v, char *out);

/* App entry points used by the window manager and dispatchers. */
static void draw_browser(int cx, int cy, int cw, int ch);
static void web_key(u8 k);
static void web_click(int lx, int ly, int cw, int ch);
static void web_init(void);
static void web_fetch(const char *url, int add_history);
static void ms_reset(void);
static void g2048_reset(void);
static void tet_reset(void);
static void tet_step(void);
static void net_route_udp(const u8 *ip, u16 total);
static void net_route_tcp(const u8 *ip, u16 total);

/* ------------------------------ port I/O ---------------------------- */
static inline void outb(u16 port, u8 val)
{
    __asm__ volatile ("outb %%al, %%dx" : : "a"(val), "d"(port));
}
static inline u8 inb(u16 port)
{
    u8 r;
    __asm__ volatile ("inb %%dx, %%al" : "=a"(r) : "d"(port));
    return r;
}
static inline void outw(u16 port, u16 val)
{
    __asm__ volatile ("outw %%ax, %%dx" : : "a"(val), "d"(port));
}
static inline u16 inw(u16 port)
{
    u16 r;
    __asm__ volatile ("inw %%dx, %%ax" : "=a"(r) : "d"(port));
    return r;
}
static inline void outl(u16 port, u32 val)
{
    __asm__ volatile ("outl %%eax, %%dx" : : "a"(val), "d"(port));
}
static inline u32 inl(u16 port)
{
    u32 r;
    __asm__ volatile ("inl %%dx, %%eax" : "=a"(r) : "d"(port));
    return r;
}

/* Minimal COM1 output, used only to report the selected video mode. */
static void serial_init(void)
{
    outb(0x3F8 + 1, 0);
    outb(0x3F8 + 3, 0x80);
    outb(0x3F8, 1);
    outb(0x3F8 + 3, 3);
    outb(0x3F8 + 2, 0);
}
static void serial_putc(char c)
{
    while (!(inb(0x3F8 + 5) & 0x20)) {}
    outb(0x3F8, (u8)c);
}
static void serial_puts(const char *s)
{
    while (*s) serial_putc(*s++);
}
/* --------------------------- small libc ----------------------------- */
static void memset(void *p, int v, u32 n)
{
    u8 *b = (u8 *)p;
    while (n--) *b++ = (u8)v;
}
static void memcpy(void *d, const void *s, u32 n)
{
    u8 *dst = (u8 *)d;
    const u8 *src = (const u8 *)s;
    while (n--) *dst++ = *src++;
}
static u32 strlen(const char *s)
{
    u32 n = 0;
    while (s[n]) n++;
    return n;
}
static void strcpy(char *d, const char *s)
{
    while ((*d++ = *s++)) {}
}
static int streq(const char *a, const char *b)
{
    while (*a && *b) { if (*a++ != *b++) return 0; }
    return *a == *b;
}
static int memcmp_local(const void *a, const void *b, u32 n)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    while (n--) if (*x++ != *y++) return 1;
    return 0;
}
static void itoa(u32 v, char *out)
{
    char tmp[12];
    int n = 0;
    if (!v) { out[0] = '0'; out[1] = 0; return; }
    while (v) { tmp[n++] = (char)('0' + (v % 10)); v /= 10; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

/* ======================== VIDEO / TIMER ============================= */
#define INFO       ((const u8 *)0x5000) /* filled in by stage-2 bootloader */
#define FONT_PTR   ((const u8 *)0x6000) /* 8x16 VGA glyphs copied by stage 2 */
#define BACK       ((u8 *)0x100000)     /* hidden back buffer, 1 MiB */
#define DESKTOP    ((u8 *)0x1000000)    /* static wallpaper layer, 16 MiB */
#define CANVAS_MEM ((u8 *)0x1A00000)    /* Paint canvas, 26 MiB */

#define TASKBAR_H 18

/* Logical GUI coordinates are native framebuffer pixels. Stage 2 starts a
   known-good 800x600 LFB; apply_requested_mode() may replace it with a
   custom Bochs-VBE mode before any GUI buffer is built. */
static int SW = 800, SH = 600, TASKBAR_Y = 582;
static volatile u8 *FB = (volatile u8 *)0xFD000000;
static u16 VW = 800, VH = 600, VPITCH = 800;
static u8 fb_bpp = 8;
static u16 req_mode_w = 800, req_mode_h = 600;
static int req_mode_valid = 0;

static void fast_copy(void *d, const void *s, u32 n)
{
    __asm__ volatile ("cld; rep movsb" : : "D"(d), "S"(s), "c"(n) : "memory");
}

/* The bootloader writes: width, height, pitch, bpp, framebuffer address,
   VBE mode, VBE flag, font pointer, requested width/height and requested-mode
   flag into the info block at 0x5000. */
static void read_video_info(void)
{
    const u16 *w16 = (const u16 *)INFO;
    const u32 *w32 = (const u32 *)INFO;
    VW = w16[0];
    VH = w16[1];
    VPITCH = w16[2];
    fb_bpp = INFO[6];
    FB = (volatile u8 *)w32[2];        /* offset 8 */
    req_mode_w = w16[10];              /* offset 0x14 */
    req_mode_h = w16[11];              /* offset 0x16 */
    req_mode_valid = (w16[12] != 0 && w16[7] != 0);

    SW = VW;
    SH = VH;
    TASKBAR_Y = SH - TASKBAR_H;
    if (TASKBAR_Y < 64) TASKBAR_Y = SH - 1;
    serial_init();
}

/* -------------------- Bochs VBE DISPI custom modes ------------------ */
/* QEMU's standard VGA implements the Bochs VBE extension registers. Stage 2
   gives us the LFB address through VBE mode 0x103; these registers change the
   scanout geometry. Every write is read back before it is accepted. */
#define VBE_DISPI_INDEX     0x1CE
#define VBE_DISPI_DATA      0x1CF
#define DISPI_ID            0x00
#define DISPI_XRES          0x01
#define DISPI_YRES          0x02
#define DISPI_BPP           0x03
#define DISPI_ENABLE        0x04
#define DISPI_VIRT_WIDTH    0x06
#define DISPI_VIRT_HEIGHT   0x07
#define DISPI_X_OFFSET      0x08
#define DISPI_Y_OFFSET      0x09
#define DISPI_ENABLED       0x01
#define DISPI_LFB_ENABLED   0x40
#define DISPI_NOCLEARMEM    0x80

static void dispi_write(u16 index, u16 value)
{
    outw(VBE_DISPI_INDEX, index);
    outw(VBE_DISPI_DATA, value);
}
static u16 dispi_read(u16 index)
{
    outw(VBE_DISPI_INDEX, index);
    return inw(VBE_DISPI_DATA);
}

typedef struct { u16 w, h; } VidMode;
/* Ordered by pixel count so a failed mode falls back to a smaller safe mode,
   then to a larger one if no smaller mode is accepted. Widths are 16-pixel
   aligned because QEMU's Bochs VBE rejects unaligned XRES values. */
static const VidMode vid_modes[] = {
    {3840, 2160}, {2560, 1440}, {1920, 1080}, {1600, 1200},
    {1280, 1024}, {1360,  768}, {1280,  720}, {1024,  768},
    { 800,  600}, { 640,  480}, { 640,  400}
};
#define VID_MODE_COUNT ((int)(sizeof(vid_modes) / sizeof(vid_modes[0])))

static int find_vid_mode(u16 w, u16 h)
{
    if ((w == 1366 || w == 1368) && h == 768) w = 1360;
    for (int i = 0; i < VID_MODE_COUNT; i++)
        if (vid_modes[i].w == w && vid_modes[i].h == h) return i;
    return -1;
}

static int dispi_set_mode(u16 w, u16 h)
{
    u16 id = dispi_read(DISPI_ID);
    if (id < 0xB0C0 || id > 0xB0CF) return 0;   /* not a Bochs-style VBE */

    dispi_write(DISPI_ENABLE, 0);               /* must be off to reprogram */
    dispi_write(DISPI_XRES, w);
    dispi_write(DISPI_YRES, h);
    dispi_write(DISPI_BPP, 8);
    dispi_write(DISPI_VIRT_WIDTH, w);           /* 8 bpp: pitch == width */
    dispi_write(DISPI_VIRT_HEIGHT, h);
    dispi_write(DISPI_X_OFFSET, 0);
    dispi_write(DISPI_Y_OFFSET, 0);
    dispi_write(DISPI_ENABLE, DISPI_ENABLED | DISPI_LFB_ENABLED |
                              DISPI_NOCLEARMEM);

    u16 enable = dispi_read(DISPI_ENABLE);
    if ((enable & (DISPI_ENABLED | DISPI_LFB_ENABLED)) !=
        (DISPI_ENABLED | DISPI_LFB_ENABLED)) return 0;
    if (dispi_read(DISPI_XRES) != w || dispi_read(DISPI_YRES) != h ||
        dispi_read(DISPI_BPP) != 8) return 0;

    u16 pitch = dispi_read(DISPI_VIRT_WIDTH);
    if (pitch < w) pitch = w;                   /* 8 bpp, no planar modes */
    VW = w;
    VH = h;
    VPITCH = pitch;
    SW = VW;
    SH = VH;
    TASKBAR_Y = SH - TASKBAR_H;
    if (TASKBAR_Y < 64) TASKBAR_Y = SH - 1;
    return 1;
}

static void report_video(void)
{
    char msg[96];
    int n = 0;
    const char *a = "PAPEROS VIDEO ";
    while (*a) msg[n++] = *a++;
    itoa(VW, msg + n); n += strlen(msg + n);
    msg[n++] = 'x';
    itoa(VH, msg + n); n += strlen(msg + n);
    a = " PITCH ";
    while (*a) msg[n++] = *a++;
    itoa(VPITCH, msg + n); n += strlen(msg + n);
    a = " NATIVE\n";
    while (*a) msg[n++] = *a++;
    msg[n] = 0;
    serial_puts(msg);
}

static void apply_requested_mode(void)
{
    if (req_mode_valid && fb_bpp == 8) {
        u16 w = req_mode_w, h = req_mode_h;
        int start = find_vid_mode(w, h);
        int applied = 0;

        if (start >= 0 && dispi_set_mode(w, h)) applied = 1;
        if (!applied && start >= 0) {
            for (int i = start + 1; i < VID_MODE_COUNT && !applied; i++)
                applied = dispi_set_mode(vid_modes[i].w, vid_modes[i].h);
            for (int i = start - 1; i >= 0 && !applied; i--)
                applied = dispi_set_mode(vid_modes[i].w, vid_modes[i].h);
        }
    }
    report_video();
}

/* -------------------------- PIT timer driver ------------------------ */
/* Channel 2 is free (the PC speaker is not used). Free-running count down
   from 65535 gives a monotonic millisecond clock without BIOS calls. */
#define PIT_CTRL 0x43
#define PIT_CH2  0x42
static u32 pit_base = 0;
static u16 pit_prev = 0;

static void pit_init(void)
{
    outb(PIT_CTRL, 0xB8);              /* ch2, lo/hi, mode 3, binary */
    outb(PIT_CH2, 0);
    outb(PIT_CH2, 0);                  /* reload = 65536 */
    pit_base = 0;
    pit_prev = 0;
}
static u16 pit_count(void)
{
    outb(PIT_CTRL, 0x80);              /* latch channel 2 */
    u8 lo = inb(PIT_CH2);
    u8 hi = inb(PIT_CH2);
    return (u16)((hi << 8) | lo);
}
static u32 pit_ms(void)
{
    u16 c = pit_count();
    if (c > pit_prev) pit_base += 65536;
    pit_prev = c;
    return (pit_base + 65535u - c) * 1000u / 1193182u;
}
static void pit_sleep_ms(u32 ms)
{
    u32 t0 = pit_ms();
    while (pit_ms() - t0 < ms) {}
}

static u8 palette[768];

static void init_palette(void)
{
    static const u8 base[16][3] = {
        {0,0,0},   {0,0,42},  {0,42,0},  {0,42,42},
        {42,0,0},  {42,0,42}, {42,21,0}, {42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},
        {63,21,21},{63,21,63},{63,63,0}, {63,63,63}
    };
    int i, j = 0;
    for (i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++) palette[j++] = base[i][k];
    for (int r = 0; r < 6; r++)
        for (int g = 0; g < 6; g++)
            for (int b = 0; b < 6; b++) {
                palette[j++] = (u8)(r * 12);
                palette[j++] = (u8)(g * 12);
                palette[j++] = (u8)(b * 12);
            }
    for (i = 0; i < 24; i++) {
        u8 v = (u8)(i * 2 + 15);
        palette[j++] = v; palette[j++] = v; palette[j++] = v;
    }
    outb(0x3C6, 0xFF);                 /* DAC mask: use all 6 colour bits */
    outb(0x3C8, 0);
    for (i = 0; i < 768; i++) outb(0x3C9, palette[i]);
}

#define RGB(r,g,b) (u8)(16 + (r)*36 + (g)*6 + (b))
#define C_BLACK   0
#define C_WHITE   15
#define C_LGRAY   7
#define C_DGRAY   8
#define C_BLUE    1
#define C_LBLUE   9
#define C_RED     4
#define C_LRED    12
#define C_GREEN   2
#define C_LGREEN  10
#define C_YELLOW  14
#define C_CYAN    3
#define C_MAGENTA 5
#define C_FACE    RGB(4,4,4)           /* raised button face */
#define C_FACE2   RGB(3,3,3)           /* recessed face */
#define C_SHADOW  RGB(1,1,1)           /* lower/right bevel */
#define C_HILITE  RGB(5,5,5)           /* upper/left bevel */

/* Drawing always targets draw_buf (BACK, or DESKTOP while building the
   wallpaper layer). A clip rectangle keeps every string and widget inside
   its window; fills are clipped once per row and use rep stosb, so redraw
   cost follows the damaged area instead of the whole window. */
static u8 *draw_buf = BACK;
static int clip_x0 = 0, clip_y0 = 0, clip_x1 = 800, clip_y1 = 600;

static inline void set_clip(int x0, int y0, int x1, int y1)
{
    clip_x0 = x0; clip_y0 = y0; clip_x1 = x1; clip_y1 = y1;
}
static inline void clip_full(void) { set_clip(0, 0, SW, SH); }
static inline void restrict_clip(int x0, int y0, int x1, int y1)
{
    if (x0 > clip_x0) clip_x0 = x0;
    if (y0 > clip_y0) clip_y0 = y0;
    if (x1 < clip_x1) clip_x1 = x1;
    if (y1 < clip_y1) clip_y1 = y1;
    if (clip_x1 < clip_x0) clip_x1 = clip_x0;
    if (clip_y1 < clip_y0) clip_y1 = clip_y0;
}

static inline void putp(int x, int y, u8 c)
{
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1) return;
    if ((u32)x >= (u32)SW || (u32)y >= (u32)SH) return;
    draw_buf[y * SW + x] = c;
}

static void fillrect(int x, int y, int w, int h, u8 c)
{
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    if (x0 < clip_x0) x0 = clip_x0;
    if (y0 < clip_y0) y0 = clip_y0;
    if (x1 > clip_x1) x1 = clip_x1;
    if (y1 > clip_y1) y1 = clip_y1;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SW) x1 = SW;
    if (y1 > SH) y1 = SH;
    int fw = x1 - x0, fh = y1 - y0;
    if (fw <= 0 || fh <= 0) return;
    for (int yy = y0; yy < y1; yy++) {
        u8 *dst = draw_buf + yy * SW + x0;
        __asm__ volatile ("cld; rep stosb"
                          :
                          : "D"(dst), "a"(c), "c"((u32)fw)
                          : "memory", "cc");
    }
}
static void hline(int x, int y, int w, u8 c) { fillrect(x, y, w, 1, c); }
static void vline(int x, int y, int h, u8 c) { fillrect(x, y, 1, h, c); }
static void frame(int x, int y, int w, int h, u8 c)
{
    hline(x, y, w, c); hline(x, y + h - 1, w, c);
    vline(x, y, h, c); vline(x + w - 1, y, h, c);
}

/* --------------------------- damage tracking ------------------------ */
/* Damage is a list of small rectangles, not one bounding box. A cursor move
   touches two 16x16 areas even if it crosses the whole 4K desktop. Touching
   rectangles are joined; if the list is ever exhausted, the safe fallback is
   one full-screen rectangle. */
#define MAX_DAMAGE 192
typedef struct { int x0, y0, x1, y1; } DmgRect;
static DmgRect dmg[MAX_DAMAGE];
static int dmg_n = 0;

static int damage_touch(const DmgRect *a, const DmgRect *b)
{
    return a->x0 <= b->x1 + 1 && b->x0 <= a->x1 + 1 &&
           a->y0 <= b->y1 + 1 && b->y0 <= a->y1 + 1;
}
static void damage_all(void)
{
    dmg_n = 1;
    dmg[0].x0 = 0; dmg[0].y0 = 0;
    dmg[0].x1 = SW; dmg[0].y1 = SH;
}
static void mark_rect(int x, int y, int w, int h)
{
    DmgRect r;
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    r.x0 = x; r.y0 = y;
    r.x1 = x + w; r.y1 = y + h;
    if (r.x1 > SW) r.x1 = SW;
    if (r.y1 > SH) r.y1 = SH;
    if (r.x0 >= r.x1 || r.y0 >= r.y1) return;

    int merged;
    do {
        merged = 0;
        for (int i = 0; i < dmg_n; i++) {
            if (!damage_touch(&dmg[i], &r)) continue;
            if (dmg[i].x0 < r.x0) r.x0 = dmg[i].x0;
            if (dmg[i].y0 < r.y0) r.y0 = dmg[i].y0;
            if (dmg[i].x1 > r.x1) r.x1 = dmg[i].x1;
            if (dmg[i].y1 > r.y1) r.y1 = dmg[i].y1;
            dmg[i] = dmg[--dmg_n];       /* remove the absorbed rectangle */
            merged = 1;
            break;
        }
    } while (merged);

    if (dmg_n < MAX_DAMAGE) dmg[dmg_n++] = r;
    else damage_all();
}
static int damage_empty(void) { return dmg_n == 0; }

static int rect_hit(const DmgRect *r, int x, int y, int w, int h)
{
    if (w <= 0 || h <= 0) return 0;
    return r->x0 < x + w && x < r->x1 && r->y0 < y + h && y < r->y1;
}

/* Wait for vertical retrace once per frame, then publish every dirty rect. */
static void present_frame(void)
{
    u32 t0 = pit_ms();
    while (!(inb(0x3DA) & 8) && pit_ms() - t0 < 20) {}
    for (int i = 0; i < dmg_n; i++) {
        const DmgRect *r = &dmg[i];
        int w = r->x1 - r->x0;
        if (w <= 0 || r->y1 <= r->y0) continue;
        for (int y = r->y0; y < r->y1; y++) {
            __asm__ volatile ("cld; rep movsb"
                              :
                              : "D"((void *)(FB + y * VPITCH + r->x0)),
                                "S"(BACK + y * SW + r->x0), "c"((u32)w)
                              : "memory");
        }
    }
}

/* ------------------------------- font ------------------------------- */
/* Stage 2 copies the BIOS 8x16 font to 0x6000 before switching video modes,
   so glyphs stay readable at native 640/800/1024-pixel widths. */
#define FONT_W 8
#define FONT_H 16

static void init_font(void)
{
    /* Nothing to copy at runtime: FONT_PTR is already populated. */
}

static void draw_char(int x, int y, char ch, u8 fg, u8 bg)
{
    const u8 *g = FONT_PTR + ((u8)ch * FONT_H);
    for (int row = 0; row < FONT_H; row++) {
        u8 bits = g[row];
        for (int col = 0; col < FONT_W; col++)
            putp(x + col, y + row, (bits & (0x80 >> col)) ? fg : bg);
    }
}
static void draw_text_n(int x, int y, const char *s, int max_chars, u8 fg, u8 bg)
{
    int i = 0;
    while (s[i] && i < max_chars) {
        draw_char(x, y, s[i], fg, bg);
        x += FONT_W;
        i++;
    }
}
static void draw_text(int x, int y, const char *s, u8 fg, u8 bg)
{
    draw_text_n(x, y, s, 0x7FFFFFFF, fg, bg);
}
static void draw_text_s(int x, int y, const char *s, int scale, u8 fg, u8 bg)
{
    while (*s) {
        const u8 *g = FONT_PTR + ((u8)*s++ * FONT_H);
        for (int row = 0; row < FONT_H; row++)
            for (int col = 0; col < FONT_W; col++) {
                u8 c = (g[row] & (0x80 >> col)) ? fg : bg;
                fillrect(x + col * scale, y + row * scale, scale, scale, c);
            }
        x += FONT_W * scale;
    }
}

/* ---------------------------- mouse art ----------------------------- */
/* Cursor art generated from cursor.png by make_cursor.py. It uses the kernel's
   native 8-bpp palette, so no extra DAC entries are needed. */
static int cursor_scale(void)
{
    return SW >= 2000 ? 2 : 1;
}
static void draw_cursor(int x, int y)
{
    int s = cursor_scale();
    int stride = (CUR_W + 7) >> 3;
    for (int row = 0; row < CUR_H; row++)
        for (int col = 0; col < CUR_W; col++) {
            u8 m = cursor_mask[row * stride + (col >> 3)] &
                   (0x80 >> (col & 7));
            if (!m) continue;
            u8 c = cursor_pixels[row * CUR_W + col];
            fillrect(x + col * s, y + row * s, s, s, c);
        }
}

/* =========================== INPUT ================================== */
#define K_ESC   1
#define K_UP    2
#define K_DOWN  3
#define K_LEFT  4
#define K_RIGHT 5
#define K_ENTER 6
#define K_BACK  7
#define K_TAB   8
#define K_DEL   9

static const u8 scmap[128] = {
/*00*/ 0, K_ESC,'1','2','3','4','5','6','7','8','9','0','-','=',K_BACK,K_TAB,
/*10*/ 'q','w','e','r','t','y','u','i','o','p','[',']',K_ENTER,0,'a','s',
/*20*/ 'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
/*30*/ 'b','n','m',',','.','/',0,0,0,' ',0,0,0,0,0,0,
/*40*/ 0,0,0,0,0,0,0,0,K_UP,0,0,K_LEFT,0,K_RIGHT,0,0,
/*50*/ 0,0,0,0,K_DOWN,0,0,0,0,0,0,0,0,K_DEL,0,0,
/*60*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/*70*/ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0
};

#define QSIZE 32
static u8 keyq[QSIZE];
static int qhead = 0, qtail = 0;
static void key_push(u8 k)
{
    int n = (qhead + 1) % QSIZE;
    if (n != qtail) { keyq[qhead] = k; qhead = n; }
}
static u8 key_pop(void)
{
    if (qhead == qtail) return 0;
    u8 k = keyq[qtail];
    qtail = (qtail + 1) % QSIZE;
    return k;
}
static int shift_down = 0;

/* Shifted symbols are not in scmap. Map them so Calculator's + and * keys and
   filenames with punctuation work from the keyboard as well as from buttons. */
static u8 shifted_key(u8 sc, u8 base)
{
    if (base >= 'a' && base <= 'z') return (u8)(base - 32);
    switch (sc) {
        case 0x02: return '!';
        case 0x03: return '@';
        case 0x04: return '#';
        case 0x05: return '$';
        case 0x06: return '%';
        case 0x07: return '^';
        case 0x08: return '&';
        case 0x09: return '*';
        case 0x0A: return '(';
        case 0x0B: return ')';
        case 0x0C: return '_';
        case 0x0D: return '+';
        case 0x1A: return '{';
        case 0x1B: return '}';
        case 0x27: return ':';
        case 0x28: return '"';
        case 0x29: return '~';
        case 0x2B: return '|';
        case 0x33: return '<';
        case 0x34: return '>';
        case 0x35: return '?';
        default:  return base;
    }
}

static void poll_keyboard(void)
{
    u32 guard = 0;
    while (guard++ < 64) {
        u8 st = inb(0x64);
        if (!(st & 1)) break;          /* no output byte waiting */
        if (st & 0x20) break;          /* it is MOUSE data: leave it for poll_mouse */
        u8 sc = inb(0x60);
        if (sc & 0x80) {
            if (sc == 0xAA || sc == 0xB6) shift_down = 0;
            continue;
        }
        if (sc == 0x2A || sc == 0x36) { shift_down = 1; continue; }
        u8 sc7 = sc & 0x7F;
        u8 c = scmap[sc7];
        if (shift_down) c = shifted_key(sc7, c);
        if (c) key_push(c);
    }
}

static int mouse_x = 0, mouse_y = 0, mouse_btns = 0, mouse_click = 0;
static u8 mpacket[3];
static int midx = 0;

static void mouse_wait_in(void)  { u32 t = 0; while ((inb(0x64) & 2) && t++ < 100000) {} }
static void mouse_wait_out(void) { u32 t = 0; while (!(inb(0x64) & 1) && t++ < 100000) {} }
static void mouse_write(u8 v)
{
    mouse_wait_in(); outb(0x64, 0xD4);
    mouse_wait_in(); outb(0x60, v);
    mouse_wait_out(); (void)inb(0x60);
}
static void mouse_init(void)
{
    u8 cfg;
    outb(0x64, 0xA8);                 /* enable auxiliary device */
    mouse_wait_in(); outb(0x64, 0x20);
    mouse_wait_out(); cfg = inb(0x60);
    cfg |= 2;
    mouse_wait_in(); outb(0x64, 0x60);
    mouse_wait_in(); outb(0x60, cfg);
    mouse_write(0xF6);                /* load defaults */
    mouse_write(0xF3); mouse_write(100); /* 100 reports/sec */
    mouse_write(0xF4);                /* streaming on */
    /* Throw away any stale bytes left in the 8042 output buffer. */
    while (inb(0x64) & 1) (void)inb(0x60);
}
static void poll_mouse(void)
{
    /* Drain every queued byte in one call. Processing one byte per main-loop
       iteration made each mouse movement wait for its own video retrace. */
    u32 guard = 0;
    while (guard++ < 64) {
        u8 st = inb(0x64);
        if ((st & 0x21) != 0x21) break; /* output-full AND auxiliary source */
        u8 d = inb(0x60);

        if (midx == 0 && !(d & 0x08)) continue; /* resync to packet start */

        mpacket[midx++] = d;
        if (midx >= 3) {
            midx = 0;
            if (!(mouse_btns & 1) && (mpacket[0] & 1)) mouse_click = 1;
            mouse_btns = mpacket[0] & 7;
            mouse_x += (i8)mpacket[1];       /* logical == physical pixels */
            mouse_y -= (i8)mpacket[2];       /* PS/2 +Y is up */
            if (mouse_x < 0) mouse_x = 0;
            if (mouse_x >= SW) mouse_x = SW - 1;
            if (mouse_y < 0) mouse_y = 0;
            if (mouse_y >= SH) mouse_y = SH - 1;
        }
    }
}

/* ----------------------------- clock -------------------------------- */
static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
static u8 bcd(u8 v) { return (u8)((v >> 4) * 10 + (v & 15)); }
static void draw_clock(void)
{
    char s[6];
    u8 h = bcd(cmos(0x04)), m = bcd(cmos(0x02));
    s[0] = (char)('0' + h / 10); s[1] = (char)('0' + h % 10);
    s[2] = ':';
    s[3] = (char)('0' + m / 10); s[4] = (char)('0' + m % 10);
    s[5] = 0;
    int x = SW - 58, y = TASKBAR_Y + 1;
    fillrect(x - 2, y - 1, 52, 16, C_FACE);
    frame(x - 2, y - 1, 52, 16, C_SHADOW);
    draw_text(x, y, s, C_BLACK, C_FACE);
}

/* ====================== WINDOW MANAGER ============================== */
#define MAXW 12
typedef struct {
    int used, type, x, y, w, h, min;
    char title[28];
} Win;
static Win wins[MAXW];
static int nwin = 0, focus = -1;
static int dragging = 0, drag_win = -1, drag_dx = 0, drag_dy = 0;

#define WIN_BORDER 3
#define TITLE_H    24
#define BTN_W      20
#define BTN_H      16
#define CONTENT_X(w) ((w)->x + WIN_BORDER)
#define CONTENT_Y(w) ((w)->y + TITLE_H)
#define CONTENT_W(w) ((w)->w - WIN_BORDER * 2)
#define CONTENT_H(w) ((w)->h - TITLE_H - WIN_BORDER * 2)

static u8 wall_color = RGB(0,0,3);
static u8 accent = C_BLUE;
static int show_wallpaper = 1;
static int flat_ui = 0;
static int settings_tab = 0;

/* Damage helpers for whole windows / their client areas. */
static void mark_window(int i)
{
    if (i < 0 || i >= nwin || !wins[i].used || wins[i].min) return;
    mark_rect(wins[i].x - 3, wins[i].y - 3, wins[i].w + 6, wins[i].h + 6);
}
static void mark_window_at(int i, int x, int y)
{
    if (i < 0 || i >= nwin || !wins[i].used) return;
    mark_rect(x - 3, y - 3, wins[i].w + 6, wins[i].h + 6);
}
static void mark_content(int i)
{
    if (i < 0 || i >= nwin || !wins[i].used || wins[i].min) return;
    Win *w = &wins[i];
    mark_rect(CONTENT_X(w), CONTENT_Y(w), CONTENT_W(w), CONTENT_H(w));
}
static void title_buttons(Win *w, int *minx, int *behindx, int *closex)
{
    *closex = w->x + w->w - 6 - BTN_W;
    *behindx = *closex - 4 - BTN_W;
    *minx = *behindx - 4 - BTN_W;
}

/* ---------------------- app forward declarations -------------------- */
static void snake_reset(void);
static void draw_content(int i, int cx, int cy, int cw, int ch);
static void app_key(int i, u8 k);
static void app_click(int i, int lx, int ly, int cw, int ch);
static void app_mouse(int i, int cx, int cy, int cw, int ch);
static char lab_char(int type);
static int strncmp_local(const char *a, const char *b);
static int atoi_local(const char *s);

static int find_win(int type)
{
    for (int i = 0; i < nwin; i++)
        if (wins[i].used && wins[i].type == type) return i;
    return -1;
}
static int win_width(int type)
{
    static const int pct[12] = { 45, 40, 50, 32, 42, 48, 34, 46, 42, 46, 54, 58 };
    static const int minw[12] = { 520, 460, 560, 400, 500, 520, 300, 500, 460, 460, 520, 540 };
    static const int maxw[12] = {1400,1200,1500,1000,1300,1400, 700,1300,1100,1000,1200,1500 };
    int w = SW * pct[type] / 100;
    if (w < minw[type]) w = minw[type];
    if (w > maxw[type]) w = maxw[type];
    int max = SW - 24;
    if (max < 160) max = 160;
    if (w > max) w = max;
    return w;
}
static int win_height(int type)
{
    static const int pct[12] = { 70, 58, 64, 58, 50, 66, 62, 60, 56, 58, 68, 66 };
    static const int minh[12] = { 420, 340, 380, 340, 300, 380, 300, 340, 340, 340, 420, 400 };
    static const int maxh[12] = {1000, 900,1000, 900, 700,1000, 650, 800, 800, 800, 900,1100 };
    int h = TASKBAR_Y * pct[type] / 100;
    if (h < minh[type]) h = minh[type];
    if (h > maxh[type]) h = maxh[type];
    int max = TASKBAR_Y - 24;
    if (max < 120) max = 120;
    if (h > max) h = max;
    return h;
}

/* Move a window inside the z-order array. Index 0 is the bottom window and
   nwin-1 is the topmost one; focus and drag indices must follow the slot. */
static void move_win(int from, int to)
{
    if (from < 0 || from >= nwin || to < 0 || to >= nwin || from == to) return;
    Win tmp = wins[from];
    if (from < to) {
        for (int j = from; j < to; j++) wins[j] = wins[j + 1];
    } else {
        for (int j = from; j > to; j--) wins[j] = wins[j - 1];
    }
    wins[to] = tmp;
    if (focus == from) focus = to;
    else if (from < focus && focus <= to) focus--;
    else if (to < focus && focus < from) focus++;
    if (drag_win == from) drag_win = to;
    else if (from < drag_win && drag_win <= to) drag_win--;
    else if (to < drag_win && drag_win < from) drag_win++;
}

static void raise_win(int i)
{
    if (i < 0 || i >= nwin) return;
    if (i != nwin - 1) move_win(i, nwin - 1);
    focus = nwin - 1;
    damage_all();
}

static void lower_win(int i)
{
    if (i < 0 || i >= nwin) return;
    int newfocus = (focus == i) ? nwin - 1 : focus;
    if (i != 0) move_win(i, 0);
    focus = newfocus;
    if (focus >= nwin) focus = nwin - 1;
    damage_all();
}

static void open_win(int type)
{
    int i = find_win(type);
    if (i >= 0) { wins[i].min = 0; raise_win(i); return; }
    if (nwin >= MAXW) return;
    static const char *tt[12] = {
        "TERMINAL", "PAINT", "NOTEPAD", "SNAKE", "SETTINGS",
        "FILE MANAGER", "CALCULATOR", "SYSTEM MONITOR",
        "MINESWEEPER", "2048", "BRICK DROP", "PAPEROS WEB"
    };
    Win *w = &wins[nwin];
    memset(w, 0, sizeof(Win));
    w->used = 1; w->type = type;
    w->w = win_width(type); w->h = win_height(type);
    int xstep = SW / 64, ystep = TASKBAR_Y / 40;
    if (xstep < 20) xstep = 20;
    if (xstep > 64) xstep = 64;
    if (ystep < 14) ystep = 14;
    if (ystep > 44) ystep = 44;
    w->x = 20 + (type % 6) * xstep;
    w->y = 14 + (type / 6) * ystep;
    if (w->x + w->w > SW - 2) w->x = SW - w->w - 2;
    if (w->y + w->h > TASKBAR_Y - 2) w->y = TASKBAR_Y - w->h - 2;
    if (w->x < 0) w->x = 0;
    if (w->y < 0) w->y = 0;
    strcpy(w->title, tt[type]);
    nwin++;
    if (type == 3) snake_reset();
    if (type == 8) ms_reset();
    if (type == 9) g2048_reset();
    if (type == 10) tet_reset();
    if (type == 11) web_init();
    focus = nwin - 1;
    damage_all();
}
static void close_win(int i)
{
    if (i < 0 || i >= nwin) return;
    mark_window(i);
    for (int j = i; j < nwin - 1; j++) wins[j] = wins[j + 1];
    nwin--;
    memset(&wins[nwin], 0, sizeof(Win));
    if (focus == i) focus = nwin - 1;
    else if (focus > i) focus--;
    if (drag_win == i) { dragging = 0; drag_win = -1; }
    else if (drag_win > i) drag_win--;
    if (focus >= nwin) focus = nwin - 1;
    damage_all();
}

/* Raised/recessed 3-D edge used by buttons and the taskbar. */
static void draw_bevel(int x, int y, int w, int h, int pressed)
{
    if (flat_ui) {
        fillrect(x, y, w, h, C_FACE);
        frame(x, y, w, h, pressed ? C_HILITE : C_SHADOW);
        return;
    }
    u8 hi = pressed ? C_SHADOW : C_HILITE;
    u8 lo = pressed ? C_HILITE : C_SHADOW;
    fillrect(x, y, w, h, C_FACE);
    hline(x, y, w, hi); vline(x, y, h, hi);
    hline(x, y + h - 1, w, lo); vline(x + w - 1, y, h, lo);
}

static void draw_win(int i)
{
    Win *w = &wins[i];
    if (!w->used || w->min) return;
    u8 tb = (i == focus) ? accent : C_FACE2;
    int sx0 = clip_x0, sy0 = clip_y0, sx1 = clip_x1, sy1 = clip_y1;

    fillrect(w->x + 3, w->y + 3, w->w, w->h, C_BLACK);
    fillrect(w->x, w->y, w->w, w->h, C_FACE);
    hline(w->x, w->y, w->w, C_HILITE);
    vline(w->x, w->y, w->h, C_HILITE);
    hline(w->x, w->y + w->h - 1, w->w, C_SHADOW);
    vline(w->x + w->w - 1, w->y, w->h, C_SHADOW);
    frame(w->x + 1, w->y + 1, w->w - 2, w->h - 2, C_BLACK);

    fillrect(w->x + 2, w->y + 2, w->w - 4, TITLE_H - 2, tb);
    hline(w->x + 2, w->y + 2, w->w - 4, i == focus ? C_HILITE : C_WHITE);
    hline(w->x + 2, w->y + TITLE_H - 2, w->w - 4, C_SHADOW);
    fillrect(w->x + 5, w->y + 6, 9, 11, i == focus ? C_WHITE : C_LGRAY);
    frame(w->x + 5, w->y + 6, 9, 11, C_BLACK);
    fillrect(w->x + 7, w->y + 8, 5, 7, tb);

    restrict_clip(w->x + 18, w->y + 3, w->x + w->w - 78, w->y + TITLE_H - 1);
    draw_text_n(w->x + 19, w->y + 5, w->title,
                (w->w - 92) / FONT_W,
                (i == focus) ? C_WHITE : C_BLACK, tb);
    clip_x0 = sx0; clip_y0 = sy0; clip_x1 = sx1; clip_y1 = sy1;

    int minx, behindx, closex;
    title_buttons(w, &minx, &behindx, &closex);
    draw_bevel(minx, w->y + 4, BTN_W, BTN_H, 0);
    draw_text(minx + 6, w->y + 4, "_", C_BLACK, C_FACE);
    draw_bevel(behindx, w->y + 4, BTN_W, BTN_H, 0);
    draw_text(behindx + 5, w->y + 4, "v", C_BLACK, C_FACE);
    draw_bevel(closex, w->y + 4, BTN_W, BTN_H, 0);
    draw_text(closex + 5, w->y + 4, "x", C_RED, C_FACE);

    int cx = CONTENT_X(w), cy = CONTENT_Y(w);
    int cw = CONTENT_W(w), ch = CONTENT_H(w);
    restrict_clip(cx, cy, cx + cw, cy + ch);
    draw_content(i, cx, cy, cw, ch);
    clip_x0 = sx0; clip_y0 = sy0; clip_x1 = sx1; clip_y1 = sy1;
}

/* The generated wallpaper is 1024x752 RLE pairs. Tile it across the whole
   desktop so 1080p/1440p/4K do not show a small cropped corner image. */
static void draw_wallpaper(void)
{
    u32 src = 0;
    int x = 0, y = 0;
    int limit_y = TASKBAR_Y;
    while (src + 1 < WALL_RLE_LEN && y < WALL_H) {
        u8 count = wall_rle[src++];
        u8 color = wall_rle[src++];
        while (count--) {
            for (int oy = 0; oy < limit_y; oy += WALL_H) {
                int ty = oy + y;
                if (ty >= limit_y) continue;
                for (int ox = 0; ox < SW; ox += WALL_W) {
                    int tx = ox + x;
                    if (tx < SW) putp(tx, ty, color);
                }
            }
            if (++x >= WALL_W) {
                x = 0; y++;
                if (y >= WALL_H) break;
            }
        }
    }
}

static int ui_scale(void)
{
    if (SW >= 3200) return 4;
    if (SW >= 2000) return 3;
    if (SW >= 1200) return 2;
    return 1;
}

/* Build the static wallpaper layer once. Windows and the taskbar are drawn
   on top of a copy of this layer, so damage redraws stay flicker-free. */
static void build_desktop(void)
{
    u8 *old_buf = draw_buf;
    int ox0 = clip_x0, oy0 = clip_y0, ox1 = clip_x1, oy1 = clip_y1;
    draw_buf = DESKTOP;
    clip_full();

    memset(DESKTOP, wall_color, (u32)SW * SH);
    if (show_wallpaper) draw_wallpaper();

    char res[48];
    int n = 0;
    const char *label = "PAPEROS 0.9 - NATIVE ";
    while (*label) res[n++] = *label++;
    itoa(VW, res + n); n += strlen(res + n);
    res[n++] = 'x';
    itoa(VH, res + n); n += strlen(res + n);
    res[n] = 0;

    int s = ui_scale();
    int bw = 160 + 76 * s;
    int bh = 40 + 34 * s;
    fillrect(10, 10, bw, bh, C_BLACK);
    frame(10, 10, bw, bh, C_LGRAY);
    draw_text_s(18, 15, "PAPEROS", s, C_YELLOW, C_BLACK);
    draw_text(18, 15 + 22 * s, res, C_LGRAY, C_BLACK);
    draw_text(18, 15 + 38 * s, "Tiled wallpaper - native pixels", C_DGRAY, C_BLACK);

    draw_buf = old_buf;
    clip_x0 = ox0; clip_y0 = oy0; clip_x1 = ox1; clip_y1 = oy1;
}

/* Start button and popup menu geometry. */
#define START_X       4
#define START_W       76
#define START_GAP     6
#define MENU_HEADER_H 26
#define MENU_ITEM_H   24
#define MENU_ITEMS    12

/* Taskbar buttons grow with the framebuffer instead of staying clustered in
   the top-left corner on a 4K desktop. */
static int task_button_w(void)
{
    /* draw_taskbar() reserves SW-62 for the clock; size buttons so the
       12th one is still drawn at the smallest supported framebuffer. */
    int avail = SW - 62 - (START_X + START_W + START_GAP);
    int w = (avail - (MENU_ITEMS - 1) * 6) / MENU_ITEMS;
    if (w < 34) w = 34;
    if (w > 150) w = 150;
    return w;
}
static int task_button_x(int i)
{
    return START_X + START_W + START_GAP + i * (task_button_w() + 6);
}

/* Start button and its popup menu. The menu sits above the taskbar and is
   redrawn like any other widget, so it stays flicker-free at 4K. */
static int start_open = 0;
static int menu_hover = -1;

static int menu_width(void)
{
    int w = 220;
    if (SW < 320) w = SW - 12;
    return w;
}
static int menu_height(void)
{
    return MENU_HEADER_H + MENU_ITEMS * MENU_ITEM_H + 4;
}
static void start_menu_rect(DmgRect *r)
{
    int w = menu_width(), h = menu_height();
    r->x0 = START_X;
    r->y0 = TASKBAR_Y - h - 2;
    if (r->y0 < 0) r->y0 = 0;
    r->x1 = r->x0 + w;
    r->y1 = r->y0 + h;
    if (r->x1 > SW) r->x1 = SW;
    if (r->y1 > TASKBAR_Y) r->y1 = TASKBAR_Y;
}
static int start_menu_item(int x, int y)
{
    if (!start_open) return -1;
    DmgRect r;
    start_menu_rect(&r);
    if (x < r.x0 || x >= r.x1 || y < r.y0 || y >= r.y1) return -1;
    int row = y - (r.y0 + MENU_HEADER_H);
    if (row < 0 || row >= MENU_ITEMS * MENU_ITEM_H) return -1;
    return row / MENU_ITEM_H;
}
static void menu_item_rect(DmgRect *r, int item)
{
    start_menu_rect(r);
    int y = r->y0 + MENU_HEADER_H + 2 + item * MENU_ITEM_H;
    r->x0 += 3; r->x1 -= 3;
    r->y0 = y; r->y1 = y + MENU_ITEM_H - 2;
}
static void update_menu_hover(void)
{
    int h = start_open ? start_menu_item(mouse_x, mouse_y) : -1;
    if (h == menu_hover) return;
    if (menu_hover >= 0) {
        DmgRect r;
        menu_item_rect(&r, menu_hover);
        mark_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    }
    if (h >= 0) {
        DmgRect r;
        menu_item_rect(&r, h);
        mark_rect(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    }
    menu_hover = h;
}
static void draw_start_button(void)
{
    draw_bevel(START_X, TASKBAR_Y + 2, START_W, TASKBAR_H - 4, start_open);
    int ix = START_X + 5, iy = TASKBAR_Y + 5;
    fillrect(ix, iy, 10, 10, start_open ? C_WHITE : accent);
    fillrect(ix + 1, iy + 1, 4, 4, C_RED);
    fillrect(ix + 5, iy + 1, 4, 4, C_GREEN);
    fillrect(ix + 1, iy + 5, 4, 4, C_BLUE);
    fillrect(ix + 5, iy + 5, 4, 4, C_YELLOW);
    draw_text(START_X + 19, TASKBAR_Y + 4, "START",
              start_open ? C_WHITE : C_BLACK, C_FACE);
}
static void draw_start_menu(void)
{
    if (!start_open) return;
    DmgRect r;
    start_menu_rect(&r);
    int w = r.x1 - r.x0, h = r.y1 - r.y0;
    fillrect(r.x0 + 3, r.y0 + 3, w, h, C_BLACK);
    draw_bevel(r.x0, r.y0, w, h, 0);

    fillrect(r.x0 + 3, r.y0 + 3, w - 6, MENU_HEADER_H - 4, accent);
    hline(r.x0 + 3, r.y0 + 3, w - 6, C_HILITE);
    hline(r.x0 + 3, r.y0 + MENU_HEADER_H - 2, w - 6, C_SHADOW);
    draw_text(r.x0 + 12, r.y0 + 7, "PAPEROS 95", C_WHITE, accent);

    int list_y = r.y0 + MENU_HEADER_H;
    fillrect(r.x0 + 3, list_y, 4, h - MENU_HEADER_H - 3, accent);
    hline(r.x0 + 2, list_y, w - 4, C_SHADOW);
    static const char *labels[MENU_ITEMS] = {
        "Terminal", "Paint", "Notepad", "Snake", "Settings",
        "File Manager", "Calculator", "System Monitor",
        "Minesweeper", "2048", "Brick Drop", "Internet Browser"
    };
    int hover = menu_hover;
    for (int i = 0; i < MENU_ITEMS; i++) {
        int y = list_y + 2 + i * MENU_ITEM_H;
        int hot = (i == hover);
        fillrect(r.x0 + 9, y, w - 13, MENU_ITEM_H - 3,
                 hot ? accent : C_FACE);
        if (hot) frame(r.x0 + 9, y, w - 13, MENU_ITEM_H - 3, C_HILITE);
        fillrect(r.x0 + 13, y + 6, 6, 6, hot ? C_YELLOW : C_DGRAY);
        draw_text(r.x0 + 25, y + 5, labels[i],
                  hot ? C_WHITE : C_BLACK, hot ? accent : C_FACE);
    }
}

static void draw_taskbar(void)
{
    static const int types[MENU_ITEMS] = {0,1,2,3,4,5,6,7,8,9,10,11};
    static const char *lab[MENU_ITEMS] = {
        "TERM", "PAINT", "NOTE", "SNAKE", "SET", "FILES",
        "CALC", "MON", "MINE", "2048", "BRCK", "WEB"
    };
    fillrect(0, TASKBAR_Y, SW, SH - TASKBAR_Y, C_FACE);
    hline(0, TASKBAR_Y, SW, C_HILITE);
    hline(0, TASKBAR_Y + 1, SW, C_WHITE);
    draw_start_button();
    int bw = task_button_w();
    for (int i = 0; i < MENU_ITEMS; i++) {
        int x = task_button_x(i), y = TASKBAR_Y + 2;
        if (x + bw > SW - 62) break;      /* leave room for the clock */
        int h = TASKBAR_H - 4;
        int idx = find_win(types[i]);
        int active = idx >= 0 && !wins[idx].min && idx == focus;
        draw_bevel(x, y, bw, h, active);
        int maxchars = (bw - 6) / FONT_W;
        if (maxchars < 1) maxchars = 1;
        draw_text_n(x + 4, y, lab[i], maxchars,
                    active ? C_WHITE : C_BLACK, C_FACE);
    }
    draw_clock();
}

/* Recompose each dirty rectangle from the static desktop layer, skip windows
   and widgets that do not intersect it, then publish all rectangles once. */
static void redraw_damage(void)
{
    if (damage_empty()) return;
    u8 *old_buf = draw_buf;
    draw_buf = BACK;
    clip_full();

    for (int d = 0; d < dmg_n; d++) {
        const DmgRect *r = &dmg[d];
        int w = r->x1 - r->x0;
        if (w <= 0 || r->y1 <= r->y0) continue;
        for (int y = r->y0; y < r->y1; y++)
            fast_copy(BACK + y * SW + r->x0,
                      DESKTOP + y * SW + r->x0, (u32)w);

        set_clip(r->x0, r->y0, r->x1, r->y1);
        for (int i = 0; i < nwin; i++) {
            Win *win = &wins[i];
            if (!win->used || win->min) continue;
            if (!rect_hit(r, win->x - 1, win->y - 1,
                          win->w + 6, win->h + 6)) continue;
            draw_win(i);
        }
        if (rect_hit(r, 0, TASKBAR_Y, SW, SH - TASKBAR_Y))
            draw_taskbar();
        if (start_open) {
            DmgRect mr;
            start_menu_rect(&mr);
            if (rect_hit(r, mr.x0, mr.y0, mr.x1 - mr.x0, mr.y1 - mr.y0))
                draw_start_menu();
        }
        int cs = cursor_scale();
        if (rect_hit(r, mouse_x, mouse_y, CUR_W * cs, CUR_H * cs))
            draw_cursor(mouse_x, mouse_y);
    }

    clip_full();
    present_frame();
    draw_buf = old_buf;
    dmg_n = 0;
}

/* ========================== ATA / FILESYSTEM ========================= */
/* A small, real persistent filesystem on the primary IDE disk. Layout:
   LBA 0..1023    boot image reserved (stage 1, stage 2, kernel)
   LBA 1024       PAPEROS1 superblock
   LBA 1025-1026  directory (32 x 32-byte entries)
   LBA 1027+      contiguous file data
   Everything is written through ATA PIO in 28-bit LBA mode. */
#define ATA_DATA     0x1F0
#define ATA_SECTOR   0x1F2
#define ATA_LBA_LO   0x1F3
#define ATA_LBA_MID  0x1F4
#define ATA_LBA_HI   0x1F5
#define ATA_DRIVE    0x1F6
#define ATA_STATUS   0x1F7
#define ATA_COMMAND  0x1F7
#define ATA_CTRL     0x3F6
#define FS_BASE      1024u
#define FS_TOTAL     (32768u - FS_BASE)
#define FS_MAX_FILES 32

typedef struct {
    char name[16];
    u32 lba;
    u32 bytes;
    u8 used;
    u8 res[7];
} FSEnt;

static FSEnt fs_dir[FS_MAX_FILES];
static u32 fs_next = FS_BASE + 3;
static int fs_ready = 0;
static u8 fs_buf[1024];

static void ata_delay(void)
{
    for (int i = 0; i < 4; i++) (void)inb(0x84);
}
/* want_drq=1 waits for the drive to own the data port; want_drq=0 waits for
   it to release the port. Treating a stale DRQ as ready is what caused whole
   runs of kernel sectors to be written to the wrong place. */
static int ata_wait(int want_drq)
{
    for (u32 timeout = 0; timeout < 200000u; timeout++) {
        u8 st = inb(ATA_STATUS);
        if (st & 0x80) { ata_delay(); continue; }   /* BSY */
        if (st & 0x01) return 0;                    /* error */
        if (want_drq) {
            if (st & 0x08) return 1;
        } else {
            if (!(st & 0x08)) return 1;
        }
        ata_delay();
    }
    return 0;
}
static int ata_select(u32 lba)
{
    if (!ata_wait(0)) return 0;
    outb(ATA_DRIVE, 0xE0 | (u8)((lba >> 24) & 0x0F));
    ata_delay();
    return ata_wait(0);
}
static int ata_rw(u32 lba, u8 count, void *buf, int write)
{
    if (!count || count > 2) return 0;              /* directory sector pairs */
    if (!ata_select(lba)) return 0;
    outb(ATA_SECTOR, count);
    outb(ATA_LBA_LO, (u8)lba);
    outb(ATA_LBA_MID, (u8)(lba >> 8));
    outb(ATA_LBA_HI, (u8)(lba >> 16));
    outb(ATA_COMMAND, write ? 0x30 : 0x20);
    ata_delay();                       /* do not sample stale pre-command status */
    u8 *b = (u8 *)buf;
    for (u32 i = 0; i < (u32)count * 256u; i++) {
        if (!ata_wait(1)) return 0;
        if (write) outw(ATA_DATA, (u16)((b[i * 2 + 1] << 8) | b[i * 2]));
        else { u16 v = inw(ATA_DATA); b[i * 2] = (u8)v; b[i * 2 + 1] = (u8)(v >> 8); }
    }
    return ata_wait(0);
}
static int ata_read(u32 lba, u8 count, void *buf) { return ata_rw(lba, count, buf, 0); }
static int ata_write(u32 lba, u8 count, const void *buf)
{
    return ata_rw(lba, count, (void *)buf, 1);
}
static int ata_flush(void)
{
    if (!ata_wait(0)) return 0;
    outb(ATA_COMMAND, 0xE7);           /* CACHE FLUSH */
    return ata_wait(0);
}

static void fs_set_magic(u8 *sector)
{
    const char *m = "PAPEROS1";
    for (int i = 0; i < 8; i++) sector[i] = (u8)m[i];
}
static int fs_check_magic(const u8 *sector)
{
    const char *m = "PAPEROS1";
    for (int i = 0; i < 8; i++) if (sector[i] != (u8)m[i]) return 0;
    return 1;
}
static void fs_put32(u8 *p, u32 v)
{
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static u32 fs_get32(const u8 *p)
{
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void fs_copy_name(char *dst, const char *src)
{
    int i = 0;
    for (; i < 15 && src[i]; i++) dst[i] = src[i];
    for (; i < 16; i++) dst[i] = 0;
}
static int fs_name_eq(const char *a, const char *b)
{
    for (int i = 0; i < 16; i++) {
        char ca = a[i], cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 'a' + 'A');
        if (ca != cb) return 0;
        if (!ca) return 1;
    }
    return 1;
}
static int fs_save_super(void)
{
    memset(fs_buf, 0, 512);
    fs_set_magic(fs_buf);
    fs_put32(fs_buf + 8, FS_TOTAL);
    fs_put32(fs_buf + 12, FS_BASE + 1);
    fs_put32(fs_buf + 16, FS_BASE + 3);
    fs_put32(fs_buf + 20, fs_next);
    if (!ata_write(FS_BASE, 1, fs_buf)) return 0;
    return ata_flush();
}
static void fs_dir_to_buf(void)
{
    memset(fs_buf, 0, 1024);
    for (int i = 0; i < FS_MAX_FILES; i++) {
        u8 *e = fs_buf + i * 32;
        for (int j = 0; j < 16; j++) e[j] = (u8)fs_dir[i].name[j];
        fs_put32(e + 16, fs_dir[i].lba);
        fs_put32(e + 20, fs_dir[i].bytes);
        e[24] = fs_dir[i].used;
    }
}
static void fs_buf_to_dir(void)
{
    for (int i = 0; i < FS_MAX_FILES; i++) {
        const u8 *e = fs_buf + i * 32;
        for (int j = 0; j < 16; j++) fs_dir[i].name[j] = (char)e[j];
        fs_dir[i].lba = fs_get32(e + 16);
        fs_dir[i].bytes = fs_get32(e + 20);
        fs_dir[i].used = e[24];
    }
}
static int fs_save_dir(void)
{
    fs_dir_to_buf();
    if (!ata_write(FS_BASE + 1, 2, fs_buf)) return 0;
    return ata_flush();
}
static int fs_load(void)
{
    if (!ata_read(FS_BASE, 1, fs_buf) || !fs_check_magic(fs_buf)) return 0;
    fs_next = fs_get32(fs_buf + 20);
    if (fs_next < FS_BASE + 3 || fs_next >= FS_BASE + FS_TOTAL) fs_next = FS_BASE + 3;
    if (!ata_read(FS_BASE + 1, 2, fs_buf)) return 0;
    fs_buf_to_dir();
    fs_ready = 1;
    return 1;
}
static int fs_format(void)
{
    memset(fs_dir, 0, sizeof(fs_dir));
    fs_next = FS_BASE + 3;
    fs_ready = 0;
    if (!fs_save_super() || !fs_save_dir()) return 0;
    fs_ready = 1;
    return 1;
}
static int fs_find(const char *name)
{
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fs_dir[i].used && fs_name_eq(fs_dir[i].name, name)) return i;
    return -1;
}
static int fs_touch(const char *name)
{
    if (!fs_ready || !name[0] || fs_find(name) >= 0) return 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_dir[i].used) {
            memset(&fs_dir[i], 0, sizeof(FSEnt));
            fs_copy_name(fs_dir[i].name, name);
            fs_dir[i].lba = fs_next;
            fs_dir[i].bytes = 0;
            fs_dir[i].used = 1;
            return fs_save_dir();
        }
    }
    return 0;
}
static int fs_write(const char *name, const u8 *data, u32 len)
{
    if (!fs_ready || !name[0]) return 0;
    int idx = fs_find(name);
    u32 sectors = (len + 511) / 512;
    if (idx < 0) {
        for (idx = 0; idx < FS_MAX_FILES; idx++) if (!fs_dir[idx].used) break;
        if (idx >= FS_MAX_FILES) return 0;
        memset(&fs_dir[idx], 0, sizeof(FSEnt));
        fs_copy_name(fs_dir[idx].name, name);
        fs_dir[idx].used = 1;
    }
    u32 start = fs_dir[idx].lba;
    if (start < FS_BASE + 3) start = fs_next;
    if (len > fs_dir[idx].bytes || sectors == 0) {
        start = fs_next;
        fs_next += sectors ? sectors : 1;
        if (fs_next >= FS_BASE + FS_TOTAL) return 0;
        if (!fs_save_super()) return 0;
    }
    for (u32 s = 0; s < sectors; s++) {
        u32 off = s * 512;
        u32 chunk = len - off;
        if (chunk > 512) chunk = 512;
        memset(fs_buf, 0, 512);
        for (u32 j = 0; j < chunk; j++) fs_buf[j] = data[off + j];
        if (!ata_write(start + s, 1, fs_buf)) return 0;
    }
    if (!ata_flush()) return 0;
    fs_dir[idx].lba = start;
    fs_dir[idx].bytes = len;
    return fs_save_dir();
}
static int fs_read(const char *name, u8 *dst, u32 max)
{
    if (!fs_ready) return 0;
    int idx = fs_find(name);
    if (idx < 0) return 0;
    u32 len = fs_dir[idx].bytes;
    if (len > max) len = max;
    u32 sectors = (len + 511) / 512;
    for (u32 s = 0; s < sectors; s++) {
        if (!ata_read(fs_dir[idx].lba + s, 1, fs_buf)) return 0;
        u32 off = s * 512;
        u32 chunk = len - off;
        if (chunk > 512) chunk = 512;
        for (u32 j = 0; j < chunk; j++) dst[off + j] = fs_buf[j];
    }
    return (int)len;
}
static int fs_delete(const char *name)
{
    if (!fs_ready) return 0;
    int idx = fs_find(name);
    if (idx < 0) return 0;
    memset(&fs_dir[idx], 0, sizeof(FSEnt));
    return fs_save_dir();
}
static u32 fs_used_sectors(void)
{
    u32 used = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fs_dir[i].used) used += (fs_dir[i].bytes + 511) / 512;
    return used;
}

/* ============================= APPS ================================= */

/* ---------------------------- terminal ------------------------------ */
#define TLN 32
#define TLC 160
static char term[TLN][TLC + 1];
static int term_n = 0;
static char tin[TLC + 1];
static int tin_len = 0;
static u8 term_filebuf[4608];
static int note_save(const char *name);
static int note_load(const char *name);

static void term_push(const char *s)
{
    if (term_n >= TLN) {
        for (int i = 0; i < TLN - 1; i++) strcpy(term[i], term[i + 1]);
        term_n = TLN - 1;
    }
    int n = 0;
    while (s[n] && n < TLC) { term[term_n][n] = s[n]; n++; }
    term[term_n][n] = 0;
    term_n++;
    mark_content(find_win(0));
}
static void term_help(void)
{
    term_push("help ver clear echo color wall date about");
    term_push("files ls cat <f> touch <f> rm <f> format df");
    term_push("save <f> load <f> edit calc mon games");
    term_push("net ip ping <ip> web [url] snake paint note");
    term_push("mines g2048 tetris settings reboot");
}
static void term_u32(const char *prefix, u32 v)
{
    char s[TLC + 1];
    int n = 0;
    while (*prefix && n < TLC) s[n++] = *prefix++;
    if (n < TLC) { itoa(v, s + n); n += strlen(s + n); }
    s[n] = 0;
    term_push(s);
}
static void term_fs_list(void)
{
    if (!fs_ready) { term_push("FS: not ready (type format)"); return; }
    int shown = 0;
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_dir[i].used) continue;
        char s[TLC + 1];
        int n = 0;
        for (int j = 0; j < 16 && fs_dir[i].name[j]; j++) s[n++] = fs_dir[i].name[j];
        while (n < 18) s[n++] = ' ';
        itoa(fs_dir[i].bytes, s + n); n += strlen(s + n);
        s[n] = 0;
        term_push(s);
        shown++;
    }
    if (!shown) term_push("(empty)");
}
static void term_fs_cat(const char *name)
{
    if (!name[0]) { term_push("cat <file>"); return; }
    int len = fs_read(name, term_filebuf, sizeof(term_filebuf));
    if (len < 0) { term_push("FS: read failed"); return; }
    if (!len) { term_push("(empty file)"); return; }
    int start = 0;
    for (int i = 0; i <= len; i++) {
        if (i == len || term_filebuf[i] == '\n') {
            char s[TLC + 1];
            int n = 0;
            for (int j = start; j < i && n < TLC; j++)
                if (term_filebuf[j] != '\r') s[n++] = (char)term_filebuf[j];
            s[n] = 0;
            term_push(s);
            start = i + 1;
        }
    }
}
static void term_command(void)
{
    char cmd[TLC + 1];
    strcpy(cmd, tin);
    char prompt[TLC + 3];
    prompt[0] = '>'; prompt[1] = ' ';
    strcpy(prompt + 2, cmd);
    term_push(prompt);
    tin_len = 0; tin[0] = 0;

    if (streq(cmd, "help")) term_help();
    else if (streq(cmd, "ver")) term_push("PAPEROS 0.9 / storage + installer + TTY + 12 apps");
    else if (streq(cmd, "clear")) { term_n = 0; }
    else if (streq(cmd, "about"))
        term_push("Made from scratch on this PC.");
    else if (streq(cmd, "win")) {
        char s[TLC + 1]; int k = 0;
        s[k++] = 'W'; s[k++] = 'I'; s[k++] = 'N'; s[k++] = ':'; s[k++] = ' ';
        for (int i = 0; i < nwin; i++)
            if (wins[i].used) { s[k++] = lab_char(wins[i].type); s[k++] = ' '; }
        s[k] = 0; term_push(s);
    } else if (streq(cmd, "date")) {
        char s[TLC + 1];
        int d = bcd(cmos(0x07)), mo = bcd(cmos(0x08)), y = bcd(cmos(0x09));
        s[0] = '2'; s[1] = '0'; s[2] = (char)('0' + y / 10); s[3] = (char)('0' + y % 10);
        s[4] = '-'; s[5] = (char)('0' + mo / 10); s[6] = (char)('0' + mo % 10);
        s[7] = '-'; s[8] = (char)('0' + d / 10); s[9] = (char)('0' + d % 10);
        s[10] = 0; term_push(s);
    } else if (strncmp_local(cmd, "echo ") == 0) term_push(cmd + 5);
    else if (strncmp_local(cmd, "color ") == 0) {
        int v = atoi_local(cmd + 6);
        if (v >= 0 && v < 16) { accent = (u8)v; term_push("accent changed"); }
        else term_push("use 0-15");
    } else if (strncmp_local(cmd, "wall ") == 0) {
        int v = atoi_local(cmd + 5);
        if (v >= 0 && v < 16) { wall_color = (u8)v; term_push("wallpaper changed"); }
        else term_push("use 0-15");
    } else if (streq(cmd, "format")) {
        term_push(fs_format() ? "FS: formatted" : "FS: format failed");
    } else if (streq(cmd, "ls")) term_fs_list();
    else if (strncmp_local(cmd, "cat ") == 0) term_fs_cat(cmd + 4);
    else if (strncmp_local(cmd, "touch ") == 0)
        term_push(fs_touch(cmd + 6) ? "FS: created" : "FS: create failed");
    else if (strncmp_local(cmd, "rm ") == 0)
        term_push(fs_delete(cmd + 3) ? "FS: removed" : "FS: remove failed");
    else if (streq(cmd, "df")) {
        if (!fs_ready) term_push("FS: not ready (type format)");
        else {
            term_u32("FS used: ", fs_used_sectors());
            term_u32("FS total: ", FS_TOTAL);
        }
    } else if (strncmp_local(cmd, "save ") == 0)
        term_push(note_save(cmd + 5) ? "NOTE: saved" : "NOTE: save failed");
    else if (strncmp_local(cmd, "load ") == 0)
        term_push(note_load(cmd + 5) ? "NOTE: loaded" : "NOTE: load failed");
    else if (streq(cmd, "edit")) open_win(2);
    else if (streq(cmd, "files")) open_win(5);
    else if (streq(cmd, "calc")) open_win(6);
    else if (streq(cmd, "mon")) open_win(7);
    else if (streq(cmd, "net")) net_init();
    else if (streq(cmd, "ip")) net_cmd_ip();
    else if (strncmp_local(cmd, "ping ") == 0) net_cmd_ping(cmd + 5);
    else if (streq(cmd, "snake")) open_win(3);
    else if (streq(cmd, "paint")) open_win(1);
    else if (streq(cmd, "note")) open_win(2);
    else if (streq(cmd, "settings")) open_win(4);
    else if (streq(cmd, "mines")) open_win(8);
    else if (streq(cmd, "g2048")) open_win(9);
    else if (streq(cmd, "tetris") || streq(cmd, "brick")) open_win(10);
    else if (streq(cmd, "games")) {
        open_win(8); open_win(9); open_win(10);
        term_push("Games: Minesweeper, 2048, Brick Drop");
    }
    else if (streq(cmd, "web")) open_win(11);
    else if (strncmp_local(cmd, "web ") == 0) {
        open_win(11);
        web_fetch(cmd + 4, 1);
    }
    else if (streq(cmd, "reboot")) { outb(0x64, 0xFE); for (;;) {} }
    else if (cmd[0]) { term_push("unknown command"); }
}
static int strncmp_local(const char *a, const char *b)
{
    while (*b) { if (*a++ != *b++) return 1; }
    return 0;
}
static int atoi_local(const char *s)
{
    int v = 0, neg = 0;
    if (*s == '-') { neg = 1; s++; }
    while (*s >= '0' && *s <= '9') v = v * 10 + (*s++ - '0');
    return neg ? -v : v;
}
static char lab_char(int type)
{
    static const char c[12] = {'T','P','N','S','C','F','K','M','B','2','R','W'};
    if (type < 0 || type >= 12) return '?';
    return c[type];
}
static void term_key(u8 k)
{
    if (k == K_ENTER) { term_command(); return; }
    if (k == K_BACK) { if (tin_len) tin[--tin_len] = 0; return; }
    if (k >= 32 && k < 127 && tin_len < TLC) { tin[tin_len++] = (char)k; tin[tin_len] = 0; }
}
static void draw_terminal(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_BLACK);
    frame(cx, cy, cw, ch, C_DGRAY);
    int cols = cw / FONT_W;
    int rows = (ch - FONT_H) / FONT_H;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    int start = term_n > rows ? term_n - rows : 0;
    int y = cy + 2;
    for (int i = start; i < term_n && i - start < rows; i++, y += FONT_H)
        draw_text_n(cx + 3, y, term[i], cols, C_LGREEN, C_BLACK);
    char p[TLC + 3];
    p[0] = '>'; p[1] = ' ';
    for (int i = 0; i < tin_len; i++) p[2 + i] = tin[i];
    p[2 + tin_len] = 0;
    if (y + FONT_H <= cy + ch)
        draw_text_n(cx + 3, y, p, cols, C_YELLOW, C_BLACK);
}

/* ------------------------------ paint ------------------------------- */
/* The canvas lives above the two frame-sized buffers and grows with the
   Paint window, so a 4K desktop gets a useful drawing area without adding
   megabytes to the kernel binary. */
#define CANVAS_MAX_W 1600
#define CANVAS_MAX_H 1200
static u8 *canvas = CANVAS_MEM;
static int PCW = 0, PCH = 0;
static int pcolor = C_LRED;
static int painting = 0, lastpx = -1, lastpy = -1;

static void paint_resize(int cw, int ch)
{
    int w = cw - 10, h = ch - 44;
    if (w < 64) w = 64;
    if (h < 48) h = 48;
    if (w > CANVAS_MAX_W) w = CANVAS_MAX_W;
    if (h > CANVAS_MAX_H) h = CANVAS_MAX_H;
    if (w != PCW || h != PCH) {
        PCW = w; PCH = h;
        memset(canvas, 0, (u32)PCW * PCH);
        painting = 0; lastpx = lastpy = -1;
    }
}
static void paint_palette_size(int cw, int *pw, int *step)
{
    int w = 10 + cw / 80;
    if (w < 10) w = 10;
    if (w > 24) w = 24;
    *pw = w;
    *step = w + 4;
}
static void paint_plot(int x, int y)
{
    if (x < 0 || y < 0 || x >= PCW || y >= PCH) return;
    canvas[y * PCW + x] = (u8)pcolor;
    if (x + 1 < PCW) canvas[y * PCW + x + 1] = (u8)pcolor;
    if (y + 1 < PCH) canvas[(y + 1) * PCW + x] = (u8)pcolor;
}
static void paint_line(int x0, int y0, int x1, int y1)
{
    int dx = x1 - x0, dy = y1 - y0, n = dx > dy ? (dx > -dx ? dx : -dx) : (dy > -dy ? dy : -dy);
    if (!n) { paint_plot(x0, y0); return; }
    for (int i = 0; i <= n; i++)
        paint_plot(x0 + dx * i / n, y0 + dy * i / n);
}
/* Blit only the visible part of the canvas, one row at a time. */
static void blit_canvas(int cx, int cy)
{
    int x0 = 0, y0 = 0;
    int gx0 = cx, gy0 = cy, gx1 = cx + PCW, gy1 = cy + PCH;
    int dx = clip_x0 - gx0, dy = clip_y0 - gy0;
    if (dx > 0) { x0 += dx; gx0 += dx; }
    if (dy > 0) { y0 += dy; gy0 += dy; }
    if (gx1 > clip_x1) gx1 = clip_x1;
    if (gy1 > clip_y1) gy1 = clip_y1;
    if (gx0 < 0) { x0 -= gx0; gx0 = 0; }
    if (gy0 < 0) { y0 -= gy0; gy0 = 0; }
    if (gx1 > SW) gx1 = SW;
    if (gy1 > SH) gy1 = SH;
    int w = gx1 - gx0, h = gy1 - gy0;
    if (w <= 0 || h <= 0 || x0 < 0 || y0 < 0) return;
    for (int y = 0; y < h; y++)
        fast_copy(draw_buf + (gy0 + y) * SW + gx0,
                  canvas + (y0 + y) * PCW + x0, (u32)w);
}
static void draw_paint(int cx, int cy, int cw, int ch)
{
    paint_resize(cw, ch);
    fillrect(cx, cy, cw, ch, C_DGRAY);
    blit_canvas(cx, cy);
    frame(cx, cy, PCW + 1, PCH + 1, C_BLACK);

    int pw, step;
    paint_palette_size(cw, &pw, &step);
    int py = cy + PCH + 6;
    for (int i = 0; i < 16; i++) {
        int x = cx + 4 + i * step;
        if (x + pw > cx + cw) break;
        fillrect(x, py, pw, 12, (u8)i);
        frame(x, py, pw, 12, (pcolor == i) ? C_WHITE : C_BLACK);
    }
    draw_text_n(cx + 4, py + 16, "draw: hold left mouse | C: clear",
                cw / FONT_W, C_BLACK, C_DGRAY);
}
static void paint_click(int lx, int ly, int cw, int ch)
{
    paint_resize(cw, ch);
    int pw, step;
    paint_palette_size(cw, &pw, &step);
    int py = PCH + 6;
    if (ly >= py && ly < py + 12) {
        int i = (lx - 4) / step;
        if (i >= 0 && i < 16 && lx >= 4 + i * step && lx < 4 + i * step + pw)
            pcolor = i;
        return;
    }
    if (lx >= 0 && ly >= 0 && lx < PCW && ly < PCH) {
        painting = 1; lastpx = lx; lastpy = ly;
        paint_plot(lx, ly);
    }
}
static void paint_mouse(int mx, int my, int btns, int cx, int cy, int cw, int ch)
{
    paint_resize(cw, ch);
    int lx = mx - cx, ly = my - cy;
    if (!(btns & 1)) { painting = 0; lastpx = lastpy = -1; return; }
    if (!painting) return;
    if (lx >= 0 && ly >= 0 && lx < PCW && ly < PCH) {
        if (lastpx >= 0) paint_line(lastpx, lastpy, lx, ly);
        else paint_plot(lx, ly);
        lastpx = lx; lastpy = ly;
    }
}

/* ------------------------------ notepad ----------------------------- */
#define NLN 28
#define NLC 160
#define NOTE_FILE_MAX 4608
static char note[NLN][NLC + 1];
static int nline = 0, ncol = 0;
static char note_name[16] = "NOTE.TXT";
static int note_name_len = 8;
static int note_name_focus = 0;
static u8 note_filebuf[NOTE_FILE_MAX];

static void note_set_name(const char *name)
{
    int i = 0;
    for (; i < 15 && name[i]; i++) note_name[i] = name[i];
    note_name[i] = 0;
    note_name_len = i;
}
static int note_save(const char *name)
{
    if (!fs_ready || !name[0]) return 0;
    int last = NLN - 1;
    while (last > 0 && note[last][0] == 0) last--;
    u32 len = 0;
    for (int i = 0; i <= last; i++) {
        u32 n = strlen(note[i]);
        for (u32 j = 0; j < n && len < NOTE_FILE_MAX; j++) note_filebuf[len++] = (u8)note[i][j];
        if (len < NOTE_FILE_MAX) note_filebuf[len++] = '\n';
    }
    if (!fs_write(name, note_filebuf, len)) return 0;
    note_set_name(name);
    return 1;
}
static int note_load(const char *name)
{
    if (!name[0]) return 0;
    int len = fs_read(name, note_filebuf, NOTE_FILE_MAX - 1);
    if (len < 0) { term_push("FS: read failed"); return 0; }
    memset(note, 0, sizeof(note));
    nline = 0; ncol = 0;
    int line = 0, col = 0;
    for (int i = 0; i < len && line < NLN; i++) {
        u8 c = note_filebuf[i];
        if (c == '\n') {
            line++; col = 0;
        } else if (c != '\r' && col < NLC) {
            note[line][col++] = (char)c;
        }
    }
    note_set_name(name);
    note_name_focus = 0;
    return 1;
}
static void note_key(u8 k)
{
    if (note_name_focus) {
        if (k == K_ENTER || k == K_ESC) { note_name_focus = 0; return; }
        if (k == K_BACK) { if (note_name_len) note_name[--note_name_len] = 0; return; }
        if (k >= 32 && k < 127 && note_name_len < 15) {
            note_name[note_name_len++] = (char)k;
            note_name[note_name_len] = 0;
        }
        return;
    }
    if (k == K_LEFT) { if (ncol) ncol--; return; }
    if (k == K_RIGHT) { if (ncol < strlen(note[nline])) ncol++; return; }
    if (k == K_UP) { if (nline) { nline--; if (ncol > strlen(note[nline])) ncol = strlen(note[nline]); } return; }
    if (k == K_DOWN) { if (nline < NLN - 1) { nline++; if (ncol > strlen(note[nline])) ncol = strlen(note[nline]); } return; }
    if (k == K_BACK) {
        if (ncol) {
            for (int i = ncol - 1; i < (int)strlen(note[nline]); i++)
                note[nline][i] = note[nline][i + 1];
            ncol--;
        }
        return;
    }
    if (k == K_ENTER) {
        if (nline < NLN - 1) {
            for (int i = NLN - 1; i > nline + 1; i--) strcpy(note[i], note[i - 1]);
            note[nline + 1][0] = 0;
            for (int i = strlen(note[nline]); i > ncol; i--)
                note[nline + 1][i - ncol - 1] = note[nline][i];
            note[nline][ncol] = 0;
            nline++; ncol = 0;
        }
        return;
    }
    if (k >= 32 && k < 127 && ncol < NLC && strlen(note[nline]) < NLC) {
        for (int i = (int)strlen(note[nline]) + 1; i > ncol; i--)
            note[nline][i] = note[nline][i - 1];
        note[nline][ncol++] = (char)k;
    }
}
static void draw_notepad(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_WHITE);
    frame(cx, cy, cw, ch, C_BLACK);
    int cols = cw / FONT_W;
    if (cols < 1) cols = 1;

    fillrect(cx + 3, cy + 2, 170, FONT_H + 2,
             note_name_focus ? C_YELLOW : C_LGRAY);
    frame(cx + 3, cy + 2, 170, FONT_H + 2, C_BLACK);
    draw_text_n(cx + 6, cy + 3, note_name, 20, C_BLACK,
                note_name_focus ? C_YELLOW : C_LGRAY);

    const char *labels[3] = {"Save", "Load", "New"};
    for (int i = 0; i < 3; i++) {
        int x = cx + cw - 66 * (3 - i);
        if (x < cx + 180) continue;
        draw_bevel(x, cy + 2, 60, FONT_H + 2, 0);
        draw_text(x + 10, cy + 3, labels[i], C_BLACK, C_FACE);
    }

    int text_top = 24;
    int text_rows = (ch - text_top - FONT_H) / FONT_H;
    if (text_rows < 1) text_rows = 1;
    int start = nline >= text_rows ? nline - text_rows + 1 : 0;
    for (int i = start; i < NLN && i - start < text_rows; i++)
        draw_text_n(cx + 4, cy + text_top + (i - start) * FONT_H,
                    note[i], cols, C_BLACK, C_WHITE);
    int cyy = cy + text_top + (nline - start) * FONT_H;
    int ccol = ncol < cols ? ncol : cols - 1;
    fillrect(cx + 4 + ccol * FONT_W, cyy, FONT_W, FONT_H, C_BLUE);
    draw_text_n(cx + 4, cy + ch - FONT_H,
                "click name to rename | Save/Load buttons | Enter newline",
                cols, C_DGRAY, C_WHITE);
}
static void note_click(int lx, int ly, int cw, int ch)
{
    (void)ch;
    note_name_focus = 0;
    if (ly >= 2 && ly < 24) {
        if (lx >= 3 && lx < 173) {
            /* Clicking the name field selects it for replacement, so typing
               a new filename does not append to the default NOTE.TXT. */
            note_name_focus = 1;
            note_name[0] = 0;
            note_name_len = 0;
            return;
        }
        for (int i = 0; i < 3; i++) {
            int x = cw - 66 * (3 - i);
            if (lx >= x && lx < x + 60) {
                if (i == 0) note_save(note_name);
                else if (i == 1) note_load(note_name);
                else {
                    memset(note, 0, sizeof(note));
                    nline = ncol = 0;
                }
                return;
            }
        }
    }
}

/* ------------------------------- snake ------------------------------ */
#define SNAKEMAX 512
static int snx[SNAKEMAX], sny[SNAKEMAX], snlen, sndir, apple_x, apple_y, snscore, snover;
static int snake_gw = 39, snake_gh = 32, snake_cell = 8;
static u32 rng = 12345;
static void snake_reset(void)
{
    if (snake_gw < 8) { snake_gw = 39; snake_gh = 32; snake_cell = 8; }
    snlen = 4; sndir = 4; snscore = 0; snover = 0;
    int sx = snake_gw / 2, sy = snake_gh / 3;
    for (int i = 0; i < snlen; i++) { snx[i] = sx - i; sny[i] = sy; }
    apple_x = sx + 8; apple_y = sy;
    if (apple_x >= snake_gw) apple_x = snake_gw - 2;
    if (apple_x < 1) apple_x = 1;
}
static void set_snake_grid(int cw, int ch)
{
    int cell = 8;
    if (cw < 340 || ch < 280) cell = 4;
    if (cw >= 700 || ch >= 560) cell = 12;
    if (cw >= 1000 || ch >= 800) cell = 16;
    snake_cell = cell;
    snake_gw = cw / cell;
    snake_gh = (ch - FONT_H) / cell;
    if (snake_gw < 8) snake_gw = 8;
    if (snake_gh < 8) snake_gh = 8;
    if (snake_gw > 160) snake_gw = 160;
    if (snake_gh > 100) snake_gh = 100;
    if (apple_x >= snake_gw) apple_x = snake_gw - 2;
    if (apple_y >= snake_gh) apple_y = snake_gh - 2;
    if (apple_x < 1) apple_x = 1;
    if (apple_y < 1) apple_y = 1;
    for (int i = 0; i < snlen; i++) {
        if (snx[i] >= snake_gw) snx[i] = snake_gw - 1;
        if (sny[i] >= snake_gh) sny[i] = snake_gh - 1;
        if (snx[i] < 0) snx[i] = 0;
        if (sny[i] < 0) sny[i] = 0;
    }
}
static void snake_step(void)
{
    if (snover) return;
    int nx = snx[0] + (sndir == 4 ? 1 : sndir == 3 ? -1 : 0);
    int ny = sny[0] + (sndir == 2 ? -1 : sndir == 5 ? 1 : 0);
    if (nx < 0 || ny < 0 || nx >= snake_gw || ny >= snake_gh) { snover = 1; return; }
    for (int i = 0; i < snlen; i++)
        if (snx[i] == nx && sny[i] == ny) { snover = 1; return; }
    for (int i = snlen; i > 0; i--) { snx[i] = snx[i - 1]; sny[i] = sny[i - 1]; }
    snx[0] = nx; sny[0] = ny;
    if (nx == apple_x && ny == apple_y) {
        if (snlen < SNAKEMAX - 1) snlen++;
        snscore++;
        rng = rng * 1103515245 + 12345;
        apple_x = (int)((rng >> 16) % (u32)(snake_gw - 2)) + 1;
        apple_y = (int)((rng >> 8) % (u32)(snake_gh - 2)) + 1;
    }
}
static void snake_key(u8 k)
{
    if (k == K_UP && sndir != 5) sndir = 2;
    else if (k == K_DOWN && sndir != 2) sndir = 5;
    else if (k == K_LEFT && sndir != 4) sndir = 3;
    else if (k == K_RIGHT && sndir != 3) sndir = 4;
    else if (k == 'r' || k == 'R') snake_reset();
}
static void draw_snake(int cx, int cy, int cw, int ch)
{
    set_snake_grid(cw, ch);
    int cell = snake_cell;
    fillrect(cx, cy, cw, ch, C_BLACK);
    frame(cx, cy, cw, ch, C_DGRAY);
    int dot = cell >= 12 ? 2 : 1;
    for (int y = 0; y + cell <= ch - FONT_H; y += cell)
        for (int x = 0; x + cell <= cw; x += cell)
            fillrect(cx + x, cy + y, dot, dot, RGB(0,0,1));
    fillrect(cx + apple_x * cell, cy + apple_y * cell, cell, cell, C_LRED);
    for (int i = 0; i < snlen; i++)
        fillrect(cx + snx[i] * cell, cy + sny[i] * cell, cell, cell,
                 i == 0 ? C_YELLOW : C_LGREEN);
    char s[24];
    strcpy(s, "SCORE ");
    itoa((u32)snscore, s + 6);
    draw_text_n(cx + 4, cy + ch - FONT_H, s, cw / FONT_W, C_WHITE, C_BLACK);
    if (snover)
        draw_text_n(cx + 4, cy + ch / 2 - FONT_H / 2, "GAME OVER - press R",
                    cw / FONT_W, C_LRED, C_BLACK);
}

/* ---------------------------- minesweeper --------------------------- */
#define MS_W   16
#define MS_H   12
#define MS_MINES 28
static u8 ms_mine[MS_W][MS_H], ms_shown[MS_W][MS_H], ms_flag[MS_W][MS_H];
static int ms_started, ms_over, ms_won, ms_flagmode, ms_left;

static int ms_neighbors(int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dy) continue;
            int nx = x + dx, ny = y + dy;
            if (nx >= 0 && nx < MS_W && ny >= 0 && ny < MS_H && ms_mine[nx][ny]) n++;
        }
    return n;
}

static void ms_reset(void)
{
    memset(ms_mine, 0, sizeof(ms_mine));
    memset(ms_shown, 0, sizeof(ms_shown));
    memset(ms_flag, 0, sizeof(ms_flag));
    ms_started = 0; ms_over = 0; ms_won = 0;
    ms_flagmode = 0; ms_left = MS_MINES;
}

static void ms_place(int fx, int fy)
{
    int placed = 0;
    while (placed < MS_MINES) {
        rng = rng * 1103515245u + 12345u;
        int x = (int)((rng >> 16) % MS_W);
        rng = rng * 1103515245u + 12345u;
        int y = (int)((rng >> 8) % MS_H);
        int dx = x > fx ? x - fx : fx - x;
        int dy = y > fy ? y - fy : fy - y;
        if (ms_mine[x][y] || (dx <= 1 && dy <= 1)) continue;
        ms_mine[x][y] = 1;
        placed++;
    }
    ms_started = 1;
}

static void ms_reveal(int x, int y)
{
    if (x < 0 || x >= MS_W || y < 0 || y >= MS_H) return;
    if (ms_flag[x][y] || ms_shown[x][y]) return;
    ms_shown[x][y] = 1;
    if (ms_mine[x][y]) { ms_over = 1; return; }
    if (ms_neighbors(x, y)) return;
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++)
            if (dx || dy) ms_reveal(x + dx, y + dy);
}

static void ms_check_win(void)
{
    int hidden = 0;
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++)
            if (!ms_mine[x][y] && !ms_shown[x][y]) hidden++;
    if (hidden) return;
    ms_won = 1;
    for (int y = 0; y < MS_H; y++)
        for (int x = 0; x < MS_W; x++)
            if (ms_mine[x][y]) ms_flag[x][y] = 1;
    ms_left = 0;
}

static void ms_layout(int cw, int ch, int *cell, int *ox, int *oy)
{
    int c = (cw - 16) / MS_W;
    int c2 = (ch - 44) / MS_H;
    if (c2 < c) c = c2;
    if (c < 12) c = 12;
    if (c > 28) c = 28;
    *cell = c;
    *ox = (cw - c * MS_W) / 2;
    if (*ox < 4) *ox = 4;
    *oy = 24;
    if (*oy + c * MS_H + 4 > ch) *oy = ch - c * MS_H - 4;
    if (*oy < 20) *oy = 20;
}

static void ms_click(int lx, int ly, int cw, int ch)
{
    if (ly >= 2 && ly < 20 && lx >= cw - 62 && lx < cw - 6) {
        ms_reset();
        return;
    }
    int cell, ox, oy;
    ms_layout(cw, ch, &cell, &ox, &oy);
    int x = (lx - ox) / cell, y = (ly - oy) / cell;
    if (x < 0 || x >= MS_W || y < 0 || y >= MS_H || ms_over || ms_won) return;
    if (ms_flagmode) {
        if (!ms_shown[x][y]) {
            if (ms_flag[x][y]) { ms_flag[x][y] = 0; ms_left++; }
            else { ms_flag[x][y] = 1; ms_left--; }
        }
        return;
    }
    if (!ms_started) ms_place(x, y);
    ms_reveal(x, y);
    if (!ms_over) ms_check_win();
}

static void ms_key(u8 k)
{
    if (k == 'r' || k == 'R') ms_reset();
    else if (k == 'f' || k == 'F') ms_flagmode = !ms_flagmode;
}

static void draw_mines(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_FACE);
    frame(cx, cy, cw, ch, C_BLACK);
    char s[56];
    int n = 0;
    const char *a = "MINES "; while (*a) s[n++] = *a++;
    itoa((u32)(ms_left < 0 ? 0 : ms_left), s + n); n += strlen(s + n);
    a = ms_flagmode ? " | FLAG MODE" : " | DIG MODE";
    while (*a) s[n++] = *a++;
    a = " | R NEW  F FLAG"; while (*a) s[n++] = *a++;
    s[n] = 0;
    draw_text_n(cx + 6, cy + 4, s, cw / FONT_W, C_BLACK, C_FACE);
    draw_bevel(cx + cw - 58, cy + 2, 52, 16, 0);
    draw_text(cx + cw - 48, cy + 3, "NEW", C_BLACK, C_FACE);

    int cell, ox, oy;
    ms_layout(cw, ch, &cell, &ox, &oy);
    static const u8 numcol[9] = {
        C_BLACK, C_BLUE, C_GREEN, C_RED, C_LBLUE,
        C_MAGENTA, C_CYAN, C_DGRAY, C_LGRAY
    };
    for (int y = 0; y < MS_H; y++) {
        for (int x = 0; x < MS_W; x++) {
            int px = cx + ox + x * cell, py = cy + oy + y * cell;
            if (!ms_shown[x][y]) {
                draw_bevel(px, py, cell - 1, cell - 1, 0);
                if (ms_flag[x][y])
                    draw_text(px + cell / 2 - 4, py + cell / 2 - 8,
                              "F", C_LRED, C_FACE);
            } else {
                fillrect(px, py, cell - 1, cell - 1, C_LGRAY);
                frame(px, py, cell - 1, cell - 1, C_DGRAY);
                if (ms_mine[x][y])
                    draw_text(px + cell / 2 - 4, py + cell / 2 - 8,
                              "*", C_RED, C_LGRAY);
                else {
                    int cnt = ms_neighbors(x, y);
                    if (cnt) {
                        char t[2]; t[0] = (char)('0' + cnt); t[1] = 0;
                        draw_text(px + cell / 2 - 4, py + cell / 2 - 8,
                                  t, numcol[cnt], C_LGRAY);
                    }
                }
            }
        }
    }
    if (ms_over || ms_won)
        draw_text_n(cx + 6, cy + ch - FONT_H,
                    ms_won ? "CLEARED! press R for a new board" :
                             "BOOM! press R to try again",
                    cw / FONT_W, ms_won ? C_LGREEN : C_LRED, C_FACE);
}

/* -------------------------------- 2048 ------------------------------ */
static u32 g4[4][4];
static int g4_score, g4_over;

static void g2048_spawn(void)
{
    int empty[16], n = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (!g4[y][x]) empty[n++] = y * 4 + x;
    if (!n) return;
    rng = rng * 1103515245u + 12345u;
    int pick = empty[(rng >> 16) % (u32)n];
    rng = rng * 1103515245u + 12345u;
    g4[pick / 4][pick % 4] = ((rng >> 8) % 10 == 0) ? 4u : 2u;
}

static void g2048_reset(void)
{
    memset(g4, 0, sizeof(g4));
    g4_score = 0; g4_over = 0;
    g2048_spawn(); g2048_spawn();
}

static int g4_line(u32 *a)
{
    u32 t[4] = {0,0,0,0};
    int out = 0, changed = 0;
    for (int i = 0; i < 4; i++) {
        if (!a[i]) continue;
        if (out && t[out - 1] == a[i]) {
            t[out - 1] *= 2;
            g4_score += (int)t[out - 1];
            changed = 1;
        } else t[out++] = a[i];
    }
    for (int i = 0; i < 4; i++) if (a[i] != t[i]) changed = 1;
    for (int i = 0; i < 4; i++) a[i] = t[i];
    return changed;
}

static void g2048_move(int dir)
{
    if (g4_over) return;
    int changed = 0;
    for (int i = 0; i < 4; i++) {
        u32 line[4];
        for (int j = 0; j < 4; j++) {
            int idx = (dir == 3 || dir == 4) ? 3 - j : j;
            if (dir == 1 || dir == 3) line[j] = g4[i][idx];
            else line[j] = g4[idx][i];
        }
        if (g4_line(line)) changed = 1;
        for (int j = 0; j < 4; j++) {
            int idx = (dir == 3 || dir == 4) ? 3 - j : j;
            if (dir == 1 || dir == 3) g4[i][idx] = line[j];
            else g4[idx][i] = line[j];
        }
    }
    if (!changed) return;
    g2048_spawn();
    int moves = 0;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!g4[y][x]) moves = 1;
            if (x < 3 && g4[y][x] == g4[y][x + 1]) moves = 1;
            if (y < 3 && g4[y][x] == g4[y + 1][x]) moves = 1;
        }
    if (!moves) g4_over = 1;
}

static void g2048_key(u8 k)
{
    if (k == K_LEFT || k == 'a' || k == 'A') g2048_move(1);
    else if (k == K_RIGHT || k == 'd' || k == 'D') g2048_move(3);
    else if (k == K_UP || k == 'w' || k == 'W') g2048_move(2);
    else if (k == K_DOWN || k == 's' || k == 'S') g2048_move(4);
    else if (k == 'r' || k == 'R') g2048_reset();
}

static void g2048_click(int lx, int ly, int cw, int ch)
{
    (void)ch;
    if (ly >= 2 && ly < 20 && lx >= cw - 62 && lx < cw - 6) g2048_reset();
}

static u8 g4_color(u32 v)
{
    switch (v) {
        case 0: return C_FACE2;
        case 2: return C_LGRAY;
        case 4: return C_YELLOW;
        case 8: return C_LRED;
        case 16: return C_LGREEN;
        case 32: return C_LBLUE;
        case 64: return C_MAGENTA;
        default: return C_CYAN;
    }
}

static void draw_2048(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_FACE);
    frame(cx, cy, cw, ch, C_BLACK);
    char s[48];
    int n = 0;
    const char *a = "2048  SCORE "; while (*a) s[n++] = *a++;
    itoa((u32)g4_score, s + n); n += strlen(s + n);
    a = "   R NEW / ARROWS OR WASD"; while (*a) s[n++] = *a++;
    s[n] = 0;
    draw_text_n(cx + 6, cy + 4, s, cw / FONT_W, C_BLACK, C_FACE);
    draw_bevel(cx + cw - 58, cy + 2, 52, 16, 0);
    draw_text(cx + cw - 48, cy + 3, "NEW", C_BLACK, C_FACE);

    int cell = (cw - 24) / 4;
    int c2 = (ch - 54) / 4;
    if (c2 < cell) cell = c2;
    if (cell < 36) cell = 36;
    if (cell > 96) cell = 96;
    int ox = (cw - cell * 4) / 2, oy = 26;
    if (oy + cell * 4 + 4 > ch) oy = ch - cell * 4 - 4;
    if (oy < 24) oy = 24;
    fillrect(cx + ox - 3, cy + oy - 3, cell * 4 + 6, cell * 4 + 6, C_DGRAY);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int px = cx + ox + x * cell, py = cy + oy + y * cell;
            u8 col = g4_color(g4[y][x]);
            draw_bevel(px, py, cell - 4, cell - 4, 0);
            fillrect(px + 2, py + 2, cell - 8, cell - 8, col);
            if (g4[y][x]) {
                char t[12]; itoa(g4[y][x], t);
                int tw = (int)strlen(t) * FONT_W;
                u8 fg = (g4[y][x] <= 4 || g4[y][x] == 64) ? C_BLACK : C_WHITE;
                draw_text(px + (cell - tw) / 2 - 2, py + cell / 2 - 8,
                          t, fg, col);
            }
        }
    if (g4_over)
        draw_text_n(cx + 6, cy + ch - FONT_H, "NO MOVES - press R",
                    cw / FONT_W, C_LRED, C_FACE);
}

/* --------------------------- brick drop ----------------------------- */
#define TET_W 10
#define TET_H 18
static const u8 tet_shape[7][16] = {
    {0,0,0,0, 1,1,1,1, 0,0,0,0, 0,0,0,0}, /* I */
    {0,1,1,0, 0,1,1,0, 0,0,0,0, 0,0,0,0}, /* O */
    {0,0,1,0, 0,1,1,1, 0,0,0,0, 0,0,0,0}, /* T */
    {0,1,1,0, 1,1,0,0, 0,0,0,0, 0,0,0,0}, /* S */
    {1,1,0,0, 0,1,1,0, 0,0,0,0, 0,0,0,0}, /* Z */
    {0,1,0,0, 0,1,0,0, 0,1,1,0, 0,0,0,0}, /* L */
    {1,0,0,0, 1,0,0,0, 1,1,0,0, 0,0,0,0}  /* J */
};
static u8 tet_board[TET_H][TET_W], tet_cur[4][4];
static int tet_piece, tet_rot, tet_x, tet_y, tet_score, tet_lines, tet_over;

static void tet_build(int piece, int rot, u8 out[4][4])
{
    memset(out, 0, 16);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            int nx = x, ny = y;
            for (int r = 0; r < rot; r++) {
                int ox = nx, oy = ny;
                nx = 3 - oy; ny = ox;
            }
            out[ny][nx] = tet_shape[piece][y * 4 + x];
        }
}

static int tet_collide(int px, int py, const u8 cells[4][4])
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!cells[y][x]) continue;
            int gx = px + x, gy = py + y;
            if (gx < 0 || gx >= TET_W || gy >= TET_H) return 1;
            if (gy >= 0 && tet_board[gy][gx]) return 1;
        }
    return 0;
}

static void tet_spawn(void)
{
    rng = rng * 1103515245u + 12345u;
    tet_piece = (int)((rng >> 16) % 7);
    tet_rot = 0; tet_x = 3; tet_y = 0;
    tet_build(tet_piece, tet_rot, tet_cur);
    if (tet_collide(tet_x, tet_y, tet_cur)) tet_over = 1;
}

static void tet_reset(void)
{
    memset(tet_board, 0, sizeof(tet_board));
    tet_score = 0; tet_lines = 0; tet_over = 0;
    tet_spawn();
}

static void tet_merge(void)
{
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) {
            if (!tet_cur[y][x]) continue;
            int gy = tet_y + y, gx = tet_x + x;
            if (gy >= 0 && gy < TET_H && gx >= 0 && gx < TET_W)
                tet_board[gy][gx] = (u8)(tet_piece + 1);
        }
    int cleared = 0;
    for (int y = TET_H - 1; y >= 0; y--) {
        int full = 1;
        for (int x = 0; x < TET_W; x++) if (!tet_board[y][x]) { full = 0; break; }
        if (!full) continue;
        cleared++;
        for (int yy = y; yy > 0; yy--)
            for (int x = 0; x < TET_W; x++)
                tet_board[yy][x] = tet_board[yy - 1][x];
        for (int x = 0; x < TET_W; x++) tet_board[0][x] = 0;
        y++;
    }
    tet_lines += cleared;
    tet_score += cleared * 100 + 10;
    tet_spawn();
}

static void tet_step(void)
{
    if (tet_over) return;
    if (tet_collide(tet_x, tet_y + 1, tet_cur)) tet_merge();
    else tet_y++;
}

static void tet_key(u8 k)
{
    if (k == 'r' || k == 'R') { tet_reset(); return; }
    if (tet_over) return;
    if (k == K_LEFT) {
        if (!tet_collide(tet_x - 1, tet_y, tet_cur)) tet_x--;
    } else if (k == K_RIGHT) {
        if (!tet_collide(tet_x + 1, tet_y, tet_cur)) tet_x++;
    } else if (k == K_DOWN) {
        if (!tet_collide(tet_x, tet_y + 1, tet_cur)) { tet_y++; tet_score++; }
    } else if (k == K_UP) {
        int nr = (tet_rot + 1) & 3;
        u8 next[4][4];
        tet_build(tet_piece, nr, next);
        if (!tet_collide(tet_x, tet_y, next)) { tet_rot = nr; memcpy(tet_cur, next, 16); }
    } else if (k == ' ') {
        while (!tet_collide(tet_x, tet_y + 1, tet_cur)) tet_y++;
        tet_score += 5;
        tet_merge();
    }
}

static void tet_click(int lx, int ly, int cw, int ch)
{
    (void)ch;
    if (ly >= 2 && ly < 20 && lx >= cw - 62 && lx < cw - 6) tet_reset();
}

static u8 tet_color(int id)
{
    static const u8 c[8] = {
        C_FACE2, C_CYAN, C_YELLOW, C_MAGENTA,
        C_LGREEN, C_LRED, C_LBLUE, C_LGRAY
    };
    if (id < 0 || id > 7) return C_FACE2;
    return c[id];
}

static void draw_tetris(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_FACE);
    frame(cx, cy, cw, ch, C_BLACK);
    draw_bevel(cx + cw - 58, cy + 2, 52, 16, 0);
    draw_text(cx + cw - 48, cy + 3, "NEW", C_BLACK, C_FACE);

    int cell = (cw - 150) / TET_W;
    int c2 = (ch - 28) / TET_H;
    if (c2 < cell) cell = c2;
    if (cell < 10) cell = 10;
    if (cell > 28) cell = 28;
    int ox = 6, oy = 24;
    fillrect(cx + ox - 2, cy + oy - 2, TET_W * cell + 4, TET_H * cell + 4, C_BLACK);
    for (int y = 0; y < TET_H; y++)
        for (int x = 0; x < TET_W; x++)
            fillrect(cx + ox + x * cell, cy + oy + y * cell, cell - 1, cell - 1,
                     tet_color(tet_board[y][x]));
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (tet_cur[y][x] && tet_y + y >= 0 && tet_y + y < TET_H) {
                int gy = tet_y + y, gx = tet_x + x;
                if (gx >= 0 && gx < TET_W)
                    fillrect(cx + ox + gx * cell, cy + oy + gy * cell,
                             cell - 1, cell - 1, tet_color(tet_piece + 1));
            }

    int px = ox + TET_W * cell + 12;
    char s[48];
    int n = 0;
    const char *a = "BRICK DROP"; while (*a) s[n++] = *a++;
    s[n] = 0;
    draw_text(cx + px, cy + 24, s, C_BLACK, C_FACE);
    n = 0; a = "SCORE "; while (*a) s[n++] = *a++;
    itoa((u32)tet_score, s + n); n += strlen(s + n); s[n] = 0;
    draw_text(cx + px, cy + 46, s, C_BLACK, C_FACE);
    n = 0; a = "LINES "; while (*a) s[n++] = *a++;
    itoa((u32)tet_lines, s + n); n += strlen(s + n); s[n] = 0;
    draw_text(cx + px, cy + 66, s, C_BLACK, C_FACE);
    draw_text(cx + px, cy + 96, "NEXT:", C_BLACK, C_FACE);
    u8 next[4][4];
    tet_build(tet_piece, tet_rot, next);
    fillrect(cx + px, cy + 116, 64, 64, C_BLACK);
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (next[y][x])
                fillrect(cx + px + 2 + x * 15, cy + 118 + y * 15, 14, 14,
                         tet_color(tet_piece + 1));
    draw_text(cx + px, cy + 190, "LEFT/RIGHT", C_DGRAY, C_FACE);
    draw_text(cx + px, cy + 206, "UP ROTATE", C_DGRAY, C_FACE);
    draw_text(cx + px, cy + 222, "DOWN SOFT", C_DGRAY, C_FACE);
    draw_text(cx + px, cy + 238, "SPACE DROP", C_DGRAY, C_FACE);
    if (tet_over)
        draw_text(cx + px, cy + 264, "GAME OVER", C_LRED, C_FACE);
}

/* ---------------------------- calculator ---------------------------- */
static char calc_entry[20] = "0";
static int calc_len = 1;
static u32 calc_acc = 0, calc_result = 0;
static int calc_op = 0, calc_new = 1, calc_err = 0;

static void calc_set(const char *s)
{
    int i = 0;
    for (; s[i] && i < 19; i++) calc_entry[i] = s[i];
    calc_entry[i] = 0;
    calc_len = i;
}
static u32 calc_value(void) { return atoi_local(calc_entry); }
static void calc_apply(void)
{
    u32 v = calc_value();
    if (calc_err) return;
    if (calc_op == 1) calc_result = calc_acc + v;
    else if (calc_op == 2) calc_result = calc_acc - v;
    else if (calc_op == 3) calc_result = calc_acc * v;
    else if (calc_op == 4) {
        if (!v) { calc_err = 1; calc_set("ERR"); return; }
        calc_result = calc_acc / v;
    } else calc_result = v;
    char s[16]; itoa(calc_result, s); calc_set(s);
}
static void calc_choose(int op)
{
    if (calc_op && !calc_new && !calc_err) calc_apply();
    else if (!calc_err) calc_result = calc_value();
    calc_acc = calc_result;
    calc_op = op;
    calc_new = 1;
    calc_err = 0;
}
static void calc_equals(void)
{
    if (calc_err) return;
    if (calc_op) calc_apply();
    else { calc_result = calc_value(); char s[16]; itoa(calc_result, s); calc_set(s); }
    calc_op = 0; calc_new = 1;
}
static void calc_clear(void)
{
    calc_set("0"); calc_acc = calc_result = 0;
    calc_op = 0; calc_new = 1; calc_err = 0;
}
static void calc_key(u8 k)
{
    if (k >= '0' && k <= '9') {
        /* Starting a new operand must not discard the pending operator or
           accumulator; calc_clear() is only for an explicit C/ESC reset. */
        if (calc_new) { calc_set("0"); calc_new = 0; }
        else if (calc_err) { calc_clear(); calc_new = 0; }
        if (calc_len < 19) {
            if (calc_len == 1 && calc_entry[0] == '0') calc_len = 0;
            calc_entry[calc_len++] = (char)k;
            calc_entry[calc_len] = 0;
        }
    } else if (k == '+') calc_choose(1);
    else if (k == '-') calc_choose(2);
    else if (k == '*') calc_choose(3);
    else if (k == '/') calc_choose(4);
    else if (k == K_ENTER || k == '=') calc_equals();
    else if (k == K_BACK) {
        if (calc_len > 1) calc_entry[--calc_len] = 0;
        else calc_set("0");
    } else if (k == K_ESC || k == 'c' || k == 'C') calc_clear();
}
static int calc_button_at(int lx, int ly, int cw, int ch)
{
    int gx = 8, gy = 36;
    int bw = (cw - 16) / 4, bh = (ch - gy - 8) / 4;
    if (bw < 32 || bh < 20) return -1;
    int col = (lx - gx) / bw, row = (ly - gy) / bh;
    if (col < 0 || col > 3 || row < 0 || row > 3) return -1;
    if (lx >= gx + col * bw + bw || ly >= gy + row * bh + bh) return -1;
    return row * 4 + col;
}
static void calc_click(int lx, int ly, int cw, int ch)
{
    int b = calc_button_at(lx, ly, cw, ch);
    if (b < 0) return;
    static const char keys[16] = {'7','8','9','/','4','5','6','*','1','2','3','-','C','0','=','+'};
    calc_key((u8)keys[b]);
}
static void draw_calc(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_DGRAY);
    frame(cx, cy, cw, ch, C_BLACK);
    int cols = cw / FONT_W;
    if (cols < 1) cols = 1;

    fillrect(cx + 6, cy + 5, cw - 12, 25, C_BLACK);
    frame(cx + 6, cy + 5, cw - 12, 25, C_HILITE);
    draw_text_n(cx + cw - 12 - (int)strlen(calc_entry) * FONT_W - 4, cy + 10,
                calc_entry, cols, calc_err ? C_LRED : C_LGREEN, C_BLACK);

    static const char *labels[16] = {
        "7","8","9","/","4","5","6","*","1","2","3","-","C","0","=","+"
    };
    int gx = 8, gy = 36;
    int bw = (cw - 16) / 4, bh = (ch - gy - 8) / 4;
    if (bw < 24) bw = 24;
    if (bh < 16) bh = 16;
    for (int i = 0; i < 16; i++) {
        int x = cx + gx + (i % 4) * bw;
        int y = cy + gy + (i / 4) * bh;
        draw_bevel(x, y, bw - 3, bh - 3, 0);
        int tw = (int)strlen(labels[i]) * FONT_W;
        draw_text(x + (bw - 3 - tw) / 2, y + (bh - 3 - FONT_H) / 2,
                  labels[i], C_BLACK, C_FACE);
    }
    (void)cols;
}

/* ---------------------------- file manager -------------------------- */
static int fm_sel = 0, fm_scroll = 0;
static int fm_used_idx[FS_MAX_FILES];
static int fm_count(void)
{
    int n = 0;
    for (int i = 0; i < FS_MAX_FILES; i++)
        if (fs_dir[i].used) fm_used_idx[n++] = i;
    return n;
}
static void fm_open(int idx)
{
    int n = fm_count();
    if (idx < 0 || idx >= n) return;
    FSEnt *e = &fs_dir[fm_used_idx[idx]];
    if (note_load(e->name)) open_win(2);
}
static int fm_button_at(int lx, int ly, int cw, int ch)
{
    int y = ch - FONT_H - 10;
    if (ly < y || ly >= y + FONT_H + 6) return -1;
    int bw = (cw - 20) / 4;
    int col = (lx - 10) / bw;
    if (col < 0 || col > 3 || lx >= 10 + col * bw + bw - 4) return -1;
    return col;
}
static void fm_click(int lx, int ly, int cw, int ch)
{
    int b = fm_button_at(lx, ly, cw, ch);
    if (b >= 0) {
        if (b == 0) fm_open(fm_sel);
        else if (b == 1) { int n = fm_count(); if (fm_sel < n) fs_delete(fs_dir[fm_used_idx[fm_sel]].name); }
        else if (b == 2) fs_format();
        fm_sel = 0; fm_scroll = 0;
        return;
    }
    int row = (ly - 6) / FONT_H;
    if (ly >= 6 && row >= 0) {
        int n = fm_count();
        if (fm_scroll + row < n) fm_sel = fm_scroll + row;
    }
}
static void draw_fm(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_WHITE);
    frame(cx, cy, cw, ch, C_BLACK);
    int cols = cw / FONT_W;
    if (cols < 1) cols = 1;
    draw_text_n(cx + 4, cy + 2, "PAPEROS FILES  (disk.img)", cols,
                C_WHITE, accent);

    int n = fm_count();
    int rows = (ch - FONT_H - 34) / FONT_H;
    if (rows < 1) rows = 1;
    if (fm_sel < fm_scroll) fm_scroll = fm_sel;
    if (fm_sel >= fm_scroll + rows) fm_scroll = fm_sel - rows + 1;
    if (fm_scroll < 0) fm_scroll = 0;

    for (int r = 0; r < rows && r + fm_scroll < n; r++) {
        int idx = fm_used_idx[fm_scroll + r];
        char s[48];
        int k = 0;
        for (int j = 0; j < 16 && fs_dir[idx].name[j]; j++) s[k++] = fs_dir[idx].name[j];
        while (k < 18) s[k++] = ' ';
        itoa(fs_dir[idx].bytes, s + k); k += strlen(s + k);
        while (k < 32) s[k++] = ' ';
        itoa(fs_dir[idx].lba, s + k); k += strlen(s + k);
        s[k] = 0;
        int sel = (fm_scroll + r == fm_sel);
        fillrect(cx + 3, cy + 20 + r * FONT_H, cw - 6, FONT_H,
                 sel ? accent : C_WHITE);
        draw_text_n(cx + 5, cy + 20 + r * FONT_H, s, cols,
                    sel ? C_WHITE : C_BLACK, sel ? accent : C_WHITE);
    }
    if (!n) draw_text_n(cx + 5, cy + 22, "(empty - use terminal 'touch' or Format)", cols, C_DGRAY, C_WHITE);

    static const char *blab[4] = {"Open", "Delete", "Format", "Refresh"};
    int bw = (cw - 20) / 4;
    int by = ch - FONT_H - 10;
    for (int i = 0; i < 4; i++) {
        int x = 10 + i * bw;
        draw_bevel(cx + x, cy + by, bw - 4, FONT_H + 6, 0);
        draw_text(cx + x + 8, cy + by + 2, blab[i], C_BLACK, C_FACE);
    }
}

/* -------------------------- system monitor -------------------------- */
static void draw_monitor(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_BLACK);
    frame(cx, cy, cw, ch, C_DGRAY);
    int cols = cw / FONT_W;
    if (cols < 1) cols = 1;
    int y = cy + 8;
    u32 up = pit_ms() / 1000;

    draw_text_n(cx + 8, y, "PAPEROS SYSTEM MONITOR", cols, C_YELLOW, C_BLACK); y += FONT_H + 8;

    char s[80];
    int n = 0;
    const char *a = "Uptime: "; while (*a) s[n++] = *a++;
    itoa(up / 60, s + n); n += strlen(s + n);
    s[n++] = 'm'; s[n++] = ' ';
    itoa(up % 60, s + n); n += strlen(s + n);
    s[n++] = 's'; s[n] = 0;
    draw_text_n(cx + 8, y, s, cols, C_LGREEN, C_BLACK); y += FONT_H + 3;

    n = 0; a = "Video: "; while (*a) s[n++] = *a++;
    itoa(VW, s + n); n += strlen(s + n); s[n++] = 'x';
    itoa(VH, s + n); n += strlen(s + n);
    s[n++] = ' '; s[n++] = 'p'; s[n++] = 'i'; s[n++] = 't'; s[n++] = 'c'; s[n++] = 'h';
    s[n++] = ' '; itoa(VPITCH, s + n); n += strlen(s + n); s[n] = 0;
    draw_text_n(cx + 8, y, s, cols, C_LGREEN, C_BLACK); y += FONT_H + 3;

    n = 0; a = "CPU: 32-bit x86 / polled PIT, keyboard, mouse, NE2000";
    while (*a) s[n++] = *a++;
    s[n] = 0;
    draw_text_n(cx + 8, y, s, cols, C_LGREEN, C_BLACK); y += FONT_H + 3;

    n = 0; a = "Windows: "; while (*a) s[n++] = *a++;
    itoa((u32)nwin, s + n); n += strlen(s + n);
    a = " / Storage: "; while (*a) s[n++] = *a++;
    if (fs_ready) { itoa(fs_used_sectors(), s + n); n += strlen(s + n); a = " sectors used"; }
    else { a = "not formatted"; }
    while (*a) s[n++] = *a++;
    s[n] = 0;
    draw_text_n(cx + 8, y, s, cols, C_LGREEN, C_BLACK); y += FONT_H + 3;

    n = 0; a = "Network: "; while (*a) s[n++] = *a++;
    if (net_up) {
        a = "UP  IP "; while (*a) s[n++] = *a++;
        ip_str(net_ip, s + n); n += strlen(s + n);
    } else { a = "DOWN (type net in terminal)"; while (*a) s[n++] = *a++; }
    s[n] = 0;
    draw_text_n(cx + 8, y, s, cols, net_up ? C_LGREEN : C_LRED, C_BLACK); y += FONT_H + 8;

    draw_text_n(cx + 8, y, "ACTIVITY", cols, C_DGRAY, C_BLACK); y += FONT_H + 2;
    int bars = 16;
    int bw = (cw - 24) / bars;
    if (bw < 2) bw = 2;
    for (int i = 0; i < bars; i++) {
        int h = 6 + (int)(((pit_ms() / 80u) + (u32)i * 7u) % 13u);
        fillrect(cx + 8 + i * bw, y + 18 - h, bw - 2, h,
                 i % 3 == 0 ? C_YELLOW : C_LGREEN);
    }
    (void)ch;
}

/* ----------------------------- settings ----------------------------- */
static void settings_swatch_size(int cw, int *sw, int *step)
{
    int w = 22 + cw / 24;
    if (w < 22) w = 22;
    if (w > 70) w = 70;
    *sw = w;
    *step = w + 8;
}
static int settings_tab_w(int cw)
{
    int w = (cw - 12) / 4;
    if (w < 42) w = 42;
    return w;
}

static void settings_defaults(void)
{
    wall_color = RGB(0,0,3);
    accent = C_BLUE;
    show_wallpaper = 1;
    flat_ui = 0;
    build_desktop();
    damage_all();
}

static void settings_click(int lx, int ly, int cw, int ch)
{
    (void)ch;
    int tw = settings_tab_w(cw);
    if (ly >= 3 && ly < 24) {
        int t = (lx - 6) / tw;
        if (t >= 0 && t < 4 && lx < 6 + tw * 4) {
            settings_tab = t;
            damage_all();
        }
        return;
    }

    if (settings_tab == 0) {
        int sw, step;
        settings_swatch_size(cw, &sw, &step);
        if (ly >= 48 && ly < 48 + sw) {
            int i = (lx - 8) / step;
            if (i >= 0 && i < 8 && lx >= 8 + i * step && lx < 8 + i * step + sw &&
                wall_color != (u8)i) {
                wall_color = (u8)i;
                build_desktop();
                damage_all();
            }
            return;
        }
        int ay = 70 + sw;
        if (ly >= ay && ly < ay + sw) {
            int i = (lx - 8) / step;
            if (i >= 0 && i < 8 && lx >= 8 + i * step && lx < 8 + i * step + sw) {
                accent = (u8)(i + 8);
                damage_all();
            }
            return;
        }
        int by = 86 + 2 * sw;
        int bw = (cw - 20) / 2;
        if (ly >= by && ly < by + 18) {
            if (lx >= 8 && lx < 8 + bw) { show_wallpaper = !show_wallpaper; build_desktop(); damage_all(); }
            else if (lx >= 12 + bw && lx < 12 + bw * 2) { flat_ui = !flat_ui; damage_all(); }
            return;
        }
        if (ly >= by + 24 && ly < by + 44 && lx >= 8 && lx < 150) {
            settings_defaults();
        }
    } else if (settings_tab == 2) {
        int by = 152;
        if (ly >= by && ly < by + 20) {
            if (lx >= 8 && lx < 104) net_init();
            else if (lx >= 112 && lx < 208) {
                char s[40];
                if (net_up) { ip_str(net_gw, s); net_cmd_ping(s); }
                else term_push("NET: down - use DHCP first");
            }
            damage_all();
        }
    }
}

static void draw_settings(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_LGRAY);
    frame(cx, cy, cw, ch, C_BLACK);
    int cols = cw / FONT_W;
    if (cols < 1) cols = 1;

    static const char *tabs[4] = {"LOOK", "SYSTEM", "NETWORK", "ABOUT"};
    int tw = settings_tab_w(cw);
    for (int i = 0; i < 4; i++)
        draw_bevel(cx + 6 + i * tw, cy + 3, tw - 2, 20, i == settings_tab);
    for (int i = 0; i < 4; i++)
        draw_text_n(cx + 6 + i * tw + 6, cy + 8, tabs[i],
                    (tw - 8) / FONT_W, C_BLACK, C_FACE);
    hline(cx + 3, cy + 26, cw - 6, C_SHADOW);

    char s[128];
    int n, y;
    const char *a;

    if (settings_tab == 0) {
        int sw, step;
        settings_swatch_size(cw, &sw, &step);
        draw_text_n(cx + 8, cy + 32, "Desktop background", cols, C_BLACK, C_LGRAY);
        for (int i = 0; i < 8; i++) {
            int x = cx + 8 + i * step;
            if (x + sw > cx + cw) break;
            fillrect(x, cy + 48, sw, sw, (u8)i);
            frame(x, cy + 48, sw, sw, wall_color == (u8)i ? C_WHITE : C_BLACK);
        }
        draw_text_n(cx + 8, cy + 54 + sw, "Window accent", cols, C_BLACK, C_LGRAY);
        for (int i = 0; i < 8; i++) {
            int x = cx + 8 + i * step;
            if (x + sw > cx + cw) break;
            fillrect(x, cy + 70 + sw, sw, sw, (u8)(i + 8));
            frame(x, cy + 70 + sw, sw, sw, accent == (u8)(i + 8) ? C_WHITE : C_BLACK);
        }
        int by = 86 + 2 * sw, bw = (cw - 20) / 2;
        draw_bevel(cx + 8, cy + by, bw, 18, !show_wallpaper);
        draw_text_n(cx + 14, cy + by + 2, show_wallpaper ? "WALL: ON" : "WALL: OFF",
                    bw / FONT_W, C_BLACK, C_FACE);
        draw_bevel(cx + 12 + bw, cy + by, bw, 18, flat_ui);
        draw_text_n(cx + 18 + bw, cy + by + 2, flat_ui ? "EDGES: FLAT" : "EDGES: 3D",
                    bw / FONT_W, C_BLACK, C_FACE);
        draw_bevel(cx + 8, cy + by + 24, 142, 20, 0);
        draw_text(cx + 22, cy + by + 28, "RESTORE DEFAULTS", C_BLACK, C_FACE);
    } else if (settings_tab == 1) {
        y = cy + 34;
        draw_text_n(cx + 8, y, "PAPEROS 0.9 - 32-bit protected mode", cols, C_BLACK, C_LGRAY); y += 18;
        n = 0; a = "Video: "; while (*a) s[n++] = *a++;
        itoa(VW, s + n); n += strlen(s + n); s[n++] = 'x';
        itoa(VH, s + n); n += strlen(s + n); s[n++] = ' ';
        a = "pitch "; while (*a) s[n++] = *a++;
        itoa(VPITCH, s + n); n += strlen(s + n); s[n] = 0;
        draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
        n = 0; a = "Memory: 64 MiB RAM / 8-bit LFB / tiled desktop";
        while (*a) s[n++] = *a++;
        s[n] = 0;
        draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
        n = 0; a = "Uptime: "; while (*a) s[n++] = *a++;
        itoa(pit_ms() / 1000, s + n); n += strlen(s + n); s[n++] = 's'; s[n] = 0;
        draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
        n = 0; a = "Windows: "; while (*a) s[n++] = *a++;
        itoa((u32)nwin, s + n); n += strlen(s + n); s[n++] = '/';
        a = "12 max"; while (*a) s[n++] = *a++;
        s[n] = 0;
        draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
        n = 0; a = "PAPEROSFS: "; while (*a) s[n++] = *a++;
        if (fs_ready) { itoa(fs_used_sectors(), s + n); n += strlen(s + n); a = " used sectors"; }
        else { a = "not formatted"; }
        while (*a) s[n++] = *a++;
        s[n] = 0;
        draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
        draw_text_n(cx + 8, y, "Input: PS/2 keyboard + PS/2 mouse", cols, C_BLACK, C_LGRAY);
    } else if (settings_tab == 2) {
        y = cy + 34;
        n = 0; a = "Status: "; while (*a) s[n++] = *a++;
        a = net_up ? "UP via NE2000 PCI" : "DOWN";
        while (*a) s[n++] = *a++;
        s[n] = 0;
        draw_text_n(cx + 8, y, s, cols, net_up ? C_GREEN : C_RED, C_LGRAY); y += 18;
        if (net_up) {
            n = 0; a = "IP: "; while (*a) s[n++] = *a++;
            ip_str(net_ip, s + n); n += strlen(s + n); s[n] = 0;
            draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
            n = 0; a = "Gateway: "; while (*a) s[n++] = *a++;
            ip_str(net_gw, s + n); n += strlen(s + n); s[n] = 0;
            draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
            n = 0; a = "DNS: "; while (*a) s[n++] = *a++;
            ip_str(net_dns, s + n); n += strlen(s + n); s[n] = 0;
            draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY); y += 18;
            n = 0; a = "MAC: "; while (*a) s[n++] = *a++;
            for (int i = 0; i < 6; i++) {
                char h[3]; hex_byte(net_mac[i], h);
                s[n++] = h[0]; s[n++] = h[1];
                if (i < 5) s[n++] = ':';
            }
            s[n] = 0;
            draw_text_n(cx + 8, y, s, cols, C_BLACK, C_LGRAY);
        }
        draw_bevel(cx + 8, cy + 152, 96, 20, 0);
        draw_text(cx + 18, cy + 156, "DHCP", C_BLACK, C_FACE);
        draw_bevel(cx + 112, cy + 152, 96, 20, 0);
        draw_text(cx + 120, cy + 156, "PING GW", C_BLACK, C_FACE);
    } else {
        draw_text_n(cx + 8, cy + 36, "PAPEROS 0.9 - made from scratch", cols, C_BLACK, C_LGRAY);
        draw_text_n(cx + 8, cy + 58, "Click a window to bring it forward.", cols, C_DGRAY, C_LGRAY);
        draw_text_n(cx + 8, cy + 76, "Title buttons: minimise, send behind, close.", cols, C_DGRAY, C_LGRAY);
        draw_text_n(cx + 8, cy + 94, "Games: Minesweeper, 2048, Brick Drop, Snake.", cols, C_DGRAY, C_LGRAY);
        draw_text_n(cx + 8, cy + 112, "Web: real DNS + TCP + HTTP on NE2000.", cols, C_DGRAY, C_LGRAY);
        draw_text_n(cx + 8, cy + 130, "Terminal: type help for the full command list.", cols, C_DGRAY, C_LGRAY);
    }
    (void)ch;
}

/* ------------------------ app dispatchers --------------------------- */
static void draw_content(int i, int cx, int cy, int cw, int ch)
{
    switch (wins[i].type) {
        case 0: draw_terminal(cx, cy, cw, ch); break;
        case 1: draw_paint(cx, cy, cw, ch); break;
        case 2: draw_notepad(cx, cy, cw, ch); break;
        case 3: draw_snake(cx, cy, cw, ch); break;
        case 4: draw_settings(cx, cy, cw, ch); break;
        case 5: draw_fm(cx, cy, cw, ch); break;
        case 6: draw_calc(cx, cy, cw, ch); break;
        case 7: draw_monitor(cx, cy, cw, ch); break;
        case 8: draw_mines(cx, cy, cw, ch); break;
        case 9: draw_2048(cx, cy, cw, ch); break;
        case 10: draw_tetris(cx, cy, cw, ch); break;
        case 11: draw_browser(cx, cy, cw, ch); break;
    }
}
static void app_key(int i, u8 k)
{
    switch (wins[i].type) {
        case 0: term_key(k); break;
        case 1:
            if (k == 'c' || k == 'C') {
                memset(canvas, 0, (u32)PCW * PCH);
                mark_content(i);
            }
            break;
        case 2: note_key(k); break;
        case 3: snake_key(k); break;
        case 6: calc_key(k); break;
        case 8: ms_key(k); break;
        case 9: g2048_key(k); break;
        case 10: tet_key(k); break;
        case 11: web_key(k); break;
    }
}
static void app_click(int i, int lx, int ly, int cw, int ch)
{
    if (wins[i].type == 1) paint_click(lx, ly, cw, ch);
    else if (wins[i].type == 2) note_click(lx, ly, cw, ch);
    else if (wins[i].type == 4) settings_click(lx, ly, cw, ch);
    else if (wins[i].type == 5) fm_click(lx, ly, cw, ch);
    else if (wins[i].type == 6) calc_click(lx, ly, cw, ch);
    else if (wins[i].type == 8) ms_click(lx, ly, cw, ch);
    else if (wins[i].type == 9) g2048_click(lx, ly, cw, ch);
    else if (wins[i].type == 10) tet_click(lx, ly, cw, ch);
    else if (wins[i].type == 11) web_click(lx, ly, cw, ch);
}
static void app_mouse(int i, int cx, int cy, int cw, int ch)
{
    if (wins[i].type == 1)
        paint_mouse(mouse_x, mouse_y, mouse_btns, cx, cy, cw, ch);
}

/* ============================== NETWORK ============================== */
/* Minimal NE2000 PCI driver + DHCP client + ICMP echo. Everything is
   polled; the kernel does not use interrupts. */

#define ETHERTYPE_IP 0x0800
#define DHCP_XID     0x12345678u
#define DHCP_COOKIE  0x63825363u

static u16 net_base = 0;
static u16 ping_seq = 0, ping_id = 0x1234;
static volatile int ping_done = 0;
static u32 ping_target = 0;
static u8 rxbuf[1600], txbuf[600];

static u16 get16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 get32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void put16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void put32(u8 *p, u32 v)
{
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16);
    p[2] = (u8)(v >> 8);  p[3] = (u8)v;
}

static u16 ip_checksum(const u8 *d, u32 n)
{
    u32 sum = 0;
    for (u32 i = 0; i + 1 < n; i += 2)
        sum += ((u32)d[i] << 8) | d[i + 1];
    if (n & 1) sum += (u32)d[n - 1] << 8;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static void hex_byte(u8 v, char *out)
{
    static const char digits[] = "0123456789ABCDEF";
    out[0] = digits[v >> 4];
    out[1] = digits[v & 0x0F];
    out[2] = 0;
}

static void ip_str(u32 ip, char *out)
{
    char tmp[5];
    int n = 0;
    itoa((ip >> 24) & 0xFF, tmp);
    for (int i = 0; tmp[i]; i++) out[n++] = tmp[i];
    out[n++] = '.';
    itoa((ip >> 16) & 0xFF, tmp);
    for (int i = 0; tmp[i]; i++) out[n++] = tmp[i];
    out[n++] = '.';
    itoa((ip >> 8) & 0xFF, tmp);
    for (int i = 0; tmp[i]; i++) out[n++] = tmp[i];
    out[n++] = '.';
    itoa(ip & 0xFF, tmp);
    for (int i = 0; tmp[i]; i++) out[n++] = tmp[i];
    out[n] = 0;
}

static u32 ip_parse(const char *s)
{
    u32 parts[4] = {0, 0, 0, 0};
    int idx = 0, val = 0, got = 0;
    while (*s && idx < 4) {
        if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            got = 1;
        } else if (*s == '.') {
            if (!got) return 0;
            parts[idx++] = (u32)val;
            val = 0; got = 0;
        } else return 0;
        s++;
    }
    if (!got || idx != 3) return 0;
    parts[3] = (u32)val;
    return (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
}

/* ---------------------------- PCI probe ----------------------------- */
static u32 pci_read(u8 dev, u8 reg)
{
    u32 addr = 0x80000000u | ((u32)dev << 11) | (reg & 0xFC);
    outl(0xCF8, addr);
    return inl(0xCFC);
}
static void pci_write(u8 dev, u8 reg, u32 val)
{
    u32 addr = 0x80000000u | ((u32)dev << 11) | (reg & 0xFC);
    outl(0xCF8, addr);
    outl(0xCFC, val);
}

static int pci_find_ne2k(void)
{
    for (u8 dev = 0; dev < 32; dev++) {
        if (pci_read(dev, 0x00) != 0x802910ECu) continue; /* RTL8029AS */
        u32 bar = pci_read(dev, 0x10);
        if (!(bar & 1)) continue;                        /* need I/O BAR */
        net_base = (u16)(bar & 0xFFFCu);
        pci_write(dev, 0x04, (pci_read(dev, 0x04) & 0xFFFFFF00u) | 0x05);
        serial_puts("PAPEROS NET: ne2k_pci at 0x");
        {
            char h[3];
            hex_byte((u8)(net_base >> 8), h); serial_puts(h);
            hex_byte((u8)net_base, h); serial_puts(h);
        }
        serial_puts("\n");
        return 1;
    }
    return 0;
}

/* ------------------------- NE2000 registers ------------------------- */
#define NE_CR    0x00
#define NE_ISR   0x07
#define NE_DATA  0x10
#define NE_RESET 0x1F

static void ne2k_reset(void)
{
    outb(net_base + NE_RESET, inb(net_base + NE_RESET));
    u32 t0 = pit_ms();
    while (!(inb(net_base + NE_ISR) & 0x80) && pit_ms() - t0 < 20) {}
    outb(net_base + NE_ISR, 0xFF);
}

static void ne2k_read_mac(void)
{
    u8 prom[32];
    outb(net_base + NE_CR, 0x21);
    outb(net_base + 0x0E, 0x48);
    outb(net_base + 0x0A, 32);
    outb(net_base + 0x0B, 0);
    outb(net_base + 0x08, 0);
    outb(net_base + 0x09, 0);
    outb(net_base + NE_CR, 0x0A);       /* remote read + start */
    for (int i = 0; i < 32; i++) prom[i] = inb(net_base + NE_DATA);
    outb(net_base + NE_ISR, 0x40);

    /* Some clones return doubled bytes in 16-bit mode. */
    int doubled = prom[0] == prom[1] && prom[2] == prom[3] && prom[4] == prom[5];
    for (int i = 0; i < 6; i++)
        net_mac[i] = doubled ? prom[i * 2] : prom[i];
}

static void ne2k_init(void)
{
    ne2k_reset();
    outb(net_base + NE_CR, 0x21);       /* page 0, stop, no DMA */
    outb(net_base + 0x0E, 0x48);        /* 8-bit data configuration */
    outb(net_base + 0x0A, 0);           /* remote byte count */
    outb(net_base + 0x0B, 0);
    outb(net_base + 0x0F, 0);           /* interrupts off */
    outb(net_base + 0x0C, 0x20);        /* monitor mode while configuring */
    outb(net_base + 0x0D, 0x02);        /* loopback while configuring */
    outb(net_base + 0x04, 0x40);        /* transmit page 0x40 */
    outb(net_base + 0x01, 0x50);        /* receive ring starts at 0x50 */
    outb(net_base + 0x03, 0x7F);        /* boundary one page behind */
    outb(net_base + 0x02, 0x80);        /* receive ring ends at 0x80 */

    outb(net_base + NE_CR, 0x61);       /* page 1, stop, no DMA */
    for (int i = 0; i < 6; i++) outb(net_base + 1 + i, net_mac[i]);
    outb(net_base + 0x07, 0x50);        /* current receive page */
    outb(net_base + NE_CR, 0x21);       /* back to page 0 */

    outb(net_base + NE_ISR, 0xFF);
    outb(net_base + NE_CR, 0x22);       /* page 0, start, no DMA */
    outb(net_base + 0x0D, 0x00);        /* normal transmit */
    outb(net_base + 0x0C, 0x0F);        /* promiscuous: easiest for DHCP */
}

static void ne2k_send(const u8 *pkt, u16 len)
{
    if (len < 60) len = 60;
    outb(net_base + NE_CR, 0x22);
    outb(net_base + NE_ISR, 0x40);      /* clear remote-DMA done */
    outb(net_base + 0x0A, (u8)(len & 0xFF));
    outb(net_base + 0x0B, (u8)(len >> 8));
    outb(net_base + 0x08, 0x00);        /* NIC RAM 0x4000 */
    outb(net_base + 0x09, 0x40);
    outb(net_base + NE_CR, 0x12);       /* remote write + start */
    for (u16 i = 0; i < len; i++) outb(net_base + NE_DATA, pkt[i]);
    u32 t0 = pit_ms();
    while (!(inb(net_base + NE_ISR) & 0x40) && pit_ms() - t0 < 10) {}
    outb(net_base + NE_ISR, 0x40);
    outb(net_base + 0x05, (u8)(len & 0xFF));
    outb(net_base + 0x06, (u8)(len >> 8));
    outb(net_base + 0x04, 0x40);
    outb(net_base + NE_CR, 0x26);       /* transmit + start, no DMA */
}

static void ne2k_read_chunk(u16 addr, u8 *buf, u16 len)
{
    outb(net_base + NE_CR, 0x22);
    outb(net_base + 0x0A, (u8)(len & 0xFF));
    outb(net_base + 0x0B, (u8)(len >> 8));
    outb(net_base + 0x08, (u8)(addr & 0xFF));
    outb(net_base + 0x09, (u8)(addr >> 8));
    outb(net_base + NE_CR, 0x0A);       /* remote read + start */
    for (u16 i = 0; i < len; i++) buf[i] = inb(net_base + NE_DATA);
    outb(net_base + NE_ISR, 0x40);
}

static void ne2k_remote_read(u16 addr, u8 *buf, u16 len)
{
    u16 first = (u16)(0x8000 - addr);
    if (first > len) first = len;
    ne2k_read_chunk(addr, buf, first);
    if (first < len) ne2k_read_chunk(0x5000, buf + first, (u16)(len - first));
}

static int ne2k_recv(u8 *pkt, u16 *outlen)
{
    if (!net_base) return 0;
    outb(net_base + NE_CR, 0x22);

    outb(net_base + NE_CR, 0x62);       /* page 1, start, no DMA */
    u8 cur = inb(net_base + 0x07);      /* CURR */
    outb(net_base + NE_CR, 0x22);
    u8 bn = inb(net_base + 0x03);       /* BNRY */
    u8 thisp = (u8)(bn + 1);
    if (thisp >= 0x80) thisp = 0x50;
    if (thisp == cur) return 0;         /* ring empty */

    u8 hdr[4];
    ne2k_remote_read((u16)(thisp << 8), hdr, 4);
    u16 count = (u16)(hdr[2] | (hdr[3] << 8));
    u16 len = (u16)(count - 4);
    if (len < 60 || len > 1514) {
        outb(net_base + 0x03, (u8)(hdr[1] - 1));
        outb(net_base + NE_ISR, 0x01);
        return 0;
    }
    ne2k_remote_read((u16)((thisp << 8) + 4), pkt, len);
    outb(net_base + 0x03, (u8)(hdr[1] - 1));
    outb(net_base + NE_ISR, 0x01);
    *outlen = len;
    return 1;
}

/* ------------------------------ DHCP ------------------------------- */
static u16 dhcp_build(u8 type, u32 requested_ip, u32 server_ip)
{
    const u16 eth = 14, iph = 20, udph = 8, bootp = 236;
    u8 *p = txbuf;
    u8 *b = p + eth + iph + udph;
    u8 *o = b + bootp + 4;             /* options start after the 4-byte cookie */
    memset(p, 0, 600);

    for (int i = 0; i < 6; i++) p[i] = 0xFF;
    for (int i = 0; i < 6; i++) p[6 + i] = net_mac[i];
    put16(p + 12, ETHERTYPE_IP);

    u16 dhcp_opts = 0;
    o[dhcp_opts++] = 53; o[dhcp_opts++] = 1; o[dhcp_opts++] = type;
    o[dhcp_opts++] = 61; o[dhcp_opts++] = 7; o[dhcp_opts++] = 1;
    for (int i = 0; i < 6; i++) o[dhcp_opts++] = net_mac[i];
    o[dhcp_opts++] = 55; o[dhcp_opts++] = 3;
    o[dhcp_opts++] = 1; o[dhcp_opts++] = 3; o[dhcp_opts++] = 6;
    if (type == 3) {
        o[dhcp_opts++] = 50; o[dhcp_opts++] = 4;
        put32(o + dhcp_opts, requested_ip); dhcp_opts += 4;
        o[dhcp_opts++] = 54; o[dhcp_opts++] = 4;
        put32(o + dhcp_opts, server_ip); dhcp_opts += 4;
    }
    o[dhcp_opts++] = 255;

    b[0] = 1; b[1] = 1; b[2] = 6;
    put32(b + 4, DHCP_XID);
    put16(b + 10, 0x8000);
    for (int i = 0; i < 6; i++) b[28 + i] = net_mac[i];
    put32(b + 236, DHCP_COOKIE);       /* BOOTP fixed area is 236 bytes */

    u16 udp_len = (u16)(udph + bootp + 4 + dhcp_opts);
    u16 total = (u16)(iph + udp_len);
    p[14] = 0x45;                      /* IPv4, 5-word header */
    put16(p + 14 + 2, total);
    p[14 + 4] = 0x12; p[14 + 5] = 0x34;
    p[14 + 8] = 64;   p[14 + 9] = 17;
    put32(p + 14 + 12, 0);
    put32(p + 14 + 16, 0xFFFFFFFFu);
    put16(p + 14 + 10, ip_checksum(p + 14, iph));

    put16(p + 34, 68);
    put16(p + 36, 67);
    put16(p + 38, udp_len);
    put16(p + 40, 0);

    return (u16)(eth + total);
}

static int parse_dhcp(const u8 *p, u16 len, u8 want_type,
                      u32 *yi, u32 *server, u32 *mask, u32 *gw, u32 *dns)
{
    if (len < 42 + 236 + 4) return 0;
    if (get16(p + 12) != ETHERTYPE_IP) return 0;
    const u8 *ip = p + 14;
    if ((ip[0] & 0x0F) != 5 || ip[9] != 17) return 0;
    const u8 *udp = p + 34;
    if (get16(udp) != 67 || get16(udp + 2) != 68) return 0;
    const u8 *b = p + 42;
    if (b[0] != 2 || get32(b + 4) != DHCP_XID) return 0;
    if (get32(b + 236) != DHCP_COOKIE) return 0;

    u8 type = 0;
    u16 off = 240;
    while (off + 2 <= len - 42 && b[off] != 255) {
        u8 code = b[off++];
        u8 n = b[off++];
        if (off + n > len - 42) break;
        if (code == 53 && n >= 1) type = b[off];
        if (code == 54 && n >= 4) *server = get32(b + off);
        if (code == 1 && n >= 4)  *mask = get32(b + off);
        if (code == 3 && n >= 4)  *gw = get32(b + off);
        if (code == 6 && n >= 4)  *dns = get32(b + off);
        off += n;
    }
    if (type != want_type) return 0;
    *yi = get32(b + 16);
    for (int i = 0; i < 6; i++) net_gwmac[i] = p[6 + i];
    return 1;
}

static int dhcp_wait(u8 want_type, u32 *yi, u32 *server, u32 *mask, u32 *gw, u32 *dns)
{
    u32 t0 = pit_ms();
    for (;;) {
        u16 len;
        if (ne2k_recv(rxbuf, &len) &&
            parse_dhcp(rxbuf, len, want_type, yi, server, mask, gw, dns))
            return 1;
        if (pit_ms() - t0 >= 2000) return 0;
        pit_sleep_ms(1);
    }
}

/* ------------------------------ ICMP ------------------------------- */
static u16 icmp_build(u32 dst)
{
    u8 *p = txbuf;
    memset(p, 0, 600);
    for (int i = 0; i < 6; i++) p[i] = net_gwmac[i];
    for (int i = 0; i < 6; i++) p[6 + i] = net_mac[i];
    put16(p + 12, ETHERTYPE_IP);

    const char *data = "PAPEROSPAPEROSPAPEROSPAPEROS!";
    u8 *icmp = p + 34;                 /* Ethernet(14) + IPv4(20) */
    for (int i = 0; data[i]; i++) icmp[8 + i] = (u8)data[i];
    put16(icmp, 0x0800);               /* type 8 = echo request */
    put16(icmp + 4, ping_id);
    put16(icmp + 6, ++ping_seq);
    put16(icmp + 2, ip_checksum(icmp, 40));

    u16 total = 20 + 40;
    p[14] = 0x45;                      /* IPv4, 5-word header */
    put16(p + 14 + 2, total);
    p[14 + 4] = 0x12; p[14 + 5] = 0x34;
    p[14 + 8] = 64;   p[14 + 9] = 1;
    put32(p + 14 + 12, net_ip);
    put32(p + 14 + 16, dst);
    put16(p + 14 + 10, ip_checksum(p + 14, 20));
    return (u16)(14 + total);
}

static void net_poll(void)
{
    if (!net_up) return;
    for (int i = 0; i < 4; i++) {
        u16 len;
        if (!ne2k_recv(rxbuf, &len)) break;
        if (len < 34 || get16(rxbuf + 12) != ETHERTYPE_IP) continue;
        const u8 *ip = rxbuf + 14;
        int ihl = (ip[0] & 0x0F) * 4;
        u16 total = get16(ip + 2);
        if (ihl < 20 || total < (u16)ihl || total > (u16)(len - 14)) continue;
        if (ip[9] == 1) {
            if (get32(ip + 12) != ping_target) continue;
            const u8 *icmp = ip + ihl;
            if (icmp[0] != 0) continue;  /* 0 = echo reply */
            if (get16(icmp + 4) != ping_id) continue;
            if (get16(icmp + 6) != ping_seq) continue;
            ping_done = 1;
        } else if (ip[9] == 17) {
            net_route_udp(ip, total);
        } else if (ip[9] == 6) {
            net_route_tcp(ip, total);
        }
    }
}

/* -------------------------- public commands ------------------------- */
static void net_init(void)
{
    serial_puts("PAPEROS NET: init\n");
    if (!pci_find_ne2k()) {
        term_push("NET: no ne2k_pci found");
        serial_puts("PAPEROS NET: no NIC\n");
        return;
    }
    ne2k_read_mac();
    ne2k_init();
    serial_puts("PAPEROS NET: MAC ");
    for (int i = 0; i < 6; i++) {
        char h[3];
        hex_byte(net_mac[i], h);
        serial_puts(h);
        if (i < 5) serial_puts(":");
    }
    serial_puts("\n");

    u32 yi = 0, server = 0, mask = 0, gw = 0, dns = 0;
    u16 len = dhcp_build(1, 0, 0);
    ne2k_send(txbuf, len);
    if (!dhcp_wait(2, &yi, &server, &mask, &gw, &dns)) {
        serial_puts("PAPEROS NET: DHCP discover timeout\n");
        term_push("NET: DHCP discover failed");
        return;
    }
    if (!gw) gw = server;
    len = dhcp_build(3, yi, server);
    ne2k_send(txbuf, len);
    if (!dhcp_wait(5, &yi, &server, &mask, &gw, &dns)) {
        serial_puts("PAPEROS NET: DHCP request timeout\n");
        term_push("NET: DHCP request failed");
        return;
    }
    if (!gw) gw = server;
    net_ip = yi; net_gw = gw; net_mask = mask; net_dns = dns; net_up = 1;

    char s[TLC + 1];
    ip_str(net_ip, s);
    serial_puts("PAPEROS NET: IP ");
    serial_puts(s);
    serial_puts(" GW ");
    ip_str(net_gw, s);
    serial_puts(s);
    serial_puts("\n");
    term_push(s);                       /* gateway, briefly */
    ip_str(net_ip, s);
    term_push(s);
}

static void net_cmd_ip(void)
{
    char s[TLC + 1];
    if (!net_up) { term_push("NET: down (type net)"); return; }
    ip_str(net_ip, s);
    term_push(s);
    ip_str(net_gw, s);
    term_push(s);
}

static void net_cmd_ping(const char *arg)
{
    if (!net_up) { term_push("NET: down (type net)"); return; }
    u32 dst = ip_parse(arg);
    if (!dst) { term_push("ping <ip>"); return; }
    ping_target = dst;
    ping_done = 0;
    ne2k_send(txbuf, icmp_build(dst));

    u32 t0 = pit_ms();
    while (!ping_done && pit_ms() - t0 < 1000) {
        net_poll();
        pit_sleep_ms(1);
    }
    char s[TLC + 1];
    ip_str(dst, s);
    if (ping_done) {
        char out[TLC + 1];
        int n = 0;
        const char *a = "64 bytes from ";
        while (*a) out[n++] = *a++;
        for (int i = 0; s[i]; i++) out[n++] = s[i];
        a = ": reply";
        while (*a) out[n++] = *a++;
        out[n] = 0;
        term_push(out);
    } else {
        char out[TLC + 1];
        int n = 0;
        const char *a = "ping ";
        while (*a) out[n++] = *a++;
        for (int i = 0; s[i]; i++) out[n++] = s[i];
        a = " timed out";
        while (*a) out[n++] = *a++;
        out[n] = 0;
        term_push(out);
    }
}

/* ------------------------- DNS over UDP ------------------------------ */
static volatile int dns_done = 0;
static u32 dns_result = 0;
static u16 dns_port = 0x3050;

static int dns_skip_name(const u8 **pp, const u8 *end)
{
    const u8 *p = *pp;
    int hops = 0;
    while (p < end && hops++ < 32) {
        u8 n = *p;
        if ((n & 0xC0) == 0xC0) {
            if (p + 2 > end) return 0;
            p += 2; *pp = p; return 1;
        }
        if (n & 0xC0) return 0;
        if (!n) { p++; *pp = p; return 1; }
        if (p + 1 + n > end) return 0;
        p += 1 + n;
    }
    return 0;
}

static int dns_parse_reply(const u8 *p, u16 len, u32 *out)
{
    const u8 *end = p + len;
    if (len < 12 || get16(p) != 0x2222) return 0;
    int qd = get16(p + 4), an = get16(p + 6);
    const u8 *pos = p + 12;
    for (int i = 0; i < qd; i++) {
        if (!dns_skip_name(&pos, end) || pos + 4 > end) return 0;
        pos += 4;
    }
    for (int i = 0; i < an; i++) {
        if (!dns_skip_name(&pos, end) || pos + 10 > end) return 0;
        u16 type = get16(pos), cls = get16(pos + 2), rdl = get16(pos + 8);
        pos += 10;
        if (type == 1 && cls == 1 && rdl == 4 && pos + 4 <= end) {
            *out = get32(pos);
            return 1;
        }
        if (pos + rdl > end) return 0;
        pos += rdl;
    }
    return 0;
}

static u16 dns_build(const char *host)
{
    u8 *p = txbuf;
    memset(p, 0, 600);
    for (int i = 0; i < 6; i++) p[i] = net_gwmac[i];
    for (int i = 0; i < 6; i++) p[6 + i] = net_mac[i];
    put16(p + 12, ETHERTYPE_IP);

    u8 *d = p + 42;
    put16(d, 0x2222);
    put16(d + 2, 0x0100);              /* recursion desired */
    put16(d + 4, 1);                   /* one question */
    put16(d + 6, 0);
    int n = 12;
    const char *label = host;
    while (*label && n < 180) {
        int len = 0;
        while (label[len] && label[len] != '.' && len < 63) len++;
        if (!len) return 0;
        d[n++] = (u8)len;
        for (int i = 0; i < len; i++) d[n++] = (u8)label[i];
        label += len;
        if (*label == '.') label++;
    }
    if (n + 5 > 200) return 0;
    d[n++] = 0;
    put16(d + n, 1); n += 2;           /* TYPE A */
    put16(d + n, 1); n += 2;           /* CLASS IN */
    u16 udp_len = (u16)(8 + n);
    u16 total = (u16)(20 + udp_len);

    p[14] = 0x45;
    put16(p + 16, total);
    put16(p + 18, 0x1235);
    p[22] = 64; p[23] = 17;
    put32(p + 26, net_ip);
    put32(p + 30, net_dns);
    put16(p + 24, ip_checksum(p + 14, 20));
    put16(p + 34, dns_port);
    put16(p + 36, 53);
    put16(p + 38, udp_len);
    put16(p + 40, 0);                  /* IPv4 UDP checksum may be zero */
    return (u16)(14 + total);
}

static void net_route_udp(const u8 *ip, u16 total)
{
    if (dns_done || total < 28) return;
    const u8 *udp = ip + 20;
    u16 ulen = get16(udp + 4);
    if (ulen < 8 || ulen > total - 20) return;
    if (get16(udp) != 53 || get16(udp + 2) != dns_port) return;
    if (get32(ip + 12) != net_dns) return;
    if (dns_parse_reply(udp + 8, (u16)(ulen - 8), &dns_result)) dns_done = 1;
}

static int dns_resolve(const char *host, u32 *out)
{
    if (ip_parse(host)) { *out = ip_parse(host); return (int)*out; }
    if (!net_up) return 0;
    dns_done = 0; dns_result = 0;
    dns_port++;
    if (dns_port < 1024) dns_port = 0x3050;
    u16 len = dns_build(host);
    if (!len) return 0;
    ne2k_send(txbuf, len);
    u32 t0 = pit_ms();
    while (!dns_done && pit_ms() - t0 < 3000) {
        net_poll();
        pit_sleep_ms(1);
    }
    if (!dns_done) return 0;
    *out = dns_result;
    return 1;
}

/* --------------------------- TCP for HTTP ---------------------------- */
#define WEB_TCP_PORT 0x3040
static u32 tcp_remote_ip, tcp_seq, tcp_ack, tcp_expected;
static u16 tcp_remote_port;
static int tcp_state = 0;              /* 0 idle, 1 SYN sent, 2 open */
static volatile int tcp_connected = 0, tcp_done = 0;
static u8 *tcp_rx_target = 0;
static u32 tcp_rx_cap = 0, tcp_rx_len = 0;

static u16 ones_sum(const u8 *d, u32 n)
{
    u32 sum = 0;
    while (n > 1) {
        sum += (u32)((d[0] << 8) | d[1]);
        d += 2; n -= 2;
    }
    if (n) sum += (u32)(d[0] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)sum;
}

static u16 tcp_checksum(const u8 *data, u16 len)
{
    u8 ph[12];
    put32(ph, net_ip);
    put32(ph + 4, tcp_remote_ip);
    ph[8] = 0; ph[9] = 6;
    put16(ph + 10, len);
    u32 sum = (u32)ones_sum(ph, 12) + (u32)ones_sum(data, len);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

static u16 tcp_build(u8 flags, const u8 *data, u16 dlen)
{
    u8 *p = txbuf;
    memset(p, 0, 600);
    for (int i = 0; i < 6; i++) p[i] = net_gwmac[i];
    for (int i = 0; i < 6; i++) p[6 + i] = net_mac[i];
    put16(p + 12, ETHERTYPE_IP);
    u16 tcplen = (u16)(20 + dlen);
    u16 total = (u16)(20 + tcplen);

    p[14] = 0x45;
    put16(p + 16, total);
    put16(p + 18, 0x1236);
    p[22] = 64; p[23] = 6;
    put32(p + 26, net_ip);
    put32(p + 30, tcp_remote_ip);
    put16(p + 24, ip_checksum(p + 14, 20));

    u8 *t = p + 34;                    /* 14-byte Ethernet + 20-byte IPv4 */
    put16(t, WEB_TCP_PORT);
    put16(t + 2, tcp_remote_port);
    put32(t + 4, tcp_seq);
    put32(t + 8, tcp_ack);
    t[12] = 0x50;                      /* 5 words, no options */
    t[13] = flags;
    put16(t + 14, 4096);
    put16(t + 16, 0);
    put16(t + 18, 0);
    if (dlen && data) memcpy(t + 20, data, dlen);
    put16(t + 16, tcp_checksum(t, tcplen));
    return (u16)(14 + total);
}

static void tcp_send(u8 flags, const u8 *data, u16 dlen)
{
    u16 len = tcp_build(flags, data, dlen);
    ne2k_send(txbuf, len);
    if (dlen) tcp_seq += dlen;
}

static void net_route_tcp(const u8 *ip, u16 total)
{
    if (!tcp_state || total < 40) return;
    int ihl = (ip[0] & 0x0F) * 4;
    if (ihl < 20 || total < (u16)(ihl + 20)) return;
    const u8 *t = ip + ihl;
    if (get16(t) != tcp_remote_port || get16(t + 2) != WEB_TCP_PORT) return;
    if (get32(ip + 12) != tcp_remote_ip) return;
    int hlen = ((t[12] >> 4) & 0x0F) * 4;
    if (hlen < 20 || total < (u16)(ihl + hlen)) return;

    u32 seq = get32(t + 4);
    u8 flags = t[13];
    u16 plen = (u16)(total - ihl - hlen);
    if (flags & 0x04) { tcp_done = 1; return; } /* RST */

    if (tcp_state == 1 && (flags & 0x12) == 0x12) {
        tcp_ack = seq + 1;
        tcp_expected = tcp_ack;
        tcp_seq++;                     /* SYN consumes one sequence number */
        tcp_send(0x10, 0, 0);
        tcp_state = 2;
        tcp_connected = 1;
        return;
    }
    if (tcp_state != 2) return;

    if (plen) {
        const u8 *payload = t + hlen;
        if (seq == tcp_expected && tcp_rx_target &&
            tcp_rx_len + plen <= tcp_rx_cap) {
            memcpy(tcp_rx_target + tcp_rx_len, payload, plen);
            tcp_rx_len += plen;
            tcp_expected += plen;
            tcp_ack = tcp_expected;
        }
        tcp_send(0x10, 0, 0);
    }
    if (flags & 0x01) {                 /* FIN */
        tcp_ack = tcp_expected + 1;
        tcp_send(0x10, 0, 0);
        tcp_done = 1;
    }
    if (tcp_rx_len >= tcp_rx_cap) tcp_done = 1;
}

static int tcp_connect(u32 ip, u16 port)
{
    tcp_remote_ip = ip; tcp_remote_port = port;
    tcp_seq = 0x4000u + pit_ms();
    tcp_ack = 0; tcp_expected = 0;
    tcp_state = 1; tcp_connected = 0; tcp_done = 0;
    tcp_send(0x02, 0, 0);              /* SYN; manual ++ after SYN-ACK */
    u32 t0 = pit_ms();
    while (!tcp_connected && !tcp_done && pit_ms() - t0 < 3000) {
        net_poll();
        pit_sleep_ms(1);
    }
    if (!tcp_connected) { tcp_state = 0; return 0; }
    return 1;
}

static int tcp_get(u32 ip, u16 port, const char *host, const char *path,
                   u8 *buf, u32 cap, u32 *outlen)
{
    if (!tcp_connect(ip, port)) return 0;
    char req[320];
    int n = 0;
#define REQ_ADD(str) do { const char *_a = (str); while (*_a && n < 318) req[n++] = *_a++; } while (0)
    REQ_ADD("GET ");
    REQ_ADD(path);
    REQ_ADD(" HTTP/1.0\r\nHost: ");
    REQ_ADD(host);
    REQ_ADD("\r\nUser-Agent: PAPEROS/0.9\r\nConnection: close\r\n\r\n");
#undef REQ_ADD
    tcp_rx_target = buf;
    tcp_rx_cap = cap;
    tcp_rx_len = 0;
    tcp_send(0x18, (const u8 *)req, (u16)n); /* PSH+ACK */
    u32 t0 = pit_ms();
    while (!tcp_done && tcp_rx_len < cap && pit_ms() - t0 < 6000) {
        net_poll();
        pit_sleep_ms(1);
    }
    tcp_state = 0;
    tcp_rx_target = 0;
    *outlen = tcp_rx_len;
    return tcp_rx_len > 0;
}

/* ------------------------ text-only web browser ---------------------- */
#define WEB_PAGE_MAX 8192
#define WEB_LINES    96
#define WEB_LINE_CH  96
#define WEB_LINKS    16
#define WEB_URL_MAX  128
static u8 web_page[WEB_PAGE_MAX];
static u32 web_len = 0;
static char web_url[160] = "http://10.0.2.2:8000/";
static char web_input[160] = "http://10.0.2.2:8000/";
static int web_input_len = 0;
static int web_input_focus = 0, web_scroll = 0;
static char web_status[80] = "WEB: press Enter to load a URL";
static char web_lines[WEB_LINES][WEB_LINE_CH + 1];
static int web_lines_n = 0;
static char web_links[WEB_LINKS][WEB_URL_MAX];
static int web_link_line[WEB_LINKS], web_link_n = 0;
static char web_hist[8][160];
static int web_hist_n = 0, web_hist_pos = -1;
static char web_curline[WEB_LINE_CH + 1];
static int web_curcol = 0;

static void web_set_status(const char *s)
{
    int i = 0;
    for (; s[i] && i < 79; i++) web_status[i] = s[i];
    web_status[i] = 0;
}

static void web_line_flush(void)
{
    if (web_curcol && web_lines_n < WEB_LINES) {
        memcpy(web_lines[web_lines_n], web_curline, (u32)web_curcol);
        web_lines[web_lines_n][web_curcol] = 0;
        web_lines_n++;
    }
    web_curcol = 0;
    web_curline[0] = 0;
}

static void web_line_put(char c)
{
    if (web_curcol >= WEB_LINE_CH) web_line_flush();
    web_curline[web_curcol++] = c;
    web_curline[web_curcol] = 0;
}

static char web_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c + 32) : c;
}

static int web_ci_at(const u8 *s, const char *word)
{
    int i = 0;
    while (word[i]) {
        if (!s[i] || web_lower((char)s[i]) != web_lower(word[i])) return 0;
        i++;
    }
    return 1;
}

static int web_ci_substr(const u8 *b, int n, const char *word)
{
    int wn = 0;
    while (word[wn]) wn++;
    for (int i = 0; i + wn <= n; i++)
        if (web_ci_at(b + i, word)) return 1;
    return 0;
}

static int web_tag_eq(const u8 *b, int start, int end, const char *name)
{
    int i = start;
    if (i < end && b[i] == '/') i++;
    int j = 0;
    while (name[j]) {
        if (i >= end || web_lower((char)b[i]) != web_lower(name[j])) return 0;
        i++; j++;
    }
    if (i < end) {
        char c = (char)b[i];
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return 0;
    }
    return 1;
}

static char web_entity(const u8 *s, int n)
{
    if (n == 4 && web_ci_at(s, "amp;")) return '&';
    if (n == 3 && web_ci_at(s, "lt;")) return '<';
    if (n == 3 && web_ci_at(s, "gt;")) return '>';
    if (n == 5 && web_ci_at(s, "quot;")) return '"';
    if (n == 5 && web_ci_at(s, "nbsp;")) return ' ';
    if (n == 5 && web_ci_at(s, "apos;")) return '\'';
    return 0;
}

static int web_find_href(const u8 *b, int start, int end, char *out, int max)
{
    for (int i = start; i < end - 4; i++) {
        if (!web_ci_at(b + i, "href")) continue;
        int j = i + 4;
        while (j < end && (b[j] == ' ' || b[j] == '\t')) j++;
        if (j >= end || b[j] != '=') continue;
        j++;
        while (j < end && (b[j] == ' ' || b[j] == '\t')) j++;
        if (j >= end) continue;
        u8 q = b[j];
        if (q == '"' || q == '\'') j++;
        int n = 0;
        while (j < end && n < max - 1) {
            u8 c = b[j];
            if ((q == '"' || q == '\'') && c == q) break;
            if (q != '"' && q != '\'' && (c == ' ' || c == '>')) break;
            if (c == '&') {
                int k = 1;
                while (k < 8 && j + k < end && b[j + k] != ';') k++;
                char e = (j + k < end) ? web_entity(b + j + 1, k) : 0;
                if (e) { out[n++] = e; j += k + 1; continue; }
            }
            out[n++] = (char)c;
            j++;
        }
        out[n] = 0;
        if (n) return 1;
    }
    return 0;
}

static void web_copy(char *d, const char *s, int max)
{
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

static int web_split_url(const char *url, char *host, int hostsz,
                         u16 *port, char *path, int pathsz)
{
    const char *p = url;
    if (strncmp_local(p, "http://") == 0) p += 7;
    else if (strncmp_local(p, "https://") == 0) return 0;
    int h = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && h < hostsz - 1)
        host[h++] = *p++;
    host[h] = 0;
    if (!h) return 0;
    *port = 80;
    if (*p == ':') {
        p++;
        int v = 0, d = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); d++; }
        if (!d || v < 1 || v > 65535) return 0;
        *port = (u16)v;
    }
    int n = 0;
    if (*p != '/') {
        path[n++] = '/';
        if (*p == '?') {
            path[n++] = '?';
            while (*p && n < pathsz - 1) path[n++] = *p++;
        }
    } else {
        while (*p && n < pathsz - 1) path[n++] = *p++;
    }
    path[n] = 0;
    return 1;
}

static void web_resolve(const char *href, char *out, int max)
{
    if (strncmp_local(href, "http://") == 0) { web_copy(out, href, max); return; }
    if (href[0] == '/' && href[1] == '/') {
        out[0] = 'h'; out[1] = 't'; out[2] = 't'; out[3] = 'p'; out[4] = ':';
        web_copy(out + 5, href, max - 5);
        return;
    }
    char host[96]; u16 port; char path[160];
    if (!web_split_url(web_url, host, sizeof(host), &port, path, sizeof(path))) {
        web_copy(out, href, max); return;
    }
    if (href[0] == '/') {
        int n = 0;
        const char *a = "http://"; while (*a && n < max - 1) out[n++] = *a++;
        for (int i = 0; host[i] && n < max - 1; i++) out[n++] = host[i];
        if (port != 80) {
            char ps[8]; itoa(port, ps);
            out[n++] = ':';
            for (int i = 0; ps[i] && n < max - 1; i++) out[n++] = ps[i];
        }
        for (int i = 0; href[i] && n < max - 1; i++) out[n++] = href[i];
        out[n] = 0;
        return;
    }
    int cut = 0;
    for (int i = 0; web_url[i]; i++) if (web_url[i] == '/') cut = i + 1;
    if (cut <= 7) {
        int n = 0;
        const char *a = "http://"; while (*a && n < max - 1) out[n++] = *a++;
        for (int i = 0; host[i] && n < max - 1; i++) out[n++] = host[i];
        if (n < max - 1) out[n++] = '/';
        for (int i = 0; href[i] && n < max - 1; i++) out[n++] = href[i];
        out[n] = 0;
        return;
    }
    int n = 0;
    for (int i = 0; i < cut && n < max - 1; i++) out[n++] = web_url[i];
    for (int i = 0; href[i] && n < max - 1; i++) out[n++] = href[i];
    out[n] = 0;
}

static int web_header_end(const u8 *b, u32 n)
{
    for (u32 i = 0; i + 3 < n; i++)
        if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n')
            return (int)i + 4;
    for (u32 i = 0; i + 1 < n; i++)
        if (b[i] == '\n' && b[i + 1] == '\n') return (int)i + 2;
    return -1;
}

static int web_dechunk(u8 *b, int n)
{
    int pos = 0, out = 0;
    while (pos < n) {
        while (pos < n && (b[pos] == '\r' || b[pos] == '\n')) pos++;
        int size = 0, digits = 0;
        while (pos < n) {
            char c = (char)b[pos++];
            int v = -1;
            if (c >= '0' && c <= '9') v = c - '0';
            else if (c >= 'a' && c <= 'f') v = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v = c - 'A' + 10;
            if (v < 0) break;
            size = size * 16 + v; digits++;
        }
        while (pos < n && b[pos] != '\r' && b[pos] != '\n') pos++;
        while (pos < n && (b[pos] == '\r' || b[pos] == '\n')) pos++;
        if (!digits || !size || pos + size > n) break;
        for (int i = 0; i < size; i++) b[out++] = b[pos++];
    }
    return out;
}

static void web_render(void)
{
    web_lines_n = 0; web_link_n = 0; web_curcol = 0; web_curline[0] = 0;
    int hend = web_header_end(web_page, web_len);
    if (web_len > 12) {
        int n = 0;
        for (int i = 0; i < 63 && i < (int)web_len; i++) {
            char c = (char)web_page[i];
            if (c == '\r' || c == '\n') break;
            web_status[n++] = c;
        }
        web_status[n] = 0;
    }
    const u8 *body = hend >= 0 ? web_page + hend : web_page;
    int tlen = hend >= 0 ? (int)(web_len - hend) : (int)web_len;
    if (tlen < 0) tlen = 0;
    if (hend > 0 && web_ci_substr(web_page, hend, "chunked"))
        tlen = web_dechunk((u8 *)body, tlen);

    int skip = 0, i = 0;
    while (i < tlen && web_lines_n < WEB_LINES) {
        u8 c = body[i];
        if (c == '<') {
            int e = i + 1;
            while (e < tlen && body[e] != '>') e++;
            int closing = (i + 1 < tlen && body[i + 1] == '/');
            if (!closing && (web_tag_eq(body, i + 1, e, "script") ||
                             web_tag_eq(body, i + 1, e, "style"))) {
                skip = 1;
            } else if (closing && (web_tag_eq(body, i + 1, e, "script") ||
                                   web_tag_eq(body, i + 1, e, "style"))) {
                skip = 0;
            } else if (!skip && !closing && web_tag_eq(body, i + 1, e, "a")) {
                char href[WEB_URL_MAX];
                if (web_find_href(body, i + 1, e, href, sizeof(href)) &&
                    web_link_n < WEB_LINKS) {
                    int li = web_link_n++;
                    web_resolve(href, web_links[li], WEB_URL_MAX);
                    char mark[10];
                    mark[0] = '[';
                    itoa((u32)(li + 1), mark + 1);
                    int k = 1 + (int)strlen(mark + 1);
                    mark[k++] = ']'; mark[k++] = ' '; mark[k] = 0;
                    for (int j = 0; mark[j]; j++) web_line_put(mark[j]);
                }
            } else if (!skip && (web_tag_eq(body, i + 1, e, "br") ||
                                 web_tag_eq(body, i + 1, e, "p") ||
                                 web_tag_eq(body, i + 1, e, "div") ||
                                 web_tag_eq(body, i + 1, e, "li") ||
                                 web_tag_eq(body, i + 1, e, "tr") ||
                                 web_tag_eq(body, i + 1, e, "h1") ||
                                 web_tag_eq(body, i + 1, e, "h2") ||
                                 web_tag_eq(body, i + 1, e, "h3"))) {
                web_line_flush();
            }
            i = e < tlen ? e + 1 : tlen;
            continue;
        }
        if (skip) { i++; continue; }
        if (c == '\n') { web_line_flush(); i++; continue; }
        if (c == '&') {
            int k = 1;
            while (k < 8 && i + k < tlen && body[i + k] != ';') k++;
            char e = (i + k < tlen) ? web_entity(body + i + 1, k) : 0;
            if (e) { web_line_put(e); i += k + 1; continue; }
            web_line_put('&'); i++; continue;
        }
        if (c == ' ' || c == '\t') {
            if (web_curcol && web_curline[web_curcol - 1] != ' ') web_line_put(' ');
            i++; continue;
        }
        if (c < 32) { i++; continue; }
        web_line_put((char)c);
        i++;
    }
    web_line_flush();

    for (int li = 0; li < web_link_n && web_lines_n < WEB_LINES; li++) {
        web_link_line[li] = web_lines_n;
        char line[WEB_LINE_CH + 1];
        int n = 0;
        line[n++] = '[';
        itoa((u32)(li + 1), line + n); n += strlen(line + n);
        line[n++] = ']'; line[n++] = ' ';
        for (int j = 0; web_links[li][j] && n < WEB_LINE_CH - 1; j++)
            line[n++] = web_links[li][j];
        line[n] = 0;
        memcpy(web_lines[web_lines_n], line, (u32)n + 1);
        web_lines_n++;
    }
}

static void web_history_push(const char *url)
{
    if (web_hist_n && streq(web_hist[web_hist_n - 1], url)) return;
    if (web_hist_n < 8) {
        web_copy(web_hist[web_hist_n++], url, sizeof(web_hist[0]));
    } else {
        for (int i = 0; i < 7; i++)
            web_copy(web_hist[i], web_hist[i + 1], sizeof(web_hist[0]));
        web_copy(web_hist[7], url, sizeof(web_hist[0]));
    }
    web_hist_pos = web_hist_n - 1;
}

static void web_fetch(const char *url, int add_history)
{
    if (!url || !*url) return;
    web_copy(web_url, url, sizeof(web_url));
    web_copy(web_input, url, sizeof(web_input));
    web_input_len = (int)strlen(web_input);
    web_input_focus = 0;
    web_scroll = 0;
    web_lines_n = 0; web_link_n = 0;

    char host[96], path[160];
    u16 port = 80;
    u32 ip = 0;
    web_set_status("WEB: resolving...");
    if (!web_split_url(web_url, host, sizeof(host), &port, path, sizeof(path))) {
        web_set_status("WEB: only http:// URLs are supported");
        return;
    }
    if (!dns_resolve(host, &ip)) {
        web_set_status("WEB: DNS failed");
        return;
    }
    web_set_status("WEB: connecting...");
    u32 got = 0;
    if (!tcp_get(ip, port, host, path, web_page, sizeof(web_page), &got)) {
        web_set_status("WEB: connection failed");
        return;
    }
    web_len = got;
    web_render();
    if (add_history) web_history_push(web_url);
    int i = 0;
    while (web_status[i] && i < 66) i++;
    char suffix[16] = "  [";
    itoa(got, suffix + 3);
    int j = 3 + (int)strlen(suffix + 3);
    suffix[j++] = ']'; suffix[j] = 0;
    for (int k = 0; suffix[k] && i < 79; k++) web_status[i++] = suffix[k];
    web_status[i] = 0;
}

static void web_init(void)
{
    web_copy(web_url, "http://10.0.2.2:8000/", sizeof(web_url));
    web_copy(web_input, web_url, sizeof(web_input));
    web_input_len = (int)strlen(web_input);
    web_input_focus = 0; web_scroll = 0;
    web_lines_n = 0; web_link_n = 0; web_len = 0;
    web_set_status("WEB: press Enter to load; try http://10.0.2.2:8000/");
}

static void web_layout(int cw, int *input_w, int *bx, int *bw)
{
    int iw = cw - 210;
    if (iw < 80) iw = 80;
    if (iw > cw - 12) iw = cw - 12;
    if (iw < 40) iw = 40;
    *input_w = iw;
    *bx = 4 + iw + 6;
    *bw = (cw - *bx - 4) / 3;
    if (*bw < 36) *bw = 36;
}

static void web_key(u8 k)
{
    if (web_input_focus) {
        if (k == K_ENTER) { web_fetch(web_input, 1); return; }
        if (k == K_ESC) { web_input_focus = 0; return; }
        if (k == K_BACK) {
            if (web_input_len) web_input[--web_input_len] = 0;
            return;
        }
        if (k >= 32 && k < 127 && web_input_len < 159) {
            web_input[web_input_len++] = (char)k;
            web_input[web_input_len] = 0;
        }
        return;
    }
    if (k == K_UP) { if (web_scroll) web_scroll--; return; }
    if (k == K_DOWN) { web_scroll++; return; }
    if (k == 'b' || k == 'B') {
        if (web_hist_pos > 0) { web_hist_pos--; web_fetch(web_hist[web_hist_pos], 0); }
        return;
    }
    if (k == 'r' || k == 'R') { web_fetch(web_url, 0); return; }
    if (k == 'h' || k == 'H') { web_fetch("http://10.0.2.2:8000/", 1); return; }
}

static void web_click(int lx, int ly, int cw, int ch)
{
    int input_w, bx, bw;
    web_layout(cw, &input_w, &bx, &bw);
    if (ly >= 2 && ly < 20 && lx >= 4 && lx < 4 + input_w) {
        web_input_focus = 1;
        web_input[0] = 0; web_input_len = 0;
        return;
    }
    if (ly >= 2 && ly < 20 && lx >= bx) {
        int b = (lx - bx) / bw;
        web_input_focus = 0;
        if (b == 0 && web_hist_pos > 0) {
            web_hist_pos--;
            web_fetch(web_hist[web_hist_pos], 0);
        } else if (b == 1) web_fetch(web_url, 0);
        else if (b == 2) web_fetch("http://10.0.2.2:8000/", 1);
        return;
    }
    if (ly < 24) return;
    int line = web_scroll + (ly - 24) / FONT_H;
    for (int i = 0; i < web_link_n; i++) {
        if (web_link_line[i] == line) { web_fetch(web_links[i], 1); return; }
    }
    (void)ch;
}

static void draw_browser(int cx, int cy, int cw, int ch)
{
    fillrect(cx, cy, cw, ch, C_FACE);
    frame(cx, cy, cw, ch, C_BLACK);
    int input_w, bx, bw;
    web_layout(cw, &input_w, &bx, &bw);

    fillrect(cx + 4, cy + 2, input_w, 18, C_WHITE);
    frame(cx + 4, cy + 2, input_w, 18, web_input_focus ? accent : C_DGRAY);
    int maxchars = (input_w - 8) / FONT_W;
    if (maxchars < 1) maxchars = 1;
    draw_text_n(cx + 8, cy + 5, web_input, maxchars, C_BLACK, C_WHITE);
    static const char *blab[3] = {"BACK", "LOAD", "HOME"};
    for (int i = 0; i < 3; i++) {
        draw_bevel(cx + bx + i * bw, cy + 2, bw - 3, 18, 0);
        int tw = (int)strlen(blab[i]) * FONT_W;
        draw_text(cx + bx + i * bw + (bw - 3 - tw) / 2, cy + 5,
                  blab[i], C_BLACK, C_FACE);
    }

    int top = 24;
    int sh = ch - FONT_H - 6;
    if (sh < top + FONT_H) sh = top + FONT_H;
    fillrect(cx + 3, cy + top, cw - 6, sh - top, C_WHITE);
    frame(cx + 3, cy + top, cw - 6, sh - top, C_DGRAY);
    if (web_scroll >= web_lines_n) web_scroll = web_lines_n ? web_lines_n - 1 : 0;
    int rows = (sh - top - 4) / FONT_H;
    if (rows < 1) rows = 1;
    if (web_lines_n) {
        for (int i = web_scroll; i < web_lines_n && i < web_scroll + rows; i++) {
            int hot = 0;
            for (int j = 0; j < web_link_n; j++)
                if (web_link_line[j] == i) { hot = 1; break; }
            draw_text_n(cx + 6, cy + top + 2 + (i - web_scroll) * FONT_H,
                        web_lines[i], (cw - 12) / FONT_W,
                        hot ? C_BLUE : C_BLACK, C_WHITE);
        }
    } else {
        draw_text_n(cx + 8, cy + top + 8, "PAPEROS WEB", (cw - 12) / FONT_W,
                    C_BLACK, C_WHITE);
        draw_text_n(cx + 8, cy + top + 28, "Type a URL above and press Enter.",
                    (cw - 12) / FONT_W, C_DGRAY, C_WHITE);
        draw_text_n(cx + 8, cy + top + 44, "Real DNS + TCP + HTTP over NE2000.",
                    (cw - 12) / FONT_W, C_DGRAY, C_WHITE);
    }

    draw_bevel(cx + 3, cy + ch - FONT_H - 3, cw - 6, FONT_H + 1, 1);
    draw_text_n(cx + 8, cy + ch - FONT_H - 1, web_status,
                (cw - 16) / FONT_W, C_BLACK, C_FACE);
}

/* ============================ TEXT / TTY ============================== */
#define TTY_COLS 80
#define TTY_ROWS 25
#define TTY_MEM  ((volatile u16 *)0xB8000)
static int tty_x = 0, tty_y = 0;

static void tty_clear(void)
{
    for (int i = 0; i < TTY_COLS * TTY_ROWS; i++) TTY_MEM[i] = 0x1F20;
    tty_x = tty_y = 0;
}
static void tty_scroll(void)
{
    for (int y = 0; y < TTY_ROWS - 1; y++)
        for (int x = 0; x < TTY_COLS; x++)
            TTY_MEM[y * TTY_COLS + x] = TTY_MEM[(y + 1) * TTY_COLS + x];
    for (int x = 0; x < TTY_COLS; x++)
        TTY_MEM[(TTY_ROWS - 1) * TTY_COLS + x] = 0x1F20;
    tty_y = TTY_ROWS - 1;
}
static void tty_putc(char c)
{
    if (c == '\n') { tty_x = 0; if (++tty_y >= TTY_ROWS) tty_scroll(); return; }
    if (c == '\r') { tty_x = 0; return; }
    if (c == '\b') { if (tty_x) tty_x--; return; }
    if (tty_x >= TTY_COLS) { tty_x = 0; if (++tty_y >= TTY_ROWS) tty_scroll(); }
    TTY_MEM[tty_y * TTY_COLS + tty_x++] = (u16)(0x1F00 | (u8)c);
}
static void tty_puts(const char *s)
{
    while (*s) { tty_putc(*s); serial_putc(*s); s++; }
}
static void tty_num(const char *prefix, u32 v)
{
    char s[64];
    int n = 0;
    while (*prefix && n < 60) s[n++] = *prefix++;
    itoa(v, s + n); n += strlen(s + n);
    s[n] = 0;
    tty_puts(s);
}
static char tty_line[128];
static int tty_len = 0;
static void tty_prompt(void) { tty_puts("PAPEROS:/# "); }
static void tty_fs_list(void)
{
    if (!fs_ready) { tty_puts("FS: not ready - type format\r\n"); return; }
    for (int i = 0; i < FS_MAX_FILES; i++) {
        if (!fs_dir[i].used) continue;
        char s[64];
        int n = 0;
        for (int j = 0; j < 16 && fs_dir[i].name[j]; j++) s[n++] = fs_dir[i].name[j];
        while (n < 18) s[n++] = ' ';
        itoa(fs_dir[i].bytes, s + n); n += strlen(s + n);
        s[n++] = '\r'; s[n++] = '\n'; s[n] = 0;
        tty_puts(s);
    }
}
static int install_os_to_disk(void);

static void tty_exec(void)
{
    char cmd[128], arg[32], full[128];
    int i = 0, a = 0;
    tty_puts("\r\n");
    tty_line[tty_len] = 0;
    while (tty_line[i] && i < 127) { full[i] = tty_line[i]; i++; }
    full[i] = 0;
    i = 0;
    while (full[i] && full[i] != ' ' && i < 127) { cmd[i] = full[i]; i++; }
    cmd[i] = 0;
    if (full[i] == ' ') {
        i++;
        while (full[i] && a < 31) arg[a++] = full[i++];
    }
    arg[a] = 0;

    if (streq(cmd, "help")) {
        tty_puts("help ver clear echo date about reboot\r\n");
        tty_puts("format ls touch <f> rm <f> cat <f> df install\r\n");
    } else if (streq(cmd, "ver")) tty_puts("PAPEROS 0.9 / real TTY + storage\r\n");
    else if (streq(cmd, "clear")) tty_clear();
    else if (streq(cmd, "about")) tty_puts("PAPEROS - written from scratch in C and GAS.\r\n");
    else if (streq(cmd, "echo")) { tty_puts(arg); tty_puts("\r\n"); }
    else if (streq(cmd, "date")) {
        char s[16];
        int d = bcd(cmos(0x07)), mo = bcd(cmos(0x08)), y = bcd(cmos(0x09));
        s[0]='2'; s[1]='0'; s[2]=(char)('0'+y/10); s[3]=(char)('0'+y%10);
        s[4]='-'; s[5]=(char)('0'+mo/10); s[6]=(char)('0'+mo%10);
        s[7]='-'; s[8]=(char)('0'+d/10); s[9]=(char)('0'+d%10);
        s[10]='\r'; s[11]='\n'; s[12]=0;
        tty_puts(s);
    } else if (streq(cmd, "format")) tty_puts(fs_format() ? "FS: formatted\r\n" : "FS: failed\r\n");
    else if (streq(cmd, "ls")) tty_fs_list();
    else if (streq(cmd, "touch")) tty_puts(fs_touch(arg) ? "FS: created\r\n" : "FS: failed\r\n");
    else if (streq(cmd, "rm")) tty_puts(fs_delete(arg) ? "FS: removed\r\n" : "FS: failed\r\n");
    else if (streq(cmd, "cat")) {
        int len = fs_read(arg, term_filebuf, sizeof(term_filebuf));
        if (len <= 0) tty_puts("FS: read failed\r\n");
        else {
            for (int j = 0; j < len; j++) {
                if (term_filebuf[j] == '\n') tty_putc('\r');
                tty_putc((char)term_filebuf[j]);
            }
            tty_puts("\r\n");
        }
    }
    else if (streq(cmd, "df")) {
        if (!fs_ready) tty_puts("FS: not ready\r\n");
        else { tty_num("used ", fs_used_sectors()); tty_num(" / total ", FS_TOTAL); tty_puts("\r\n"); }
    }
    else if (streq(cmd, "install")) {
        tty_puts(install_os_to_disk() ? "Install complete. Reboot to installed disk.\r\n"
                                      : "Install failed.\r\n");
    }
    else if (streq(cmd, "reboot")) { outb(0x64, 0xFE); for (;;) {} }
    else if (cmd[0]) tty_puts("unknown command\r\n");
    tty_len = 0;
    tty_line[0] = 0;
    tty_prompt();
}

static void tty_main(void)
{
    tty_clear();
    tty_puts("PAPEROS 0.9 - TTY MODE\r\n");
    tty_puts("Custom x86 kernel. Type 'help' for commands.\r\n\r\n");
    fs_load();
    tty_prompt();
    for (;;) {
        poll_keyboard();
        u8 k;
        while ((k = key_pop())) {
            if (k == K_ENTER) { tty_exec(); continue; }
            if (k == K_BACK) {
                if (tty_len) { tty_len--; tty_line[tty_len] = 0; tty_putc('\b'); tty_putc(' '); tty_putc('\b'); }
                continue;
            }
            if (k >= 32 && k < 127 && tty_len < 127) {
                tty_line[tty_len++] = (char)k;
                tty_line[tty_len] = 0;
                tty_putc((char)k);
            }
        }
        pit_sleep_ms(1);
    }
}

/* ============================= INSTALLER ============================== */
#define PRISTINE_KERN ((const u8 *)0x1C00000)
static int ata_copy_sectors(u32 lba, u32 count, u8 *buf, int write)
{
    u8 rb[512];
    for (u32 i = 0; i < count; i++) {
        if (write) {
            int good = 0;
            for (int attempt = 0; attempt < 4; attempt++) {
                if (!ata_write(lba + i, 1, buf)) continue;
                if (!ata_flush()) continue;
                if (!ata_read(lba + i, 1, rb)) continue;
                if (!memcmp_local(rb, buf, 512)) { good = 1; break; }
            }
            if (!good) return 0;
        } else if (!ata_read(lba + i, 1, buf)) return 0;
        buf += 512;
    }
    return 1;
}
static int install_os_to_disk(void)
{
    u8 buf[512];
    if (!ata_read(0, 1, buf)) return 0;              /* primary IDE must exist */
    if (!ata_copy_sectors(0, 1, (u8 *)boot1_image, 1)) return 0;
    if (!ata_copy_sectors(1, 4, (u8 *)boot2_image, 1)) return 0;
    u16 ksec = *(volatile u16 *)0x501A;
    if (!ksec || ksec > 768) ksec = 363;
    if (!ata_copy_sectors(5, ksec, (u8 *)PRISTINE_KERN, 1)) return 0;
    if (!ata_flush()) return 0;
    if (!fs_format()) return 0;
    if (!ata_read(0, 1, buf) || buf[510] != 0x55 || buf[511] != 0xAA) return 0;
    if (!ata_read(FS_BASE, 1, buf) || !fs_check_magic(buf)) return 0;
    return 1;
}
static void installer_main(void)
{
    tty_clear();
    tty_puts("PAPEROS INSTALLER\r\n================\r\n");
    tty_puts("Target: primary IDE disk (disk.img)\r\n");
    tty_puts("This writes the boot image and creates PAPEROSFS.\r\n\r\n");

    tty_puts("[1/4] Checking disk ... ");
    u8 probe[512];
    if (!ata_read(0, 1, probe)) { tty_puts("FAILED\r\nNo primary IDE disk found.\r\n"); for (;;) {} }
    tty_puts("OK\r\n");

    tty_puts("[2/4] Writing bootloader + kernel ... ");
    u16 ksec_local = *(volatile u16 *)0x501A;
    if (!ksec_local || ksec_local > 768) ksec_local = 363;
    u8 ok = ata_copy_sectors(0, 1, (u8 *)boot1_image, 1);
    if (ok) ok = ata_copy_sectors(1, 4, (u8 *)boot2_image, 1);
    if (ok) ok = ata_copy_sectors(5, ksec_local, (u8 *)PRISTINE_KERN, 1);
    tty_puts(ok ? "OK\r\n" : "FAILED\r\n");

    tty_puts("[3/4] Creating filesystem ... ");
    if (ok) ok = fs_format();
    tty_puts(ok ? "OK\r\n" : "FAILED\r\n");

    if (ok) ok = ata_flush();
    tty_puts("[4/4] Verifying ... ");
    if (ok) {
        ok = ata_read(0, 1, probe) && probe[510] == 0x55 && probe[511] == 0xAA;
        if (ok) ok = ata_read(FS_BASE, 1, probe) && fs_check_magic(probe);
    }
    tty_puts(ok ? "OK\r\n" : "FAILED\r\n");
    tty_puts(ok ? "\r\nINSTALL COMPLETE\r\nBoot from the installed IDE disk.\r\n"
                : "\r\nINSTALL FAILED\r\n");
    tty_puts("\r\nPress ENTER to reboot.\r\n");
    for (;;) {
        poll_keyboard();
        u8 k;
        while ((k = key_pop())) if (k == K_ENTER) { outb(0x64, 0xFE); for (;;) {} }
        pit_sleep_ms(1);
    }
}

/* ============================== MAIN ================================= */
__attribute__((section(".text.entry"))) void kernel_main(void)
{
    __asm__ volatile ("cld");
    read_video_info();
    pit_init();

    u8 boot_mode = *(volatile u8 *)0x501C;
    if (boot_mode == 1) tty_main();          /* real 80x25 text shell */
    if (boot_mode == 2) installer_main();    /* text-mode IDE installer */

    apply_requested_mode();         /* validated DISPI mode + serial report */
    init_palette();
    init_font();
    build_desktop();

    mouse_x = SW / 2;
    mouse_y = SH / 2;

    term_push("Welcome to PAPEROS!");
    term_push("Type 'help' and press Enter.");
    term_push("Try: mines, g2048, tetris, web, settings");
    open_win(0);
    open_win(3);
    wins[1].min = 1;               /* snake starts minimised */
    focus = 0;
    damage_all();
    mouse_init();
    net_init();
    fs_load();
    if (net_up) net_cmd_ping("10.0.2.2");   /* QEMU user-mode gateway */

    int oldb = 0;
    int prev_mx = mouse_x, prev_my = mouse_y, prev_sec = -1;
    u32 last_frame = 0, last_snake = 0, last_tet = 0;
    for (;;) {
        /* Drain all pending input first. Damage is accumulated as rectangles
           and published once, at most every ~16 ms. */
        poll_keyboard();
        poll_mouse();
        net_poll();

        if (mouse_x != prev_mx || mouse_y != prev_my || mouse_btns != oldb) {
            int cs = cursor_scale();
            mark_rect(prev_mx, prev_my, CUR_W * cs, CUR_H * cs);
            mark_rect(mouse_x, mouse_y, CUR_W * cs, CUR_H * cs);
        }
        update_menu_hover();

        int click = mouse_click;
        mouse_click = 0;

        /* start menu / taskbar click */
        if (click) {
            int on_start = mouse_x >= START_X &&
                           mouse_x < START_X + START_W &&
                           mouse_y >= TASKBAR_Y;
            if (start_open) {
                DmgRect mr;
                start_menu_rect(&mr);
                if (mouse_x >= mr.x0 && mouse_x < mr.x1 &&
                    mouse_y >= mr.y0 && mouse_y < mr.y1) {
                    int item = start_menu_item(mouse_x, mouse_y);
                    if (item >= 0) open_win(item);
                }
                start_open = 0;
                damage_all();
                update_menu_hover();
                click = 0;
            } else if (on_start) {
                start_open = 1;
                damage_all();
                update_menu_hover();
                click = 0;
            }
        }
        if (click && mouse_y >= TASKBAR_Y) {
            static const int types[MENU_ITEMS] = {0,1,2,3,4,5,6,7,8,9,10,11};
            int bw = task_button_w();
            for (int idx = 0; idx < MENU_ITEMS; idx++) {
                int bx = task_button_x(idx);
                if (mouse_x < bx || mouse_x >= bx + bw) continue;
                int w = find_win(types[idx]);
                if (w >= 0) { wins[w].min = 0; raise_win(w); }
                else open_win(types[idx]);
                break;
            }
            damage_all();
            click = 0;
        }

        /* dragging: mark only the old and new window positions */
        if (dragging && drag_win >= 0) {
            if (mouse_btns & 1) {
                Win *w = &wins[drag_win];
                int ox = w->x, oy = w->y;
                w->x = mouse_x - drag_dx;
                w->y = mouse_y - drag_dy;
                if (w->x < 0) w->x = 0;
                if (w->y < 0) w->y = 0;
                if (w->x + w->w > SW - 2) w->x = SW - w->w - 2;
                if (w->y + w->h > TASKBAR_Y - 2) w->y = TASKBAR_Y - w->h - 2;
                if (w->x != ox || w->y != oy) {
                    mark_window_at(drag_win, ox, oy);
                    mark_window(drag_win);
                }
            } else dragging = 0;
        }

        /* window hit testing, topmost first; any click raises that window */
        if (click) {
            int hit = -1;
            for (int i = nwin - 1; i >= 0; i--) {
                Win *w = &wins[i];
                if (!w->used || w->min) continue;
                if (mouse_x >= w->x && mouse_x < w->x + w->w &&
                    mouse_y >= w->y && mouse_y < w->y + w->h) { hit = i; break; }
            }
            if (hit >= 0) {
                Win *w = &wins[hit];
                int action = 0;         /* 1 minimise, 2 send behind, 3 close */
                if (mouse_y < w->y + TITLE_H) {
                    int minx, behindx, closex;
                    title_buttons(w, &minx, &behindx, &closex);
                    int by = w->y + 4;
                    if (mouse_x >= closex && mouse_x < closex + BTN_W &&
                        mouse_y >= by && mouse_y < by + BTN_H) action = 3;
                    else if (mouse_x >= behindx && mouse_x < behindx + BTN_W &&
                             mouse_y >= by && mouse_y < by + BTN_H) action = 2;
                    else if (mouse_x >= minx && mouse_x < minx + BTN_W &&
                             mouse_y >= by && mouse_y < by + BTN_H) action = 1;
                }
                if (action == 3) {
                    close_win(hit);
                } else if (action == 1) {
                    mark_window(hit);
                    wins[hit].min = 1;
                    focus = -1;
                    for (int j = nwin - 1; j >= 0; j--)
                        if (wins[j].used && !wins[j].min) { focus = j; break; }
                    damage_all();
                } else if (action == 2) {
                    lower_win(hit);
                } else {
                    raise_win(hit);
                    int top = nwin - 1;
                    w = &wins[top];
                    if (mouse_y < w->y + TITLE_H) {
                        dragging = 1; drag_win = top;
                        drag_dx = mouse_x - w->x; drag_dy = mouse_y - w->y;
                    } else {
                        int cx = CONTENT_X(w), cy = CONTENT_Y(w);
                        int cw = CONTENT_W(w), ch = CONTENT_H(w);
                        if (mouse_x >= cx && mouse_x < cx + cw &&
                            mouse_y >= cy && mouse_y < cy + ch)
                            app_click(top, mouse_x - cx, mouse_y - cy, cw, ch);
                    }
                }
            } else focus = -1;
            damage_all();            /* focus and button state affect all windows */
            click = 0;
        }

        /* keyboard to focused window - consume the whole queue here */
        {
            u8 k;
            while ((k = key_pop())) {
                if (k == K_ESC) {
                    start_open = !start_open;
                    damage_all();
                    update_menu_hover();
                    continue;
                }
                if (focus >= 0 && !wins[focus].min) {
                    app_key(focus, k);
                    mark_content(focus);
                }
            }
        }

        /* continuous mouse for paint */
        if (focus >= 0 && !wins[focus].min && wins[focus].type == 1 &&
            (mouse_btns & 1)) {
            Win *w = &wins[focus];
            app_mouse(focus, CONTENT_X(w), CONTENT_Y(w),
                      CONTENT_W(w), CONTENT_H(w));
            mark_content(focus);
        }

        /* snake timing: only step while the Snake window is actually visible */
        {
            int sw = find_win(3);
            if (sw >= 0 && !wins[sw].min) {
                u32 now = pit_ms();
                if (now - last_snake >= 150) {
                    last_snake = now;
                    snake_step();
                    mark_content(sw);
                }
            } else last_snake = pit_ms();
        }

        /* Brick Drop gravity runs only while its window is visible */
        {
            int bw = find_win(10);
            if (bw >= 0 && !wins[bw].min) {
                u32 now = pit_ms();
                u32 interval = 520u - (u32)tet_lines * 8u;
                if (interval < 120u) interval = 120u;
                if (now - last_tet >= interval) {
                    last_tet = now;
                    tet_step();
                    mark_content(bw);
                }
            } else last_tet = pit_ms();
        }

        /* system monitor refreshes twice a second */
        {
            static u32 last_mon = 0;
            int mw = find_win(7);
            if (mw >= 0 && !wins[mw].min && pit_ms() - last_mon >= 500) {
                last_mon = pit_ms();
                mark_content(mw);
            }
        }

        /* clock changes once a second */
        {
            int sec = bcd(cmos(0));
            if (sec != prev_sec) {
                prev_sec = sec;
                mark_rect(SW - 64, TASKBAR_Y, 64, TASKBAR_H);
            }
        }

        if (!damage_empty() && pit_ms() - last_frame >= 16) {
            redraw_damage();
            last_frame = pit_ms();
        } else {
            pit_sleep_ms(1);         /* idle at ~1 ms instead of busy-spinning */
        }
        prev_mx = mouse_x;
        prev_my = mouse_y;
        oldb = mouse_btns;
    }
}
