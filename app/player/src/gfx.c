#include "gfx.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb_truetype.h"

// Ingenic jzfb (smart LCD) private ioctls, recovered from stock hiby_player.
#define JZFB_SET_SLEEP     0x80044620
#define JZFB_GET_EMPTY_BUF 0x80044660
#define JZFB_START_DMA     0x80044661

static uint32_t back[SCR_W * SCR_H];
static uint32_t *fbmem; static size_t fblen; static int fbfd = -1;
int gfx_w = SCR_W, gfx_h = SCR_H; static int rot;
static int clip_x0, clip_y0, clip_x1 = SCR_W, clip_y1 = SCR_H;

// ---------- framebuffer ----------
int gfx_init(const char *font_reg, const char *font_bold);

void gfx_sleep(int s) { if (fbfd >= 0) ioctl(fbfd, JZFB_SET_SLEEP, &s); }

void gfx_present(void) {
    if (fbfd < 0) return;
    int idx = 0;
    ioctl(fbfd, JZFB_GET_EMPTY_BUF, &idx);
    size_t frame = (size_t)SCR_W * SCR_H;
    // jzfb page-aligns each buffer: buffer 1 starts at 0xA9000, not at frame * 4 (0xA8C00).
    size_t buf_bytes = (frame * 4 + 4095) & ~(size_t)4095;
    if (idx < 0 || idx * buf_bytes + frame * 4 > fblen) idx = 0;
    uint32_t *dst = (uint32_t *)((char *)fbmem + idx * buf_bytes);
    // The panel is landscape: 360 lines of 480 px. Portrait pixel (px, py) lives at fb[(359 - px) * 480 + py].
    switch (rot) {
    case ROT_PORTRAIT:       // px = x, py = y
        for (int x = 0; x < SCR_W; x++) { uint32_t *d = dst + (SCR_W - 1 - x) * SCR_H; const uint32_t *s = back + x; for (int y = 0; y < SCR_H; y++) d[y] = s[y * SCR_W]; }
        break;
    case ROT_PORTRAIT_FLIP:  // px = 359 - x, py = 479 - y
        for (int x = 0; x < SCR_W; x++) { uint32_t *d = dst + x * SCR_H + SCR_H - 1; const uint32_t *s = back + x; for (int y = 0; y < SCR_H; y++) d[-y] = s[y * SCR_W]; }
        break;
    case ROT_LANDSCAPE:      // 480x360 logical; px = 359 - y, py = x: a straight copy
        memcpy(dst, back, frame * 4);
        break;
    case ROT_LANDSCAPE_FLIP: // px = y, py = 479 - x
        for (int y = 0; y < SCR_W; y++) { uint32_t *d = dst + (SCR_W - 1 - y) * SCR_H + SCR_H - 1; const uint32_t *s = back + y * SCR_H; for (int x = 0; x < SCR_H; x++) d[-x] = s[x]; }
        break;
    }
    ioctl(fbfd, JZFB_START_DMA, 0);
}

void gfx_set_rotation(int r) {
    rot = r & 3;
    int land = rot == ROT_LANDSCAPE || rot == ROT_LANDSCAPE_FLIP;
    gfx_w = land ? SCR_H : SCR_W; gfx_h = land ? SCR_W : SCR_H;
    gfx_clip(0, 0, gfx_w, gfx_h);
}
int gfx_rotation(void) { return rot; }

// Touch reports portrait panel coordinates; convert to the current logical screen.
void gfx_map_touch(int tx, int ty, int *x, int *y) {
    switch (rot) {
    default:                 *x = tx;              *y = ty; break;
    case ROT_PORTRAIT_FLIP:  *x = SCR_W - 1 - tx;  *y = SCR_H - 1 - ty; break;
    case ROT_LANDSCAPE:      *x = ty;              *y = SCR_W - 1 - tx; break;
    case ROT_LANDSCAPE_FLIP: *x = SCR_H - 1 - ty;  *y = tx; break;
    }
}

int gfx_screenshot(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", gfx_w, gfx_h);
    for (int i = 0; i < gfx_w * gfx_h; i++) { uint32_t p = back[i]; fputc(p >> 16 & 255, f); fputc(p >> 8 & 255, f); fputc(p & 255, f); }
    fclose(f); return 0;
}

// ---------- primitives ----------
void gfx_clip(int x, int y, int w, int h) {
    clip_x0 = x < 0 ? 0 : x; clip_y0 = y < 0 ? 0 : y;
    clip_x1 = x + w > gfx_w ? gfx_w : x + w; clip_y1 = y + h > gfx_h ? gfx_h : y + h;
}
static inline void put(int x, int y, uint32_t c) {
    if (x >= clip_x0 && x < clip_x1 && y >= clip_y0 && y < clip_y1) back[y * gfx_w + x] = c;
}
static inline void blend(int x, int y, uint32_t c, int a) {
    if (x < clip_x0 || x >= clip_x1 || y < clip_y0 || y >= clip_y1 || a <= 0) return;
    if (a >= 255) { back[y * gfx_w + x] = c; return; }
    uint32_t d = back[y * gfx_w + x];
    uint32_t rb = ((c & 0xff00ff) * a + (d & 0xff00ff) * (255 - a)) >> 8 & 0xff00ff;
    uint32_t g = ((c & 0xff00) * a + (d & 0xff00) * (255 - a)) >> 8 & 0xff00;
    back[y * gfx_w + x] = rb | g;
}
void gfx_clear(uint32_t c) { for (int i = 0; i < SCR_W * SCR_H; i++) back[i] = c; }
void gfx_rect(int x, int y, int w, int h, uint32_t c) {
    int x0 = x < clip_x0 ? clip_x0 : x, x1 = x + w > clip_x1 ? clip_x1 : x + w;
    int y0 = y < clip_y0 ? clip_y0 : y, y1 = y + h > clip_y1 ? clip_y1 : y + h;
    for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) back[j * gfx_w + i] = c;
}
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c) {
    if (r * 2 > h) r = h / 2; if (r * 2 > w) r = w / 2;
    gfx_rect(x + r, y, w - 2 * r, h, c);
    gfx_rect(x, y + r, r, h - 2 * r, c); gfx_rect(x + w - r, y + r, r, h - 2 * r, c);
    for (int j = 0; j < r; j++) for (int i = 0; i < r; i++) {
        int dx = r - i, dy = r - j; int d2 = dx * dx + dy * dy;
        if (d2 > r * r) continue;
        put(x + i, y + j, c); put(x + w - 1 - i, y + j, c);
        put(x + i, y + h - 1 - j, c); put(x + w - 1 - i, y + h - 1 - j, c);
    }
}
void gfx_circle(int cx, int cy, int r, uint32_t c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) {
        int d2 = i * i + j * j;
        if (d2 <= (r - 1) * (r - 1)) put(cx + i, cy + j, c);
        else if (d2 <= r * r) blend(cx + i, cy + j, c, 128);
    }
}
void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c) {
    int minx = x0 < x1 ? (x0 < x2 ? x0 : x2) : (x1 < x2 ? x1 : x2), maxx = x0 > x1 ? (x0 > x2 ? x0 : x2) : (x1 > x2 ? x1 : x2);
    int miny = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2), maxy = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    long area = (long)(x1 - x0) * (y2 - y0) - (long)(x2 - x0) * (y1 - y0);
    if (!area) return;
    for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
        long w0 = (long)(x1 - x) * (y2 - y) - (long)(x2 - x) * (y1 - y);
        long w1 = (long)(x2 - x) * (y0 - y) - (long)(x0 - x) * (y2 - y);
        long w2 = (long)(x0 - x) * (y1 - y) - (long)(x1 - x) * (y0 - y);
        if (area > 0 ? (w0 >= 0 && w1 >= 0 && w2 >= 0) : (w0 <= 0 && w1 <= 0 && w2 <= 0)) put(x, y, c);
    }
}

// ---------- fonts ----------
static stbtt_fontinfo fonts[2]; static int font_ok[2];
typedef struct { uint32_t key; int w, h, xoff, yoff, adv; uint8_t *bmp; } glyph_t;
#define GCACHE 4096
static glyph_t gcache[GCACHE];

static void *load_file(const char *p) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    void *b = malloc(n); if (b && fread(b, 1, n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f); return b;
}

static const glyph_t *glyph(int font, int px, uint32_t cp) {
    uint32_t key = ((uint32_t)font << 30) | ((uint32_t)px << 22) | (cp & 0x3fffff);
    uint32_t h = (key * 2654435761u) % GCACHE;
    for (int probe = 0; probe < GCACHE; probe++, h = (h + 1) % GCACHE) {
        glyph_t *g = &gcache[h];
        if (g->key == key && (g->bmp || g->adv)) return g;
        if (!g->key) {
            stbtt_fontinfo *fi = &fonts[font_ok[font] ? font : 0];
            float sc = stbtt_ScaleForPixelHeight(fi, px);
            int gi = stbtt_FindGlyphIndex(fi, cp);
            if (!gi && cp != ' ') gi = stbtt_FindGlyphIndex(fi, '?');
            int adv, lsb; stbtt_GetGlyphHMetrics(fi, gi, &adv, &lsb);
            g->adv = (int)(adv * sc + 0.5f); if (!g->adv) g->adv = 1;
            g->bmp = stbtt_GetGlyphBitmap(fi, 0, sc, gi, &g->w, &g->h, &g->xoff, &g->yoff);
            g->key = key; return g;
        }
    }
    return &gcache[0];  // cache full: degrade gracefully
}

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *)*s; uint32_t c = *p++;
    if (c >= 0xf0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xe0 && (p[0] & 0xc0) == 0x80 && (p[1] & 0xc0) == 0x80) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xc0 && (p[0] & 0xc0) == 0x80) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p; return c;
}

int gfx_line_h(int font, int px) { (void)font; return px * 5 / 4; }

static int ascent_px(int font, int px) {
    stbtt_fontinfo *fi = &fonts[font_ok[font] ? font : 0];
    int a, d, l; stbtt_GetFontVMetrics(fi, &a, &d, &l);
    return (int)(a * stbtt_ScaleForPixelHeight(fi, px) + 0.5f);
}

static int text_w_n(int font, int px, const char *s, const char *end) {
    int w = 0; while (*s && (!end || s < end)) w += glyph(font, px, utf8_next(&s))->adv; return w;
}
int gfx_text_w(int font, int px, const char *s) { return text_w_n(font, px, s, NULL); }

static int draw_run(int font, int px, int x, int y, uint32_t c, const char *s, const char *end) {
    int base = y + ascent_px(font, px), x0 = x;
    while (*s && (!end || s < end)) {
        const glyph_t *g = glyph(font, px, utf8_next(&s));
        if (g->bmp) for (int j = 0; j < g->h; j++) for (int i = 0; i < g->w; i++)
            blend(x + g->xoff + i, base + g->yoff + j, c, g->bmp[j * g->w + i]);
        x += g->adv;
    }
    return x - x0;
}

int gfx_text(int font, int px, int x, int y, int maxw, uint32_t c, const char *s) {
    if (!maxw || gfx_text_w(font, px, s) <= maxw) return draw_run(font, px, x, y, c, s, NULL);
    const char *ell = "\xe2\x80\xa6"; int ew = gfx_text_w(font, px, ell), w = 0;
    const char *p = s, *cut = s;
    while (*p) { const char *q = p; int gw = glyph(font, px, utf8_next(&q))->adv; if (w + gw + ew > maxw) break; w += gw; p = q; cut = p; }
    int d = draw_run(font, px, x, y, c, s, cut);
    return d + draw_run(font, px, x + d, y, c, ell, NULL);
}

int gfx_text_wrap(int font, int px, int x, int y, int maxw, int max_lines, uint32_t c, const char *s) {
    int lines = 0, lh = gfx_line_h(font, px);
    while (*s && lines < max_lines) {
        if (lines == max_lines - 1 || gfx_text_w(font, px, s) <= maxw) { gfx_text(font, px, x, y + lines * lh, maxw, c, s); return lines + 1; }
        // longest prefix that fits, preferring to break at a space
        const char *p = s, *fit = s, *space = NULL; int w = 0;
        while (*p) { const char *q = p; uint32_t cp = utf8_next(&q); int gw = glyph(font, px, cp)->adv; if (w + gw > maxw) break; w += gw; if (cp == ' ') space = p; p = q; fit = p; }
        const char *brk = space && space > s ? space : fit;
        if (brk == s) brk = fit > s ? fit : s + 1;
        draw_run(font, px, x, y + lines * lh, c, s, brk);
        s = brk; while (*s == ' ') s++;
        lines++;
    }
    return lines;
}

int gfx_init(const char *font_reg, const char *font_bold) {
    void *r = load_file(font_reg), *b = load_file(font_bold);
    if (!r) { fprintf(stderr, "font %s missing\n", font_reg); return -1; }
    font_ok[0] = stbtt_InitFont(&fonts[0], r, 0);
    font_ok[1] = b && stbtt_InitFont(&fonts[1], b, 0);
    if (!font_ok[0]) return -1;
    fbfd = open("/dev/fb0", O_RDWR);
    if (fbfd < 0) { perror("fb0"); return 0; }  // allow headless runs
    struct fb_fix_screeninfo fi; ioctl(fbfd, FBIOGET_FSCREENINFO, &fi);
    fblen = fi.smem_len ? fi.smem_len : SCR_W * SCR_H * 4;
    fbmem = mmap(0, fblen, PROT_READ | PROT_WRITE, MAP_SHARED, fbfd, 0);
    if (fbmem == MAP_FAILED) { perror("mmap fb"); close(fbfd); fbfd = -1; return 0; }
    gfx_sleep(0);
    return 0;
}
