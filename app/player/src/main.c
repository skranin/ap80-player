// ap80player: a simple music player UI for the Hidizs AP80 Pro-X.
// Layout: status line on top, the selected page in the middle, and a tray with
// Library / Player / Settings at the bottom. Works in portrait and landscape.
#include "gfx.h"
#include "hw.h"
#include "audio.h"
#include "usb.h"
#include "bt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#define VERSION "0.1"
#define MUSIC_ROOT "/mnt/sd_0"
#define STATE_FILE "/mnt/sd_0/ap80dev/player_state.txt"
#define CMD_FIFO "/tmp/ap80p.cmd"
#define EXIT_TO_STOCK 20
#define EXIT_RESTART 21   // boot.sh restarts the player (used by deploy)

// ---------- colors ----------
#define C_BG     0x101216
#define C_PANEL  0x1c1f26
#define C_LINE   0x2a2e37
#define C_TEXT   0xffffff
#define C_DIM    0xb4b9c4
#define C_ACCENT 0xff8a3d
#define C_PRESS  0x2b303b

// ---------- settings / state ----------
static struct {
    int volume, max_volume, brightness, timeout_idx, filter, resume_play, auto_rotate, orientation, text_size, vol_step;
    char browse[PATH_MAXLEN];
    char queue_root[PATH_MAXLEN]; int queue_recursive;
    char track[PATH_MAXLEN]; double pos;
    int bt_on, bt_volume; char bt_mac[18], bt_name[64];   // Bluetooth output; volume is per output
    int autooff_idx;                                     // index into autooff_min
} cfg = { 25, 100, 120, 2, 0, 0, 0, ROT_PORTRAIT, 0, 1, MUSIC_ROOT, "", 0, "", 0, 0, 60, "", "", 1 };

static const int timeouts[] = { 15, 30, 60, 120, 300, 0 };
static const char *timeout_names[] = { "15 s", "30 s", "1 min", "2 min", "5 min", "Never" };
static const int autooff_min[] = { 5, 10, 15, 30, 60, 0 };   // power off after this long with nothing played or touched
static const char *autooff_names[] = { "5 min", "10 min", "15 min", "30 min", "1 hour", "Never" };
static const char *orient_names[] = { "Portrait", "Landscape", "Portrait, upside down", "Landscape, flipped" };
static const char *filter_names[] = { "Linear phase fast", "Linear phase slow", "Minimum phase fast", "Minimum phase slow",
                                      "Apodizing fast", "Apodizing slow", "Corrected min. fast", "Brick wall" };

static int usb_mode;          // 0 = normal, 1 = "connected to computer" prompt, 2 = SD card exported over USB
static int usb_prompt_done;   // prompt already answered for this cable connection

static void save_state(void) {
    if (usb_mode == 2) return;  // SD card belongs to the computer
    audio_status_t st; audio_status(&st);
    if (st.has_queue) { snprintf(cfg.track, sizeof cfg.track, "%s", st.path); cfg.pos = st.pos; }
    char tmp[] = STATE_FILE ".tmp";
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "volume=%d\nmax_volume=%d\nbrightness=%d\ntimeout=%d\nfilter=%d\nresume_play=%d\nauto_rotate=%d\norientation=%d\ntext_size=%d\nvol_step=%d\nbrowse=%s\nqueue_root=%s\nqueue_recursive=%d\ntrack=%s\npos=%.1f\nbt_on=%d\nbt_volume=%d\nbt_mac=%s\nbt_name=%s\nauto_off=%d\n",
            cfg.volume, cfg.max_volume, cfg.brightness, cfg.timeout_idx, cfg.filter, cfg.resume_play, cfg.auto_rotate, cfg.orientation, cfg.text_size, cfg.vol_step, cfg.browse,
            cfg.queue_root, cfg.queue_recursive, cfg.track, cfg.pos, cfg.bt_on, cfg.bt_volume, cfg.bt_mac, cfg.bt_name, cfg.autooff_idx);
    fclose(f); rename(tmp, STATE_FILE);
}

static void load_state(void) {
    FILE *f = fopen(STATE_FILE, "r"); if (!f) return;
    char line[PATH_MAXLEN + 32];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, '='); if (!v) continue; *v++ = 0;
        if (!strcmp(line, "volume")) cfg.volume = atoi(v);
        else if (!strcmp(line, "max_volume")) cfg.max_volume = atoi(v);
        else if (!strcmp(line, "brightness")) cfg.brightness = atoi(v);
        else if (!strcmp(line, "timeout")) cfg.timeout_idx = atoi(v);
        else if (!strcmp(line, "filter")) cfg.filter = atoi(v);
        else if (!strcmp(line, "resume_play")) cfg.resume_play = atoi(v);
        else if (!strcmp(line, "auto_rotate")) cfg.auto_rotate = atoi(v);
        else if (!strcmp(line, "orientation")) cfg.orientation = atoi(v);
        else if (!strcmp(line, "text_size")) cfg.text_size = atoi(v);
        else if (!strcmp(line, "vol_step")) cfg.vol_step = atoi(v);
        else if (!strcmp(line, "browse")) snprintf(cfg.browse, sizeof cfg.browse, "%s", v);
        else if (!strcmp(line, "queue_root")) snprintf(cfg.queue_root, sizeof cfg.queue_root, "%s", v);
        else if (!strcmp(line, "queue_recursive")) cfg.queue_recursive = atoi(v);
        else if (!strcmp(line, "track")) snprintf(cfg.track, sizeof cfg.track, "%s", v);
        else if (!strcmp(line, "pos")) cfg.pos = atof(v);
        else if (!strcmp(line, "bt_on")) cfg.bt_on = atoi(v) != 0;
        else if (!strcmp(line, "bt_volume")) cfg.bt_volume = atoi(v);
        else if (!strcmp(line, "bt_mac")) snprintf(cfg.bt_mac, sizeof cfg.bt_mac, "%s", v);
        else if (!strcmp(line, "bt_name")) snprintf(cfg.bt_name, sizeof cfg.bt_name, "%s", v);
        else if (!strcmp(line, "auto_off")) cfg.autooff_idx = atoi(v);
    }
    fclose(f);
    if (cfg.max_volume < 10 || cfg.max_volume > 100) cfg.max_volume = 100;
    if (cfg.volume < 0) cfg.volume = 0; if (cfg.volume > cfg.max_volume) cfg.volume = cfg.max_volume;
    if (cfg.bt_volume < 0) cfg.bt_volume = 0; if (cfg.bt_volume > cfg.max_volume) cfg.bt_volume = cfg.max_volume;
    if (cfg.brightness < 5 || cfg.brightness > 255) cfg.brightness = 120;
    if (cfg.timeout_idx < 0 || cfg.timeout_idx > 5) cfg.timeout_idx = 2;
    if (cfg.autooff_idx < 0 || cfg.autooff_idx > 5) cfg.autooff_idx = 1;
    cfg.filter &= 7; cfg.orientation &= 3; cfg.auto_rotate = !!cfg.auto_rotate; cfg.text_size &= 3;
    if (cfg.vol_step < 1 || cfg.vol_step > 3) cfg.vol_step = 1;
}

// ---------- library ----------
typedef struct { char *name; int is_dir; } entry_t;
static entry_t *ents; static int nents, has_audio_here, lib_gen;

static int hidden(const char *n) {
    return n[0] == '.' || !strcmp(n, "ap80dev") || !strcmp(n, "System Volume Information") || !strcmp(n, "$RECYCLE.BIN") || !strcmp(n, "LOST.DIR");
}
static int ent_cmp(const void *a, const void *b) {
    const entry_t *x = a, *y = b;
    if (x->is_dir != y->is_dir) return y->is_dir - x->is_dir;
    return strcasecmp(x->name, y->name);
}
static int is_dir_path(const char *p) { struct stat s; return !stat(p, &s) && S_ISDIR(s.st_mode); }

static void lib_load(const char *dir) {
    for (int i = 0; i < nents; i++) free(ents[i].name);
    free(ents); ents = NULL; nents = 0; has_audio_here = 0;
    DIR *d = opendir(dir); if (!d) return;
    int cap = 0; struct dirent *e; char p[PATH_MAXLEN];
    while ((e = readdir(d))) {
        if (hidden(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        int isd = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && is_dir_path(p));
        if (!isd && !audio_is_audio_file(e->d_name)) continue;
        if (nents == cap) { cap = cap ? cap * 2 : 64; ents = realloc(ents, cap * sizeof *ents); }
        ents[nents++] = (entry_t){ strdup(e->d_name), isd };
        if (!isd) has_audio_here = 1;
    }
    closedir(d);
    qsort(ents, nents, sizeof *ents, ent_cmp);
    lib_gen++;
}

typedef struct { char **v; int n, cap; } strvec_t;
static void sv_push(strvec_t *s, const char *p) { if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 64; s->v = realloc(s->v, s->cap * sizeof(char *)); } s->v[s->n++] = strdup(p); }
static int str_cmp(const void *a, const void *b) { return strcasecmp(*(char *const *)a, *(char *const *)b); }

// Collects audio files under dir (sorted; subfolders after the files of a folder when recursive).
static void collect(const char *dir, int recursive, strvec_t *out, int depth) {
    if (depth > 8 || out->n > 20000) return;
    DIR *d = opendir(dir); if (!d) return;
    strvec_t files = { 0 }, dirs = { 0 }; struct dirent *e; char p[PATH_MAXLEN];
    while ((e = readdir(d))) {
        if (hidden(e->d_name)) continue;
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        int isd = e->d_type == DT_DIR || (e->d_type == DT_UNKNOWN && is_dir_path(p));
        if (isd) { if (recursive) sv_push(&dirs, p); }
        else if (audio_is_audio_file(e->d_name)) sv_push(&files, p);
    }
    closedir(d);
    qsort(files.v, files.n, sizeof(char *), str_cmp); qsort(dirs.v, dirs.n, sizeof(char *), str_cmp);
    for (int i = 0; i < files.n; i++) { sv_push(out, files.v[i]); free(files.v[i]); }
    for (int i = 0; i < dirs.n; i++) { collect(dirs.v[i], 1, out, depth + 1); free(dirs.v[i]); }
    free(files.v); free(dirs.v);
}

static int start_queue(const char *root_in, int recursive, const char *track, double pos, int autoplay) {
    char root[PATH_MAXLEN]; snprintf(root, sizeof root, "%s", root_in);  // root_in may alias cfg.queue_root
    strvec_t s = { 0 }; collect(root, recursive, &s, 0);
    if (!s.n) { free(s.v); return -1; }
    int idx = 0;
    if (track) for (int i = 0; i < s.n; i++) if (!strcmp(s.v[i], track)) { idx = i; break; }
    if (track && strcmp(s.v[idx], track)) pos = 0;
    snprintf(cfg.queue_root, sizeof cfg.queue_root, "%s", root); cfg.queue_recursive = recursive;
    audio_set_queue(s.v, s.n, idx, pos, autoplay);  // the engine now owns s.v and its strings
    return 0;
}

// ---------- UI state ----------
enum { SCR_LIBRARY, SCR_PLAYER, SCR_SETTINGS, SCR_COUNT };   // also the order of the tray buttons
static int screen = SCR_LIBRARY;
static int screen_on = 1, dirty = 1;
static long last_input_ms, toast_until_ms; static char toast[128];
static int battery = -1, charging, headset;
static int scroll[SCR_COUNT], content_h[SCR_COUNT];   // per-page vertical scroll (px)
static int touch_active, touch_dragging, touch_x0, touch_y0, touch_scroll0, press_id = -1;
static int scrub_active, scrub_x;                  // dragging the seek bar
static int settings_page;                          // 0 = settings list, 1 = Bluetooth
static bt_status_t bts;                            // latest Bluetooth status
static int out_bt;                                 // playing to Bluetooth (else the jack)
static int forget_armed = -1; static long forget_until;   // "Forget" needs a second tap
static char pending_select[18];                    // use this device once pairing finishes

typedef struct { int x, y, w, h, id, arg; } button_t;
static button_t buttons[128]; static int nbuttons;
enum { B_NONE, B_BACK_DIR, B_PLAY_ALL, B_ENTRY, B_TAB, B_PLAYPAUSE, B_NEXT, B_PREV,
       B_SEEK, B_SET_DEC, B_SET_INC, B_SET_CYCLE, B_STOCK, B_POWEROFF, B_USB_STORAGE, B_USB_CHARGE, B_USB_DONE,
       B_BT_BACK, B_BT_POWER, B_BT_DEV, B_BT_FORGET, B_BT_SCAN, B_BT_FOUND };
enum { S_BT, S_TEXTSIZE, S_BRIGHT, S_TIMEOUT, S_AUTOOFF, S_AUTOROT, S_ORIENT, S_MAXVOL, S_VOLSTEP, S_FILTER, S_RESUME };

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }
static void add_button(int x, int y, int w, int h, int id, int arg) { if (nbuttons < 128) buttons[nbuttons++] = (button_t){ x, y, w, h, id, arg }; }
static button_t *hit(int x, int y) {
    for (int i = nbuttons - 1; i >= 0; i--) { button_t *b = &buttons[i]; if (x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h) return b; }
    return NULL;
}
static void show_toast(const char *s) { snprintf(toast, sizeof toast, "%s", s); toast_until_ms = now_ms() + 1300; dirty = 1; }
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static void strip_ext(char *s) { char *e = strrchr(s, '.'); if (e && e != s) *e = 0; }
static void fmt_time(char *o, int n, double t) { int s = t < 0 ? 0 : (int)t; snprintf(o, n, "%d:%02d", s / 60, s % 60); }

// ---------- layout ----------
// All sizes follow the "Text size" setting (gfx text scale). T(px) is a font size after scaling.
#define T(px) gfx_text_px(px)
#define LH(font, px) gfx_line_h(font, px)
static int landscape(void) { return gfx_w > gfx_h; }
// Status line and tray labels grow less, so three tray buttons still fit side by side.
static int cap_scale(int cap) { int old = gfx_text_scale(); if (old > cap) gfx_set_text_scale(cap); return old; }
static int status_h(void) { int o = cap_scale(125); int h = LH(FONT_BOLD, 19) + 11; gfx_set_text_scale(o); return h; }
static int tray_h(void) {
    int o = cap_scale(125), lh = LH(FONT_BOLD, 19); gfx_set_text_scale(o);
    return landscape() ? (lh + 46 > 72 ? lh + 46 : 72) : 58 + lh + 10;
}
static int content_y(void) { return status_h(); }
static int content_bottom(void) { return usb_mode ? gfx_h : gfx_h - tray_h(); }

// ---------- drawing helpers ----------
static void draw_chevron_left(int x, int y, int s, uint32_t c, uint32_t bg) {
    gfx_tri(x + s / 2, y - s / 2, x, y, x + s / 2, y + s / 2, c);
    gfx_tri(x + s / 2 + 1, y - s / 2 + 5, x + 6, y, x + s / 2 + 1, y + s / 2 - 5, bg);
}
static void draw_play_icon(int cx, int cy, int s, uint32_t c) { gfx_tri(cx - s / 3, cy - s / 2, cx - s / 3, cy + s / 2, cx + s * 2 / 3 - s / 6, cy, c); }
static void draw_pause_icon(int cx, int cy, int s, uint32_t c) { gfx_rect(cx - s / 2, cy - s / 2, s / 3, s, c); gfx_rect(cx + s / 6, cy - s / 2, s / 3, s, c); }
static void draw_skip_icon(int cx, int cy, int s, int dir, uint32_t c) {
    if (dir > 0) { gfx_tri(cx - s / 2, cy - s / 2, cx - s / 2, cy + s / 2, cx + s / 3, cy, c); gfx_rect(cx + s / 3, cy - s / 2, s / 6 + 1, s, c); }
    else { gfx_tri(cx + s / 2, cy - s / 2, cx + s / 2, cy + s / 2, cx - s / 3, cy, c); gfx_rect(cx - s / 3 - s / 6, cy - s / 2, s / 6 + 1, s, c); }
}
// Folder icon, s = width.
static void draw_folder_icon(int x, int y, int s, uint32_t c) { gfx_rrect(x, y, s * 2 / 5, s / 4, 3, c); gfx_rrect(x, y + s / 8, s, s * 5 / 8, 4, c); }
// Top y that vertically centers a line of text of this size on cy.
static int vc(int px, int cy) { return cy - T(px) * 29 / 50; }
static void text_vc(int font, int px, int x, int cy, int maxw, uint32_t c, const char *s) { gfx_text(font, px, x, vc(px, cy), maxw, c, s); }
static void text_center(int font, int px, int cx, int cy, int maxw, uint32_t c, const char *s) {
    int w = gfx_text_w(font, px, s); if (maxw && w > maxw) w = maxw;
    gfx_text(font, px, cx - w / 2, vc(px, cy), maxw, c, s);
}
// A full-width rounded button whose label wraps to fit. Returns its height.
static int big_button(int x, int y, int w, uint32_t bg, int font, uint32_t fg, const char *label, int id, int draw) {
    int px = 22, n = gfx_text_lines(font, px, w - 36, 2, label), lh = LH(font, px);
    int h = n * lh + 22; if (h < 56) h = 56;
    if (draw) {
        gfx_rrect(x, y, w, h, 12, bg);
        gfx_text_wrap(font, px, x + 18, y + (h - n * lh) / 2 + (lh - T(px)) / 4, w - 36, 2, fg, label);
        add_button(x, y, w, h, id, 0);
    }
    return h;
}

static int bt_connected(const char *mac) {
    for (int i = 0; i < bts.npaired; i++) if (!strcasecmp(bts.paired[i].mac, mac)) return bts.paired[i].connected;
    return 0;
}

static void draw_status_bar(void) {
    int o = cap_scale(125), h = status_h(), cy = h / 2;
    gfx_rect(0, 0, gfx_w, h, C_PANEL);
    char s[64];
    audio_status_t st; audio_status(&st);
    int x = 12;
    if (st.has_queue) { if (st.playing) draw_play_icon(x + 8, cy, 16, C_ACCENT); else draw_pause_icon(x + 8, cy, 14, C_DIM); x += 26; }
    snprintf(s, sizeof s, "Vol %d", out_bt ? cfg.bt_volume : cfg.volume);
    x += gfx_text(FONT_BOLD, 19, x, vc(19, cy), 0, C_TEXT, s) + 12;
    int right = gfx_w - 12;
    if (battery >= 0) {
        snprintf(s, sizeof s, "%s%d%%", charging ? "+" : "", battery);
        int w = gfx_text_w(FONT_BOLD, 19, s), bx = gfx_w - 40;
        gfx_text(FONT_BOLD, 19, bx - w - 8, vc(19, cy), 0, C_TEXT, s);
        gfx_rect(bx, cy - 8, 28, 16, C_DIM); gfx_rect(bx + 2, cy - 6, 24, 12, C_PANEL); gfx_rect(bx + 28, cy - 4, 3, 8, C_DIM);
        gfx_rect(bx + 3, cy - 5, 22 * battery / 100, 10, battery <= 15 ? 0xe04848 : C_TEXT);
        right = bx - w - 16;
    }
    if (out_bt && right - x > 30) {
        snprintf(s, sizeof s, "BT %s", cfg.bt_name[0] ? cfg.bt_name : "headphones");
        gfx_text(FONT_REG, 17, x, vc(17, cy), right - x, bt_connected(cfg.bt_mac) ? C_ACCENT : C_DIM, s);
    } else if (!headset && right - x > 30) gfx_text(FONT_REG, 17, x, vc(17, cy), right - x, C_ACCENT, "no headphones");
    gfx_set_text_scale(o);
}

// Bottom tray: Library / Player / Settings.
static void draw_tray(void) {
    int o = cap_scale(125);
    int th = tray_h(), y = gfx_h - th, w = gfx_w / 3, lh = LH(FONT_BOLD, 19);
    gfx_rect(0, y, gfx_w, th, C_PANEL);
    gfx_rect(0, y, gfx_w, 1, C_LINE);
    static const char *labels[] = { "Library", "Player", "Settings" };
    for (int i = 0; i < 3; i++) {
        int x = i * w, active = screen == i, cx = x + w / 2;
        uint32_t c = active ? C_ACCENT : C_DIM;
        if (active) { gfx_rect(x + 12, y, w - 24, 4, C_ACCENT); }
        int isz = 30, lw = gfx_text_w(FONT_BOLD, 19, labels[i]);
        int icx, icy, lx, ly, lmax;
        if (landscape()) {
            lmax = w - isz - 22; if (lw > lmax) lw = lmax;
            int tot = isz + 10 + lw; icx = cx - tot / 2 + isz / 2; icy = y + th / 2; lx = icx + isz / 2 + 10; ly = vc(19, y + th / 2);
        } else { lmax = w - 8; if (lw > lmax) lw = lmax; icx = cx; icy = y + 32; lx = cx - lw / 2; ly = y + 56 + (lh - T(19)) / 4; }
        if (i == SCR_PLAYER) { gfx_circle(icx, icy, isz / 2, c); draw_play_icon(icx + 2, icy, isz / 2, C_PANEL); }
        else if (i == SCR_LIBRARY) draw_folder_icon(icx - isz / 2, icy - isz * 3 / 8, isz, c);
        else {  // U+2699 gear, icon-sized regardless of text size
            gfx_set_text_scale(100);
            gfx_text(FONT_REG, 36, icx - gfx_text_w(FONT_REG, 36, "\xe2\x9a\x99") / 2, icy - 23, 0, c, "\xe2\x9a\x99");
            gfx_set_text_scale(o > 125 ? 125 : o);
        }
        gfx_text(FONT_BOLD, 19, lx, ly, lmax, active ? C_ACCENT : C_TEXT, labels[i]);
        add_button(x, y, w, th, B_TAB, i);
    }
    gfx_set_text_scale(o);
}

// Clamps a page's scroll and draws its scrollbar. view = visible height.
static void finish_scroll(int page, int top, int view) {
    int max = content_h[page] - view; if (max < 0) max = 0;
    if (scroll[page] > max) scroll[page] = max; if (scroll[page] < 0) scroll[page] = 0;
    if (max > 0) {
        int th = view * view / content_h[page]; if (th < 40) th = 40;
        gfx_rrect(gfx_w - 6, top + (view - th) * scroll[page] / max, 4, th, 2, C_DIM);
    }
}

// ---------- pages ----------
#define LIST_X 70         // text column in lists
#define NAME_PX 22

// Library rows can wrap a long name onto two lines, so heights vary. They are cached and
// recomputed when the folder, text size or orientation changes.
static int *row_top, row_cache_n = -1, row_cache_w, row_cache_scale, row_cache_gen;
static int lib_row_h(int lines) { int h = lines * LH(FONT_REG, NAME_PX) + 26; return h < 70 ? 70 : h; }
static void lib_layout(int at_root) {
    int rows = nents + (nents ? 1 : 0);
    if (row_cache_n == rows && row_cache_w == gfx_w && row_cache_scale == gfx_text_scale() && row_cache_gen == lib_gen) return;
    free(row_top); row_top = malloc((rows + 1) * sizeof(int));
    int y = 0;
    for (int r = 0; r < rows; r++) {
        row_top[r] = y;
        int lines = 1;
        if (r == 0) lines = gfx_text_lines(FONT_BOLD, NAME_PX, gfx_w - LIST_X - 20, 2, has_audio_here || at_root ? "Play all" : "Play all (with subfolders)");
        else {
            entry_t *e = &ents[r - 1]; char name[512]; snprintf(name, sizeof name, "%s", e->name);
            if (!e->is_dir) strip_ext(name);
            lines = gfx_text_lines(FONT_REG, NAME_PX, gfx_w - LIST_X - (e->is_dir ? 40 : 16), 2, name);
        }
        y += lib_row_h(lines);
    }
    row_top[rows] = y;
    row_cache_n = rows; row_cache_w = gfx_w; row_cache_scale = gfx_text_scale(); row_cache_gen = lib_gen;
}

static void draw_library(void) {
    audio_status_t st; audio_status(&st);
    int at_root = !strcmp(cfg.browse, MUSIC_ROOT), top = content_y(), bot = content_bottom();
    // header: back + folder name
    int hh = LH(FONT_BOLD, 24) + 26; if (hh < 60) hh = 60;
    int tx = 18;
    if (!at_root) { draw_chevron_left(14, top + hh / 2, 26, C_ACCENT, C_BG); add_button(0, top, 80, hh, B_BACK_DIR, 0); tx = 52; }
    text_vc(FONT_BOLD, 24, tx, top + hh / 2, gfx_w - tx - 14, C_TEXT, at_root ? "Library" : base_name(cfg.browse));
    gfx_rect(0, top + hh - 1, gfx_w, 1, C_LINE);
    int ly = top + hh, view = bot - ly;
    int rows = nents + (nents ? 1 : 0);
    lib_layout(at_root);
    content_h[SCR_LIBRARY] = row_top[rows];
    finish_scroll(SCR_LIBRARY, ly, view);
    gfx_clip(0, ly, gfx_w - 8, view);
    if (!nents) gfx_text_wrap(FONT_REG, 21, 20, ly + 30, gfx_w - 40, 3, C_DIM, "No music in this folder");
    int nlh = LH(FONT_REG, NAME_PX);
    for (int r = 0; r < rows; r++) {
        int y = ly + row_top[r] - scroll[SCR_LIBRARY], rh = row_top[r + 1] - row_top[r];
        if (y + rh < ly) continue;
        if (y > bot) break;
        int id = r == 0 ? B_PLAY_ALL : B_ENTRY, arg = r - 1, mid = y + rh / 2;
        if (r == 0) {
            const char *label = has_audio_here || at_root ? "Play all" : "Play all (with subfolders)";
            int n = gfx_text_lines(FONT_BOLD, NAME_PX, gfx_w - LIST_X - 20, 2, label);
            gfx_circle(36, mid, 20, C_ACCENT); draw_play_icon(38, mid, 16, C_BG);
            gfx_text_wrap(FONT_BOLD, NAME_PX, LIST_X, vc(NAME_PX, mid) - (n - 1) * nlh / 2, gfx_w - LIST_X - 20, 2, C_ACCENT, label);
        } else {
            entry_t *e = &ents[arg];
            char name[512]; snprintf(name, sizeof name, "%s", e->name);
            int playing_this = 0;
            if (e->is_dir) draw_folder_icon(20, mid - 12, 32, C_DIM);
            else {
                char full[PATH_MAXLEN]; snprintf(full, sizeof full, "%s/%s", cfg.browse, e->name);
                playing_this = st.has_queue && !strcmp(full, st.path);
                strip_ext(name);
                int o = gfx_text_scale(); gfx_set_text_scale(100);
                gfx_text(FONT_REG, 28, 24, mid - 18, 0, playing_this ? C_ACCENT : C_DIM, "\xe2\x99\xaa");  // U+266A note
                gfx_set_text_scale(o);
            }
            int maxw = gfx_w - LIST_X - (e->is_dir ? 40 : 16);
            int n = gfx_text_lines(FONT_REG, NAME_PX, maxw, 2, name);
            gfx_text_wrap(FONT_REG, NAME_PX, LIST_X, vc(NAME_PX, mid) - (n - 1) * nlh / 2, maxw, 2, playing_this ? C_ACCENT : C_TEXT, name);
            if (e->is_dir) { int o = gfx_text_scale(); gfx_set_text_scale(100); gfx_text(FONT_BOLD, 28, gfx_w - 32, mid - 18, 0, C_DIM, "\xe2\x80\xba"); gfx_set_text_scale(o); }  // U+203A ›
        }
        gfx_rect(LIST_X, y + rh - 1, gfx_w - LIST_X, 1, C_LINE);
        add_button(0, y < ly ? ly : y, gfx_w, rh - (y < ly ? ly - y : 0), id, arg);
    }
    gfx_clip(0, 0, gfx_w, gfx_h);
}

static void draw_player(void) {
    audio_status_t st; audio_status(&st);
    int top = content_y(), bot = content_bottom(), land = landscape(), m = 20, w = gfx_w - 2 * m;
    if (!st.has_queue) {
        int y = top + 30;
        y += gfx_text_wrap(FONT_BOLD, 26, m, y, w, 2, C_TEXT, "Nothing is playing") * LH(FONT_BOLD, 26) + 12;
        gfx_text_wrap(FONT_REG, 21, m, y, w, 4, C_DIM, "Pick a song or a folder in the Library.");
        return;
    }
    // bottom-up: controls, seek bar, format line
    int ctl_h = land ? 78 : 104, r = land ? 32 : 42;
    int cy = bot - ctl_h / 2 - (land ? 2 : 6);
    int gap = land ? 120 : 112, cx = gfx_w / 2;
    draw_skip_icon(cx - gap, cy, 30, -1, C_TEXT); add_button(cx - gap - 56, cy - 44, 112, 88, B_PREV, 0);
    gfx_circle(cx, cy, r, C_ACCENT);
    if (st.playing) draw_pause_icon(cx, cy, r * 2 / 3, C_BG); else draw_play_icon(cx + 3, cy, r * 3 / 4, C_BG);
    add_button(cx - r - 14, cy - r - 10, 2 * r + 28, 2 * r + 20, B_PLAYPAUSE, 0);
    draw_skip_icon(cx + gap, cy, 30, 1, C_TEXT); add_button(cx + gap - 56, cy - 44, 112, 88, B_NEXT, 0);

    int tlh19 = LH(FONT_BOLD, 19);
    int py = cy - r - 18 - tlh19 - (land ? 4 : 14);   // seek bar, with the times under it
    double frac = st.dur > 0 ? st.pos / st.dur : 0;
    if (scrub_active) frac = (double)(scrub_x - m) / w;   // follow the finger until release
    if (frac > 1) frac = 1; if (frac < 0) frac = 0;
    gfx_rrect(m, py, w, 8, 4, C_LINE); gfx_rrect(m, py, (int)(w * frac), 8, 4, C_ACCENT);
    gfx_circle(m + (int)(w * frac), py + 4, scrub_active ? 15 : 10, scrub_active ? C_ACCENT : C_TEXT);
    add_button(0, py - 20, gfx_w, 48, B_SEEK, 0);
    char t1[16], t2[16]; fmt_time(t1, sizeof t1, scrub_active ? frac * st.dur : st.pos); fmt_time(t2, sizeof t2, st.dur);
    gfx_text(FONT_BOLD, 19, m, py + 16, 0, scrub_active ? C_ACCENT : C_DIM, t1);
    gfx_text(FONT_BOLD, 19, m + w - gfx_text_w(FONT_BOLD, 19, t2), py + 16, 0, C_DIM, t2);

    char info[96];
    if (st.rate) snprintf(info, sizeof info, "%d / %d   \xc2\xb7   %s %.1f kHz %d bit", st.index + 1, st.count, st.format, st.rate / 1000.0, st.bits);
    else snprintf(info, sizeof info, "%d / %d", st.index + 1, st.count);
    int tpx = land ? 26 : 30, spx = land ? 20 : 22, y = top + (land ? 10 : 18);
    // The format line (and an error) go above the seek bar if the title still fits above them.
    int ilh = LH(FONT_REG, 18), iy = py - 16 - ilh;
    if (iy - 6 - y >= LH(FONT_BOLD, tpx)) gfx_text(FONT_REG, 18, m, iy, w, C_DIM, info);
    else iy = py - 12;
    if (st.error[0] && iy - ilh - 10 - y >= LH(FONT_BOLD, tpx)) { iy -= ilh + 4; gfx_text(FONT_REG, 18, m, iy, w, 0xe04848, st.error); }

    // top: title, artist - album, folder (as much as fits)
    char title[300], line2[600], line3[600];
    if (st.title[0]) snprintf(title, sizeof title, "%s", st.title);
    else { snprintf(title, sizeof title, "%s", base_name(st.path)); strip_ext(title); }
    if (st.artist[0] && st.album[0]) snprintf(line2, sizeof line2, "%s \xe2\x80\x94 %s", st.artist, st.album);
    else snprintf(line2, sizeof line2, "%s", st.artist[0] ? st.artist : st.album);
    char dir[PATH_MAXLEN]; snprintf(dir, sizeof dir, "%s", st.path); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    snprintf(line3, sizeof line3, "%s", base_name(dir));
    int lim = iy - 6;
    while (tpx > 16 && LH(FONT_BOLD, tpx) > lim - y) tpx -= 2;   // a big text size in landscape: shrink the title to fit
    int tlh = LH(FONT_BOLD, tpx), slh = LH(FONT_REG, spx);
    int extra = line2[0] ? slh : 0;                       // keep room for at least the artist line
    int tl = (lim - y - extra) / tlh; if (tl > 3) tl = 3; if (tl < 1) tl = 1;
    y += gfx_text_wrap(FONT_BOLD, tpx, m, y, w, tl, C_TEXT, title) * tlh + 6;
    if (line2[0] && y + slh <= lim) {
        int l2 = (lim - y) / slh; if (l2 > 2) l2 = 2; if (l2 < 1) l2 = 1;
        y += gfx_text_wrap(FONT_REG, spx, m, y, w, l2, C_DIM, line2) * slh + 2;
    }
    if ((!line2[0] || strcmp(line3, st.album)) && y + LH(FONT_REG, spx - 2) <= lim) gfx_text(FONT_REG, spx - 2, m, y, w, C_DIM, line3);
}

static const int text_sizes[] = { 100, 125, 150, 175 };
static const char *text_size_names[] = { "Normal", "Large", "Larger", "Largest" };

#define LABEL_PX 22
#define VALUE_PX 20
static void draw_bt_page(void);
static void draw_settings(void) {
    if (settings_page) { draw_bt_page(); return; }
    int top = content_y(), bot = content_bottom(), view = bot - top;
    char v[64];
    struct { const char *label; int kind, id; } rows[] = {   // kind 1 = -/+, 0 = tap to change
        { "Bluetooth", 0, S_BT }, { "Text size", 0, S_TEXTSIZE }, { "Brightness", 1, S_BRIGHT }, { "Screen off after", 0, S_TIMEOUT }, { "Auto power off", 0, S_AUTOOFF },
        { "Auto-rotate", 0, S_AUTOROT }, { "Orientation", 0, S_ORIENT }, { "Max volume", 1, S_MAXVOL }, { "Volume step", 0, S_VOLSTEP },
        { "DAC filter", 0, S_FILTER }, { "On start", 0, S_RESUME },
    };
    int nrows = sizeof rows / sizeof rows[0], m = 18, bw = gfx_w - 2 * m;
    int llh = LH(FONT_REG, LABEL_PX), vlh = LH(FONT_BOLD, VALUE_PX);
    int bs = T(30) + 18; if (bs < 48) bs = 48;             // -/+ button size
    int ctl_w = 2 * bs + T(22) * 3 + 20;                   // "-  100%  +"
    // pass 0 measures, pass 1 draws
    int y0 = top - scroll[SCR_SETTINGS], y = y0;
    for (int pass = 0; pass < 2; pass++) {
        y = y0;
        if (pass) { finish_scroll(SCR_SETTINGS, top, view); gfx_clip(0, top, gfx_w - 8, view); y = y0 = top - scroll[SCR_SETTINGS]; }
        for (int i = 0; i < nrows; i++) {
            int dim = 0;
            switch (rows[i].id) {
            case S_BT: snprintf(v, sizeof v, "%s \xe2\x80\xba", !cfg.bt_on ? "Off" : cfg.bt_mac[0] ? cfg.bt_name : "On"); break;
            case S_TEXTSIZE: snprintf(v, sizeof v, "%s", text_size_names[cfg.text_size]); break;
            case S_BRIGHT: snprintf(v, sizeof v, "%d%%", cfg.brightness * 100 / 255); break;
            case S_TIMEOUT: snprintf(v, sizeof v, "%s", timeout_names[cfg.timeout_idx]); break;
            case S_AUTOOFF: snprintf(v, sizeof v, "%s", autooff_names[cfg.autooff_idx]); break;
            case S_AUTOROT: snprintf(v, sizeof v, "%s", cfg.auto_rotate ? "On" : "Off"); break;
            case S_ORIENT: snprintf(v, sizeof v, "%s", orient_names[cfg.auto_rotate ? gfx_rotation() : cfg.orientation]); dim = cfg.auto_rotate; break;
            case S_FILTER: snprintf(v, sizeof v, "%s", filter_names[cfg.filter]); break;
            case S_MAXVOL: snprintf(v, sizeof v, "%d", cfg.max_volume); break;
            case S_VOLSTEP: snprintf(v, sizeof v, "%d per click", cfg.vol_step); break;
            case S_RESUME: snprintf(v, sizeof v, "%s", cfg.resume_play ? "Resume playing" : "Paused"); break;
            }
            int kind = rows[i].kind;
            int lw = gfx_text_w(FONT_REG, LABEL_PX, rows[i].label), vw = gfx_text_w(FONT_BOLD, VALUE_PX, v);
            int right_w = kind == 1 ? ctl_w : vw;
            int stacked = m + lw + 16 + right_w + m > gfx_w;    // label above, control below
            int rh = stacked ? llh + (kind == 1 ? bs : vlh) + 30 : (kind == 1 ? bs : llh) + 24;
            if (rh < 68) rh = 68;
            if (pass) {
                int lcy = stacked ? y + 12 + llh / 2 : y + rh / 2;                 // label line center
                int ccy = stacked ? y + 12 + llh + 6 + (kind == 1 ? bs : vlh) / 2 : y + rh / 2;  // control center
                text_vc(FONT_REG, LABEL_PX, m, lcy, stacked ? bw : gfx_w - 2 * m - right_w - 16, dim ? C_DIM : C_TEXT, rows[i].label);
                if (kind == 1) {
                    int xp = gfx_w - m - bs, xm = gfx_w - m - ctl_w;
                    gfx_rrect(xm, ccy - bs / 2, bs, bs, 10, C_PANEL); text_center(FONT_BOLD, 30, xm + bs / 2, ccy, 0, C_ACCENT, "\xe2\x88\x92");
                    gfx_rrect(xp, ccy - bs / 2, bs, bs, 10, C_PANEL); text_center(FONT_BOLD, 30, xp + bs / 2, ccy, 0, C_ACCENT, "+");
                    text_center(FONT_BOLD, 22, (xm + bs + xp) / 2, ccy, 0, C_TEXT, v);
                    add_button(xm - 12, y, bs + 24, rh, B_SET_DEC, rows[i].id);
                    add_button(xp - 12, y, bs + 24 + m, rh, B_SET_INC, rows[i].id);
                } else {
                    int maxw = stacked ? bw : right_w; if (vw > maxw) vw = maxw;
                    text_vc(FONT_BOLD, VALUE_PX, gfx_w - m - vw, ccy, maxw, dim ? C_DIM : C_ACCENT, v);
                    if (!dim) add_button(0, y, gfx_w, rh, B_SET_CYCLE, rows[i].id);
                }
                gfx_rect(m, y + rh - 1, gfx_w - m, 1, C_LINE);
            }
            y += rh;
        }
        y += 14;
        int usb = usb_host_connected();
        y += big_button(m, y, bw, usb ? C_ACCENT : C_PANEL, usb ? FONT_BOLD : FONT_REG, usb ? C_BG : C_TEXT,
                        usb ? "USB storage (copy music)" : "Switch to stock player", usb ? B_USB_STORAGE : B_STOCK, pass) + 14;
        y += big_button(m, y, bw, C_PANEL, FONT_REG, 0xff6a6a, "Power off  (or hold power)", B_POWEROFF, pass) + 14;
        if (pass) gfx_text(FONT_REG, 16, m, y + 6, 0, C_DIM, "ap80player " VERSION);
        y += LH(FONT_REG, 16) + 20;
        if (!pass) content_h[SCR_SETTINGS] = y - y0;
    }
    gfx_clip(0, 0, gfx_w, gfx_h);
}

// ---------- Bluetooth page ----------
// A row with a name, an optional second line, and an optional text button on the right.
static int bt_row(int y, int pass, const char *name, const char *sub, uint32_t name_c, int id, int arg,
                  const char *right, uint32_t right_c, int right_id) {
    int m = 18, rw = right ? gfx_text_w(FONT_BOLD, 19, right) + 28 : 0, maxw = gfx_w - 2 * m - rw;
    int nl = gfx_text_lines(FONT_BOLD, 22, maxw, 2, name), sl = sub ? gfx_text_lines(FONT_REG, 18, maxw, 2, sub) : 0;
    int nlh = nl * LH(FONT_BOLD, 22), slh = sl * LH(FONT_REG, 18);
    int rh = nlh + slh + 24; if (rh < 68) rh = 68;
    if (pass) {
        int ty = y + (rh - nlh - slh) / 2;
        gfx_text_wrap(FONT_BOLD, 22, m, ty + (LH(FONT_BOLD, 22) - T(22)) / 4, maxw, 2, name_c, name);
        if (sub) gfx_text_wrap(FONT_REG, 18, m, ty + nlh, maxw, 2, C_DIM, sub);
        add_button(0, y, gfx_w - rw - m, rh, id, arg);
        if (right) { text_vc(FONT_BOLD, 19, gfx_w - m - rw + 28, y + rh / 2, 0, right_c, right); add_button(gfx_w - rw - m, y, rw + m, rh, right_id, arg); }
        gfx_rect(m, y + rh - 1, gfx_w - m, 1, C_LINE);
    }
    return rh;
}
static int bt_section(int y, int pass, const char *label) {
    if (pass) gfx_text(FONT_BOLD, 17, 18, y + 16, gfx_w - 36, C_DIM, label);
    return LH(FONT_BOLD, 17) + 22;
}
static int bt_note(int y, int pass, uint32_t c, const char *text) {
    int w = gfx_w - 36, n = gfx_text_lines(FONT_REG, 18, w, 6, text);
    if (pass) gfx_text_wrap(FONT_REG, 18, 18, y + 12, w, 6, c, text);
    return n * LH(FONT_REG, 18) + 20;
}

static void draw_bt_page(void) {
    int top = content_y(), bot = content_bottom(), m = 18;
    int hh = LH(FONT_BOLD, 24) + 26; if (hh < 60) hh = 60;
    int ly = top + hh, view = bot - ly, y0 = ly, y = y0;
    long t = now_ms();
    for (int pass = 0; pass < 2; pass++) {
        if (pass) { finish_scroll(SCR_SETTINGS, ly, view); gfx_clip(0, ly, gfx_w - 8, view); }
        y = y0 = ly - scroll[SCR_SETTINGS];
        y += bt_row(y, pass, "Bluetooth", NULL, C_TEXT, B_BT_POWER, 0, cfg.bt_on ? "On" : "Off", cfg.bt_on ? C_ACCENT : C_DIM, B_BT_POWER);
        if (!cfg.bt_on) y += bt_note(y, pass, C_DIM, "Turn on to play to Bluetooth headphones, speakers or a car.");
        else if (!bts.powered) y += bt_note(y, pass, C_DIM, "Turning on\xe2\x80\xa6");
        else {
            y += bt_section(y, pass, "Paired devices");
            for (int i = 0; i < bts.npaired; i++) {
                bt_dev_t *d = &bts.paired[i];
                int sel = !strcasecmp(d->mac, cfg.bt_mac), armed = forget_armed == i && t < forget_until;
                const char *sub = sel ? (d->connected ? (headset ? "Connected \xc2\xb7 headphone jack in use" : "Connected \xc2\xb7 playing here")
                                                      : "Connects when you press play")
                                      : (d->connected ? "Connected \xc2\xb7 tap to use" : "Tap to use");
                y += bt_row(y, pass, d->name, sub, sel ? C_ACCENT : C_TEXT, B_BT_DEV, i,
                            armed ? "Forget?" : "Forget", armed ? 0xff6a6a : C_DIM, B_BT_FORGET);
            }
            if (!bts.npaired) y += bt_note(y, pass, C_DIM, "No paired devices yet.");
            y += 14;
            y += big_button(m, y, gfx_w - 2 * m, C_PANEL, FONT_REG, bts.scanning ? C_DIM : C_TEXT,
                            bts.scanning ? "Scanning\xe2\x80\xa6" : "Scan for new devices", bts.scanning ? B_NONE : B_BT_SCAN, pass) + 6;
            if (bts.nfound) {
                y += bt_section(y, pass, "Found \xc2\xb7 tap to pair");
                for (int i = 0; i < bts.nfound; i++) {
                    bt_dev_t *d = &bts.found[i];
                    y += bt_row(y, pass, d->name, !strcasecmp(bts.pairing, d->mac) ? "Pairing\xe2\x80\xa6" : d->mac, C_TEXT, B_BT_FOUND, i, NULL, 0, 0);
                }
            }
            if (bts.msg[0]) y += bt_note(y, pass, 0xff6a6a, bts.msg);
            y += bt_note(y, pass, C_DIM, "To add headphones, put them in pairing mode and scan. The headphone jack is used whenever something is plugged into it.");
        }
        y += 10;
        if (!pass) content_h[SCR_SETTINGS] = y - y0;
    }
    gfx_clip(0, 0, gfx_w, gfx_h);
    // header last, so it wins over rows scrolled under it
    gfx_rect(0, top, gfx_w, hh, C_BG);
    draw_chevron_left(14, top + hh / 2, 26, C_ACCENT, C_BG); add_button(0, top, 120, hh, B_BT_BACK, 0);
    text_vc(FONT_BOLD, 24, 52, top + hh / 2, gfx_w - 66, C_TEXT, "Bluetooth");
    gfx_rect(0, top + hh - 1, gfx_w, 1, C_LINE);
}

static void draw_usb_prompt(void) {
    int top = content_y(), m = 20, w = gfx_w - 2 * m, land = landscape();
    int y = top + (land ? 14 : 34);
    y += gfx_text_wrap(FONT_BOLD, 26, m, y, w, 2, C_TEXT, "Connected to computer") * LH(FONT_BOLD, 26) + 10;
    y += gfx_text_wrap(FONT_REG, 20, m, y, w, 5, C_DIM, "Use USB storage to copy music to the SD card. Playback stops while the card is connected.") * LH(FONT_REG, 20) + 16;
    int bh = LH(FONT_BOLD, 24) + 34; if (bh < (land ? 60 : 72)) bh = land ? 60 : 72;
    if (land) {
        int bw = (w - 16) / 2;
        if (y + bh > gfx_h - 8) y = gfx_h - 8 - bh;
        gfx_rrect(m, y, bw, bh, 14, C_ACCENT); text_center(FONT_BOLD, 23, m + bw / 2, y + bh / 2, bw - 16, C_BG, "USB storage");
        add_button(m, y, bw, bh, B_USB_STORAGE, 0);
        gfx_rrect(m + bw + 16, y, bw, bh, 14, C_PANEL); text_center(FONT_BOLD, 23, m + bw + 16 + bw / 2, y + bh / 2, bw - 16, C_TEXT, "Just charge");
        add_button(m + bw + 16, y, bw, bh, B_USB_CHARGE, 0);
    } else {
        if (y + 2 * bh + 16 > gfx_h - 12) y = gfx_h - 12 - 2 * bh - 16;
        gfx_rrect(m, y, w, bh, 14, C_ACCENT); text_center(FONT_BOLD, 24, gfx_w / 2, y + bh / 2, w - 16, C_BG, "USB storage");
        add_button(m, y, w, bh, B_USB_STORAGE, 0);
        y += bh + 16;
        gfx_rrect(m, y, w, bh, 14, C_PANEL); text_center(FONT_BOLD, 24, gfx_w / 2, y + bh / 2, w - 16, C_TEXT, "Just charge");
        add_button(m, y, w, bh, B_USB_CHARGE, 0);
    }
}
static void draw_usb_storage(void) {
    int top = content_y(), m = 20, w = gfx_w - 2 * m, land = landscape();
    int bh = LH(FONT_BOLD, 24) + 30; if (bh < (land ? 56 : 68)) bh = land ? 56 : 68;
    int by = gfx_h - bh - (land ? 12 : 24), y = top + (land ? 12 : 28);
    gfx_clip(0, 0, gfx_w, by - 8);
    y += gfx_text_wrap(FONT_BOLD, 26, m, y, w, 1, C_ACCENT, "USB storage") * LH(FONT_BOLD, 26) + 10;
    y += gfx_text_wrap(FONT_REG, 20, m, y, w, 6, C_TEXT,
        "The SD card is now a drive on your computer. Copy your music, then eject the drive on the computer or unplug the cable.") * LH(FONT_REG, 20) + 10;
    gfx_text_wrap(FONT_REG, 18, m, y, w, 4, C_DIM, "The drive can take a few seconds to appear. Tap Done only after ejecting.");
    gfx_clip(0, 0, gfx_w, gfx_h);
    gfx_rrect(m, by, w, bh, 14, C_PANEL); text_center(FONT_BOLD, 24, gfx_w / 2, by + bh / 2, w - 16, C_TEXT, "Done");
    add_button(m, by, w, bh, B_USB_DONE, 0);
}

static void render(void) {
    nbuttons = 0;
    gfx_clip(0, 0, gfx_w, gfx_h);
    gfx_clear(C_BG);
    if (usb_mode == 1) draw_usb_prompt();
    else if (usb_mode == 2) draw_usb_storage();
    else {
        if (screen == SCR_LIBRARY) draw_library();
        else if (screen == SCR_PLAYER) draw_player();
        else draw_settings();
        draw_tray();
    }
    draw_status_bar();
    if (toast_until_ms > now_ms()) {
        int maxw = gfx_w - 60, n = gfx_text_lines(FONT_BOLD, 30, maxw, 2, toast), lh = LH(FONT_BOLD, 30);
        int w = (n > 1 ? maxw : gfx_text_w(FONT_BOLD, 30, toast)) + 40;
        int h = n * lh + 30, y = (content_y() + content_bottom()) / 2 - h / 2;
        gfx_rrect((gfx_w - w) / 2, y, w, h, 16, 0x343a47);
        if (n == 1) text_center(FONT_BOLD, 30, gfx_w / 2, y + h / 2, maxw, C_TEXT, toast);
        else gfx_text_wrap(FONT_BOLD, 30, (gfx_w - w) / 2 + 20, y + 15, maxw, 2, C_TEXT, toast);
    }
    gfx_present();
    dirty = 0;
}

// ---------- actions ----------
static void update_accel_power(void);
static void set_screen_on(int on) {
    if (on == screen_on) return;
    screen_on = on;
    // Backlight only. jzfb's SET_SLEEP suspends the controller even with a frame in flight; that
    // buffer then stays "busy" forever and a later GET_EMPTY_BUF spins in the kernel (hung UI).
    if (on) { hw_set_brightness(cfg.brightness); dirty = 1; }
    else hw_set_brightness(0);
    update_accel_power();
}
static void change_volume(int d) {
    int *pv = out_bt ? &cfg.bt_volume : &cfg.volume;
    int v = *pv + d; if (v < 0) v = 0; if (v > cfg.max_volume) v = cfg.max_volume;
    *pv = v; if (out_bt) audio_set_soft_volume(v); else hw_set_volume(v);
    char s[32]; snprintf(s, sizeof s, v ? "Volume %d" : "Muted", v); if (screen_on) show_toast(s);
}
static void enter_dir(const char *dir) { snprintf(cfg.browse, sizeof cfg.browse, "%s", dir); lib_load(cfg.browse); scroll[SCR_LIBRARY] = 0; dirty = 1; }

static void power_off(void) {
    set_screen_on(1);
    gfx_clear(C_BG); gfx_text(FONT_BOLD, 24, 90, 220, 0, C_TEXT, "Powering off\xe2\x80\xa6"); gfx_present();
    audio_pause(); save_state(); audio_shutdown();
    sync(); sleep(1);
    hw_set_brightness(0);
    execl("/sbin/poweroff", "poweroff", (char *)NULL);
    system("poweroff");
    exit(0);
}

static void usb_enter(void) {
    char msg[128];
    save_state();
    audio_set_queue(NULL, 0, 0, 0, 0);                       // closes every file on the card
    for (int i = 0; i < 40 && !audio_files_closed(); i++) usleep(50000);
    lib_load("/nonexistent");                                // drop the directory listing
    if (usb_storage_enter(msg, sizeof msg)) {
        usb_mode = 0; show_toast(msg);
        lib_load(cfg.browse);
        if (cfg.track[0]) start_queue(cfg.queue_root, cfg.queue_recursive, cfg.track, cfg.pos, 0);
        return;
    }
    usb_mode = 2; dirty = 1;
}
static void usb_leave(void) {
    char msg[128];
    int rc = usb_storage_exit(msg, sizeof msg);
    usb_mode = 0; usb_prompt_done = 1; dirty = 1;
    last_input_ms = now_ms();   // restart the auto power off timer
    if (rc) { show_toast(msg); return; }
    if (!is_dir_path(cfg.browse)) snprintf(cfg.browse, sizeof cfg.browse, "%s", MUSIC_ROOT);
    lib_load(cfg.browse); scroll[SCR_LIBRARY] = 0;
    if (cfg.track[0] && access(cfg.track, R_OK) == 0) start_queue(cfg.queue_root, cfg.queue_recursive, cfg.track, cfg.pos, 0);
    show_toast("SD card ready");
}

// ---------- orientation ----------
static void apply_rotation(int r) {
    if (r == gfx_rotation()) return;
    gfx_set_rotation(r); dirty = 1;
    touch_active = 0; scrub_active = 0;   // a touch in progress belonged to the old layout
}
static void update_accel_power(void) { hw_accel_enable(cfg.auto_rotate && screen_on); }

// Android-style auto-rotate: switch when gravity clearly points along one screen axis
// (tilted more than ~35 degrees from flat, and that axis dominates) for about 0.75 s.
static void auto_rotate_tick(long t) {
    static int cand = -1; static long cand_since;
    int gx, gy, gz;
    if (!cfg.auto_rotate || !hw_accel_gravity(&gx, &gy, &gz)) return;
    int ax = abs(gx), ay = abs(gy), r = -1;
    long plane2 = (long)gx * gx + (long)gy * gy, all2 = plane2 + (long)gz * gz;
    if (all2 > 0 && plane2 * 3 > all2) {                    // not lying flat
        if (ay > ax * 3 / 2) r = gy > 0 ? ROT_PORTRAIT : ROT_PORTRAIT_FLIP;
        else if (ax > ay * 3 / 2) r = gx > 0 ? ROT_LANDSCAPE_FLIP : ROT_LANDSCAPE;
    }
    if (r < 0 || r == gfx_rotation()) { cand = -1; return; }
    if (r != cand) { cand = r; cand_since = t; return; }
    if (t - cand_since >= 700) { apply_rotation(r); cand = -1; }
}

// ---------- Bluetooth ----------
// Bluetooth is the output when it is on, a device is chosen and nothing is plugged into the jack.
static void update_output(void) {
    static char cur[18] = "?";
    int want = cfg.bt_on && cfg.bt_mac[0] && !headset;
    const char *mac = want ? cfg.bt_mac : "";
    if (!strcmp(cur, mac)) return;
    snprintf(cur, sizeof cur, "%s", mac);
    out_bt = want; audio_set_bluetooth(mac); dirty = 1;
}
static void bt_select(const char *mac, const char *name) {
    snprintf(cfg.bt_mac, sizeof cfg.bt_mac, "%s", mac ? mac : "");
    snprintf(cfg.bt_name, sizeof cfg.bt_name, "%s", name ? name : "");
    update_output(); save_state(); dirty = 1;
}
static void bt_tick(void) {
    static int seen = -1;
    int g = bt_gen(); if (g == seen) return;
    seen = g;
    bt_status_t prev = bts; bt_status(&bts); dirty = 1;
    if (pending_select[0] && !bts.pairing[0]) {   // pairing finished
        const char *name = bt_name(&bts, pending_select);
        if (name) bt_select(pending_select, name);
        pending_select[0] = 0;
    }
    // A paired device that connects by itself (headphones switched on, the car) becomes the output.
    for (int i = 0; i < bts.npaired; i++) {
        bt_dev_t *d = &bts.paired[i];
        if (!d->connected || !strcasecmp(d->mac, cfg.bt_mac)) continue;
        int was = 0;
        for (int j = 0; j < prev.npaired; j++) if (!strcasecmp(prev.paired[j].mac, d->mac)) was = prev.paired[j].connected;
        if (!was) { bt_select(d->mac, d->name); char s[96]; snprintf(s, sizeof s, "%s connected", d->name); if (screen_on) show_toast(s); }
    }
    const char *name = cfg.bt_mac[0] ? bt_name(&bts, cfg.bt_mac) : NULL;
    if (name && strcmp(name, cfg.bt_name)) { snprintf(cfg.bt_name, sizeof cfg.bt_name, "%s", name); save_state(); }
}
static void bt_button(button_t *b) {
    audio_status_t st;
    switch (b->id) {
    case B_BT_BACK: settings_page = 0; scroll[SCR_SETTINGS] = 0; break;
    case B_BT_POWER:
        cfg.bt_on = !cfg.bt_on; bt_set_power(cfg.bt_on);
        if (!cfg.bt_on && out_bt) audio_pause();
        update_output(); save_state(); break;
    case B_BT_DEV:
        if (b->arg < 0 || b->arg >= bts.npaired) break;
        if (!strcasecmp(bts.paired[b->arg].mac, cfg.bt_mac)) {   // tap the chosen device again: back to the jack
            if (out_bt) audio_pause();
            bt_disconnect(cfg.bt_mac); bt_select(NULL, NULL);
        } else {
            bt_select(bts.paired[b->arg].mac, bts.paired[b->arg].name);
            audio_status(&st);
            if (!st.playing && !bts.paired[b->arg].connected) bt_connect(cfg.bt_mac);
        }
        break;
    case B_BT_FORGET:
        if (b->arg < 0 || b->arg >= bts.npaired) break;
        if (forget_armed == b->arg && now_ms() < forget_until) {
            if (!strcasecmp(bts.paired[b->arg].mac, cfg.bt_mac)) { if (out_bt) audio_pause(); bt_select(NULL, NULL); }
            bt_forget(bts.paired[b->arg].mac); forget_armed = -1;
        } else { forget_armed = b->arg; forget_until = now_ms() + 3000; }
        break;
    case B_BT_SCAN: bt_scan(); break;
    case B_BT_FOUND:
        if (b->arg < 0 || b->arg >= bts.nfound || bts.pairing[0]) break;
        snprintf(pending_select, sizeof pending_select, "%s", bts.found[b->arg].mac);
        bt_pair(pending_select); break;
    }
    if (b->id != B_BT_FORGET) forget_armed = -1;
    dirty = 1;
}

static void settings_change(int id, int d) {
    switch (id) {
    case S_BRIGHT: cfg.brightness += d * 20; if (cfg.brightness < 10) cfg.brightness = 10; if (cfg.brightness > 255) cfg.brightness = 255; hw_set_brightness(cfg.brightness); break;
    case S_TIMEOUT: cfg.timeout_idx = (cfg.timeout_idx + 1) % 6; break;
    case S_AUTOOFF: cfg.autooff_idx = (cfg.autooff_idx + 1) % 6; break;
    case S_FILTER: cfg.filter = (cfg.filter + 1) & 7; hw_set_filter(cfg.filter); break;
    case S_MAXVOL: cfg.max_volume += d * 5; if (cfg.max_volume < 20) cfg.max_volume = 20; if (cfg.max_volume > 100) cfg.max_volume = 100;
        if (cfg.volume > cfg.max_volume) { cfg.volume = cfg.max_volume; hw_set_volume(cfg.volume); }
        if (cfg.bt_volume > cfg.max_volume) { cfg.bt_volume = cfg.max_volume; audio_set_soft_volume(cfg.bt_volume); } break;
    case S_RESUME: cfg.resume_play = !cfg.resume_play; break;
    case S_TEXTSIZE: cfg.text_size = (cfg.text_size + 1) & 3; gfx_set_text_scale(text_sizes[cfg.text_size]); break;
    case S_VOLSTEP: cfg.vol_step = cfg.vol_step % 3 + 1; break;
    case S_AUTOROT:
        cfg.auto_rotate = !cfg.auto_rotate;
        if (cfg.auto_rotate) cfg.orientation = gfx_rotation(); else apply_rotation(cfg.orientation);
        update_accel_power(); break;
    case S_ORIENT: cfg.orientation = (cfg.orientation + 1) & 3; apply_rotation(cfg.orientation); break;
    case S_BT: settings_page = 1; scroll[SCR_SETTINGS] = 0; forget_armed = -1; break;
    }
    save_state(); dirty = 1;
}

static void on_button(button_t *b, int tx) {
    audio_status_t st;
    switch (b->id) {
    case B_BACK_DIR: {
        char p[PATH_MAXLEN]; snprintf(p, sizeof p, "%s", cfg.browse); char *s = strrchr(p, '/');
        if (s && s > p && strcmp(cfg.browse, MUSIC_ROOT)) { *s = 0; if (strlen(p) < strlen(MUSIC_ROOT)) snprintf(p, sizeof p, "%s", MUSIC_ROOT); enter_dir(p); }
        break; }
    case B_TAB:
        if (b->arg == SCR_SETTINGS && screen == SCR_SETTINGS && settings_page) { settings_page = 0; scroll[SCR_SETTINGS] = 0; }
        screen = b->arg; dirty = 1; break;
    case B_PLAY_ALL:
        if (start_queue(cfg.browse, 1, NULL, 0, 1)) show_toast("No music here");
        else { screen = SCR_PLAYER; save_state(); }
        dirty = 1; break;
    case B_ENTRY: {
        if (b->arg < 0 || b->arg >= nents) break;
        char p[PATH_MAXLEN]; snprintf(p, sizeof p, "%s/%s", cfg.browse, ents[b->arg].name);
        if (ents[b->arg].is_dir) enter_dir(p);
        else { start_queue(cfg.browse, 0, p, 0, 1); screen = SCR_PLAYER; save_state(); dirty = 1; }
        break; }
    case B_PLAYPAUSE: audio_toggle(); save_state(); dirty = 1; break;
    case B_NEXT: audio_next(); dirty = 1; break;
    case B_PREV: audio_prev(); dirty = 1; break;
    case B_SEEK: audio_status(&st); if (st.dur > 0) { double f = (double)(tx - 20) / (gfx_w - 40); if (f < 0) f = 0; if (f > 1) f = 1; audio_seek(f * st.dur); } dirty = 1; break;
    case B_SET_DEC: settings_change(b->arg, -1); break;
    case B_SET_INC: settings_change(b->arg, +1); break;
    case B_SET_CYCLE: settings_change(b->arg, +1); break;
    case B_STOCK:
        audio_pause(); save_state(); audio_shutdown(); hw_set_brightness(cfg.brightness);
        exit(EXIT_TO_STOCK);
    case B_POWEROFF: power_off(); break;
    case B_USB_STORAGE: usb_enter(); break;
    case B_USB_CHARGE: usb_mode = 0; usb_prompt_done = 1; dirty = 1; break;
    case B_USB_DONE: usb_leave(); break;
    case B_BT_BACK: case B_BT_POWER: case B_BT_DEV: case B_BT_FORGET: case B_BT_SCAN: case B_BT_FOUND: bt_button(b); break;
    }
}

static void handle(hw_event_t *e) {
    long t = now_ms();
    int was_off = !screen_on;
    if (e->type != EV_TOUCH_MOVE) last_input_ms = t;
    if (usb_mode && e->type != EV_POWER_SHORT && e->type < EV_TOUCH_DOWN) return;  // no playback while USB dialogs are up
    switch (e->type) {
    case EV_KEY_PLAY: audio_toggle(); save_state(); dirty = 1; break;
    case EV_KEY_NEXT: audio_next(); dirty = 1; break;
    case EV_KEY_PREV: audio_prev(); dirty = 1; break;
    case EV_VOL_UP: change_volume(+cfg.vol_step); break;
    case EV_VOL_DOWN: change_volume(-cfg.vol_step); break;
    case EV_POWER_SHORT: set_screen_on(!screen_on); break;
    case EV_POWER_LONG: power_off(); break;
    case EV_TOUCH_DOWN:
        if (was_off) break;
        touch_active = 1; touch_dragging = 0; touch_x0 = e->x; touch_y0 = e->y; touch_scroll0 = scroll[screen];
        { button_t *b = hit(e->x, e->y); press_id = b ? b->id : -1; }
        if (press_id == B_SEEK) { scrub_active = 1; scrub_x = e->x; dirty = 1; }
        break;
    case EV_TOUCH_MOVE:
        if (!touch_active) break;
        if (scrub_active) { scrub_x = e->x; dirty = 1; last_input_ms = t; break; }
        if (!touch_dragging && abs(e->y - touch_y0) > 12) touch_dragging = 1;
        if (touch_dragging && !usb_mode && screen != SCR_PLAYER) { scroll[screen] = touch_scroll0 - (e->y - touch_y0); dirty = 1; last_input_ms = t; }
        break;
    case EV_TOUCH_UP:
        if (!touch_active) break;
        touch_active = 0; press_id = -1;
        if (scrub_active) {   // seek to where the finger was lifted
            scrub_active = 0;
            audio_status_t st; audio_status(&st);
            if (st.dur > 0) { double f = (double)(scrub_x - 20) / (gfx_w - 40); if (f < 0) f = 0; if (f > 1) f = 1; audio_seek(f * st.dur); }
            dirty = 1; break;
        }
        if (!touch_dragging) { button_t *b = hit(touch_x0, touch_y0); if (b) on_button(b, touch_x0); }
        dirty = 1; break;
    }
}

// ---------- debug command channel (for remote testing) ----------
static int cmd_fd = -1, cmd_wfd = -1;
static void cmd_open(void) {
    unlink(CMD_FIFO);
    if (mkfifo(CMD_FIFO, 0600)) return;
    cmd_fd = open(CMD_FIFO, O_RDONLY | O_NONBLOCK);
    cmd_wfd = open(CMD_FIFO, O_WRONLY | O_NONBLOCK);  // keep a writer so reads never hit EOF
}
static void cmd_run(char *line) {
    hw_event_t e = { 0 }; int x, y, x2, y2; char a[64], p[256];
    if (sscanf(line, "tap %d %d", &x, &y) == 2) {
        if (!screen_on) set_screen_on(1);
        if (dirty) render();
        e = (hw_event_t){ EV_TOUCH_DOWN, x, y }; handle(&e); e.type = EV_TOUCH_UP; handle(&e);
    } else if (sscanf(line, "drag %d %d %d %d", &x, &y, &x2, &y2) == 4) {
        e = (hw_event_t){ EV_TOUCH_DOWN, x, y }; handle(&e);
        for (int i = 1; i <= 8; i++) { e = (hw_event_t){ EV_TOUCH_MOVE, x + (x2 - x) * i / 8, y + (y2 - y) * i / 8 }; handle(&e); }
        e.type = EV_TOUCH_UP; handle(&e);
    } else if (sscanf(line, "key %63s", a) == 1) {
        int t = !strcmp(a, "play") ? EV_KEY_PLAY : !strcmp(a, "next") ? EV_KEY_NEXT : !strcmp(a, "prev") ? EV_KEY_PREV :
                !strcmp(a, "up") ? EV_VOL_UP : !strcmp(a, "down") ? EV_VOL_DOWN : !strcmp(a, "power") ? EV_POWER_SHORT : EV_NONE;
        e.type = t; if (t) handle(&e);
    } else if (sscanf(line, "rot %d", &x) == 1) {
        apply_rotation(x & 3);
    } else if (sscanf(line, "shot %255s", p) == 1) {
        render(); gfx_screenshot(p);
    } else if (!strncmp(line, "restart", 7)) {
        audio_pause(); save_state(); audio_shutdown(); exit(EXIT_RESTART);
    } else if (!strncmp(line, "quit", 4)) {
        audio_pause(); save_state(); audio_shutdown(); exit(0);
    }
}
static void cmd_read(void) {
    static char buf[1024]; static int len;
    int n = read(cmd_fd, buf + len, sizeof buf - 1 - len);
    if (n <= 0) return;
    len += n; buf[len] = 0;
    char *s = buf, *nl;
    while ((nl = strchr(s, '\n'))) { *nl = 0; cmd_run(s); s = nl + 1; }
    len = strlen(s); memmove(buf, s, len + 1);
}

static void on_signal(int sig) { (void)sig; save_state(); _exit(0); }

int main(int argc, char **argv) {
    const char *assets = argc > 1 ? argv[1] : "/mnt/sd_0/ap80dev/player";
    char fr[512], fb[512];
    snprintf(fr, sizeof fr, "%s/DejaVuSans.ttf", assets); snprintf(fb, sizeof fb, "%s/DejaVuSans-Bold.ttf", assets);
    signal(SIGPIPE, SIG_IGN); signal(SIGTERM, on_signal); signal(SIGINT, on_signal);
    if (gfx_init(fr, fb)) return 1;
    hw_init();
    load_state();
    hw_set_brightness(cfg.brightness); hw_set_volume(cfg.volume); hw_set_filter(cfg.filter); audio_set_soft_volume(cfg.bt_volume);
    gfx_set_rotation(cfg.orientation); gfx_set_text_scale(text_sizes[cfg.text_size]); update_accel_power();
    audio_init();
    headset = hw_headset(); battery = hw_battery(); charging = hw_charging();
    bt_init(cfg.bt_on); update_output();
    if (!is_dir_path(cfg.browse) || strncmp(cfg.browse, MUSIC_ROOT, strlen(MUSIC_ROOT))) snprintf(cfg.browse, sizeof cfg.browse, "%s", MUSIC_ROOT);
    lib_load(cfg.browse);
    // Resume: open the player if something was played before, otherwise the library.
    if (cfg.track[0] && !cfg.queue_root[0]) {  // older state files: fall back to the track's folder
        snprintf(cfg.queue_root, sizeof cfg.queue_root, "%s", cfg.track);
        char *sl = strrchr(cfg.queue_root, '/'); if (sl) *sl = 0;
    }
    if (cfg.track[0] && access(cfg.track, R_OK) == 0 && cfg.queue_root[0] &&
        !start_queue(cfg.queue_root, cfg.queue_recursive, cfg.track, cfg.pos, cfg.resume_play && headset))
        screen = SCR_PLAYER;
    cmd_open();
    last_input_ms = now_ms();

    int in_fds[8], nin = hw_input_fds(in_fds, 8);
    long last_tick = 0, last_save = now_ms(), last_play_ms = now_ms(); int last_index = -1, last_playing = -1;
    for (;;) {
        struct pollfd pf[10]; int n = 0;
        for (int i = 0; i < nin; i++) pf[n++] = (struct pollfd){ in_fds[i], POLLIN, 0 };
        if (cmd_fd >= 0) pf[n++] = (struct pollfd){ cmd_fd, POLLIN, 0 };
        int timeout = hw_power_pending_ms() ? 100 : screen_on ? 250 : 1000;
        poll(pf, n, timeout);
        for (int i = 0; i < n; i++) {
            if (!(pf[i].revents & POLLIN)) continue;
            if (pf[i].fd == cmd_fd) { cmd_read(); continue; }
            hw_event_t ev[32]; int k = hw_input_read(pf[i].fd, ev, 32);
            for (int j = 0; j < k; j++) {
                if (ev[j].type >= EV_TOUCH_DOWN) gfx_map_touch(ev[j].x, ev[j].y, &ev[j].x, &ev[j].y);
                handle(&ev[j]);
            }
        }
        bt_tick();
        if (hw_power_check_long()) { hw_event_t e = { EV_POWER_LONG, 0, 0 }; handle(&e); }
        long t = now_ms();
        static long last_acc;
        if (cfg.auto_rotate && screen_on && t - last_acc >= 250) { last_acc = t; hw_accel_sample(); if (!usb_mode && !touch_active) auto_rotate_tick(t); }
        if (t - last_tick >= 500) {
            last_tick = t;
            int hs = hw_headset();
            if (hs != headset) { headset = hs; dirty = 1; if (!hs) { audio_pause(); show_toast("Headphones unplugged"); } update_output(); }
            if (forget_armed >= 0 && t > forget_until) { forget_armed = -1; dirty = 1; }
            if (usb_mode == 2) { if (usb_storage_done()) usb_leave(); }
            else if (!usb_power()) { usb_prompt_done = 0; if (usb_mode == 1) { usb_mode = 0; dirty = 1; } }
            else if (!usb_mode && !usb_prompt_done && usb_host_connected()) { usb_mode = 1; set_screen_on(1); last_input_ms = t; dirty = 1; }
            static int bat_tick; if (++bat_tick >= 60) { bat_tick = 0; battery = hw_battery(); charging = hw_charging(); dirty = 1; }
            audio_status_t st; audio_status(&st);
            if (st.playing) { dirty = 1; last_play_ms = t; }
            int off = autooff_min[cfg.autooff_idx];
            long idle_ms = t - (last_input_ms > last_play_ms ? last_input_ms : last_play_ms);
            if (off && !usb_mode && !touch_active && idle_ms > off * 60000L) power_off();
            if (st.index != last_index || st.playing != last_playing) { last_index = st.index; last_playing = st.playing; dirty = 1; if (st.has_queue) save_state(); }
            if (st.playing && t - last_save > 15000) { last_save = t; save_state(); }
            int idle = audio_idle_seconds(); if (idle > 30) audio_amp_off();
            int to = timeouts[cfg.timeout_idx];
            if (screen_on && to && t - last_input_ms > to * 1000L && !touch_active) set_screen_on(0);
            if (toast_until_ms && t > toast_until_ms) { toast_until_ms = 0; dirty = 1; }
        }
        if (dirty && screen_on) render();
    }
}
