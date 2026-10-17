// Graphics: portrait 360x480 back buffer, rotated blit to the AP80 smart-LCD framebuffer, TrueType text.
#pragma once
#include <stdint.h>

#define SCR_W 360
#define SCR_H 480

enum { FONT_REG = 0, FONT_BOLD = 1 };

int gfx_init(const char *font_reg, const char *font_bold);
void gfx_present(void);
void gfx_sleep(int sleep);                 // 1 = panel sleep (unsafe: can hang GET_EMPTY_BUF), 0 = wake
int gfx_screenshot(const char *path);      // writes a binary PPM of the back buffer

void gfx_clear(uint32_t c);
void gfx_rect(int x, int y, int w, int h, uint32_t c);
void gfx_rrect(int x, int y, int w, int h, int r, uint32_t c);
void gfx_tri(int x0, int y0, int x1, int y1, int x2, int y2, uint32_t c);
void gfx_circle(int cx, int cy, int r, uint32_t c);
void gfx_clip(int x, int y, int w, int h);  // restrict drawing; gfx_clip(0,0,SCR_W,SCR_H) resets

int gfx_text_w(int font, int px, const char *s);
int gfx_line_h(int font, int px);
// Draws UTF-8 text with its top-left corner at (x, y). Truncates with an ellipsis to fit maxw (0 = no limit). Returns the width drawn.
int gfx_text(int font, int px, int x, int y, int maxw, uint32_t c, const char *s);
// Word-wraps into at most max_lines lines; the last line gets an ellipsis. Returns the number of lines drawn.
int gfx_text_wrap(int font, int px, int x, int y, int maxw, int max_lines, uint32_t c, const char *s);
