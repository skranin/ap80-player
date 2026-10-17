// ap80player: a simple music player UI for the Hidizs AP80 Pro-X.
// Layout: status line on top, the selected page in the middle, and a tray with
// Player / Library / Settings at the bottom. Works in portrait and landscape.
#include "gfx.h"
#include "hw.h"
#include "audio.h"
#include "usb.h"
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
    int volume, max_volume, brightness, timeout_idx, filter, resume_play, auto_rotate, orientation;
    char browse[PATH_MAXLEN];
    char queue_root[PATH_MAXLEN]; int queue_recursive;
    char track[PATH_MAXLEN]; double pos;
} cfg = { 25, 100, 120, 2, 0, 0, 0, ROT_PORTRAIT, MUSIC_ROOT, "", 0, "", 0 };

static const int timeouts[] = { 15, 30, 60, 120, 300, 0 };
static const char *timeout_names[] = { "15 s", "30 s", "1 min", "2 min", "5 min", "Never" };
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
    fprintf(f, "volume=%d\nmax_volume=%d\nbrightness=%d\ntimeout=%d\nfilter=%d\nresume_play=%d\nauto_rotate=%d\norientation=%d\nbrowse=%s\nqueue_root=%s\nqueue_recursive=%d\ntrack=%s\npos=%.1f\n",
            cfg.volume, cfg.max_volume, cfg.brightness, cfg.timeout_idx, cfg.filter, cfg.resume_play, cfg.auto_rotate, cfg.orientation, cfg.browse,
            cfg.queue_root, cfg.queue_recursive, cfg.track, cfg.pos);
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
        else if (!strcmp(line, "browse")) snprintf(cfg.browse, sizeof cfg.browse, "%s", v);
        else if (!strcmp(line, "queue_root")) snprintf(cfg.queue_root, sizeof cfg.queue_root, "%s", v);
        else if (!strcmp(line, "queue_recursive")) cfg.queue_recursive = atoi(v);
        else if (!strcmp(line, "track")) snprintf(cfg.track, sizeof cfg.track, "%s", v);
        else if (!strcmp(line, "pos")) cfg.pos = atof(v);
    }
    fclose(f);
    if (cfg.max_volume < 10 || cfg.max_volume > 100) cfg.max_volume = 100;
    if (cfg.volume < 0) cfg.volume = 0; if (cfg.volume > cfg.max_volume) cfg.volume = cfg.max_volume;
    if (cfg.brightness < 5 || cfg.brightness > 255) cfg.brightness = 120;
    if (cfg.timeout_idx < 0 || cfg.timeout_idx > 5) cfg.timeout_idx = 2;
    cfg.filter &= 7; cfg.orientation &= 3; cfg.auto_rotate = !!cfg.auto_rotate;
}

// ---------- library ----------
typedef struct { char *name; int is_dir; } entry_t;
static entry_t *ents; static int nents, has_audio_here;

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
enum { SCR_PLAYER, SCR_LIBRARY, SCR_SETTINGS, SCR_COUNT };
static int screen = SCR_LIBRARY;
static int screen_on = 1, dirty = 1;
static long last_input_ms, toast_until_ms; static char toast[128];
static int battery = -1, charging, headset;
static int scroll[SCR_COUNT], content_h[SCR_COUNT];   // per-page vertical scroll (px)
static int touch_active, touch_dragging, touch_x0, touch_y0, touch_scroll0, press_id = -1;
static int scrub_active, scrub_x;                  // dragging the seek bar

typedef struct { int x, y, w, h, id, arg; } button_t;
static button_t buttons[128]; static int nbuttons;
enum { B_NONE, B_BACK_DIR, B_PLAY_ALL, B_ENTRY, B_TAB, B_PLAYPAUSE, B_NEXT, B_PREV,
       B_SEEK, B_SET_DEC, B_SET_INC, B_SET_CYCLE, B_STOCK, B_POWEROFF, B_USB_STORAGE, B_USB_CHARGE, B_USB_DONE };
enum { S_BRIGHT, S_TIMEOUT, S_AUTOROT, S_ORIENT, S_MAXVOL, S_FILTER, S_RESUME };

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
#define STATUS_H 34
static int landscape(void) { return gfx_w > gfx_h; }
static int tray_h(void) { return landscape() ? 72 : 92; }
static int content_y(void) { return STATUS_H; }
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
static void text_center(int font, int px, int cx, int y, uint32_t c, const char *s) { gfx_text(font, px, cx - gfx_text_w(font, px, s) / 2, y, 0, c, s); }

static void draw_status_bar(void) {
    gfx_rect(0, 0, gfx_w, STATUS_H, C_PANEL);
    char s[64];
    audio_status_t st; audio_status(&st);
    int x = 12;
    if (st.has_queue) { if (st.playing) draw_play_icon(x + 8, 17, 16, C_ACCENT); else draw_pause_icon(x + 8, 17, 14, C_DIM); x += 26; }
    snprintf(s, sizeof s, "Vol %d", cfg.volume);
    x += gfx_text(FONT_BOLD, 19, x, 6, 0, C_TEXT, s) + 12;
    if (!headset) gfx_text(FONT_REG, 17, x, 7, gfx_w / 2, C_ACCENT, "no headphones");
    if (battery >= 0) {
        snprintf(s, sizeof s, "%s%d%%", charging ? "+" : "", battery);
        int w = gfx_text_w(FONT_BOLD, 19, s), bx = gfx_w - 40;
        gfx_text(FONT_BOLD, 19, bx - w - 8, 6, 0, C_TEXT, s);
        gfx_rect(bx, 9, 28, 16, C_DIM); gfx_rect(bx + 2, 11, 24, 12, C_PANEL); gfx_rect(bx + 28, 13, 3, 8, C_DIM);
        gfx_rect(bx + 3, 12, 22 * battery / 100, 10, battery <= 15 ? 0xe04848 : C_TEXT);
    }
}

// Bottom tray: Player / Library / Settings.
static void draw_tray(void) {
    int th = tray_h(), y = gfx_h - th, w = gfx_w / 3;
    gfx_rect(0, y, gfx_w, th, C_PANEL);
    gfx_rect(0, y, gfx_w, 1, C_LINE);
    static const char *labels[] = { "Player", "Library", "Settings" };
    for (int i = 0; i < 3; i++) {
        int x = i * w, active = screen == i, cx = x + w / 2;
        uint32_t c = active ? C_ACCENT : C_DIM;
        if (active) { gfx_rect(x + 12, y, w - 24, 4, C_ACCENT); }
        int lw = gfx_text_w(FONT_BOLD, 19, labels[i]), isz = 30;
        int icx, icy, lx, ly;
        if (landscape()) { int tot = isz + 10 + lw; icx = cx - tot / 2 + isz / 2; icy = y + th / 2; lx = icx + isz / 2 + 10; ly = y + th / 2 - 12; }
        else { icx = cx; icy = y + 32; lx = cx - lw / 2; ly = y + 56; }
        if (i == SCR_PLAYER) { gfx_circle(icx, icy, isz / 2, c); draw_play_icon(icx + 2, icy, isz / 2, C_PANEL); }
        else if (i == SCR_LIBRARY) draw_folder_icon(icx - isz / 2, icy - isz * 3 / 8, isz, c);
        else gfx_text(FONT_REG, 36, icx - gfx_text_w(FONT_REG, 36, "\xe2\x9a\x99") / 2, icy - 23, 0, c, "\xe2\x9a\x99");  // U+2699 gear
        gfx_text(FONT_BOLD, 19, lx, ly, 0, active ? C_ACCENT : C_TEXT, labels[i]);
        add_button(x, y, w, th, B_TAB, i);
    }
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
#define HEADER_H 60
#define ROW_H 70

static void draw_library(void) {
    audio_status_t st; audio_status(&st);
    int at_root = !strcmp(cfg.browse, MUSIC_ROOT), top = content_y(), bot = content_bottom();
    // header: back + folder name
    int tx = 18;
    if (!at_root) { draw_chevron_left(14, top + HEADER_H / 2, 26, C_ACCENT, C_BG); add_button(0, top, 80, HEADER_H, B_BACK_DIR, 0); tx = 52; }
    gfx_text(FONT_BOLD, 24, tx, top + 15, gfx_w - tx - 14, C_TEXT, at_root ? "Library" : base_name(cfg.browse));
    gfx_rect(0, top + HEADER_H - 1, gfx_w, 1, C_LINE);
    int ly = top + HEADER_H, view = bot - ly;
    int rows = nents + (nents ? 1 : 0);
    content_h[SCR_LIBRARY] = rows * ROW_H;
    finish_scroll(SCR_LIBRARY, ly, view);
    gfx_clip(0, ly, gfx_w - 8, view);
    if (!nents) gfx_text(FONT_REG, 21, 20, ly + 30, gfx_w - 40, C_DIM, "No music in this folder");
    for (int r = 0; r < rows; r++) {
        int y = ly + r * ROW_H - scroll[SCR_LIBRARY];
        if (y + ROW_H < ly || y > bot) continue;
        int id = r == 0 ? B_PLAY_ALL : B_ENTRY, arg = r - 1, mid = y + ROW_H / 2;
        if (r == 0) {
            gfx_circle(36, mid, 20, C_ACCENT); draw_play_icon(38, mid, 16, C_BG);
            gfx_text(FONT_BOLD, 22, 70, mid - 14, gfx_w - 90, C_ACCENT, has_audio_here || at_root ? "Play all" : "Play all (with subfolders)");
        } else {
            entry_t *e = &ents[arg];
            char name[512]; snprintf(name, sizeof name, "%s", e->name);
            int playing_this = 0;
            if (e->is_dir) draw_folder_icon(20, mid - 12, 32, C_DIM);
            else {
                char full[PATH_MAXLEN]; snprintf(full, sizeof full, "%s/%s", cfg.browse, e->name);
                playing_this = st.has_queue && !strcmp(full, st.path);
                strip_ext(name);
                gfx_text(FONT_REG, 28, 24, mid - 18, 0, playing_this ? C_ACCENT : C_DIM, "\xe2\x99\xaa");  // U+266A note
            }
            gfx_text(FONT_REG, 22, 70, mid - 14, gfx_w - 70 - (e->is_dir ? 40 : 16), playing_this ? C_ACCENT : C_TEXT, name);
            if (e->is_dir) gfx_text(FONT_BOLD, 28, gfx_w - 32, mid - 18, 0, C_DIM, "\xe2\x80\xba");  // U+203A ›
        }
        gfx_rect(70, y + ROW_H - 1, gfx_w - 70, 1, C_LINE);
        add_button(0, y < ly ? ly : y, gfx_w, ROW_H - (y < ly ? ly - y : 0), id, arg);
    }
    gfx_clip(0, 0, gfx_w, gfx_h);
}

static void draw_player(void) {
    audio_status_t st; audio_status(&st);
    int top = content_y(), bot = content_bottom(), land = landscape(), m = 20, w = gfx_w - 2 * m;
    if (!st.has_queue) {
        gfx_text_wrap(FONT_BOLD, 26, m, top + 40, w, 2, C_TEXT, "Nothing is playing");
        gfx_text_wrap(FONT_REG, 21, m, top + 84, w, 3, C_DIM, "Pick a song or a folder in the Library.");
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

    int py = cy - r - (land ? 44 : 56);   // seek bar
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
    int iy = py - (land ? 30 : 38);
    gfx_text(FONT_REG, 18, m, iy, w, C_DIM, info);
    if (st.error[0]) { gfx_text(FONT_REG, 18, m, iy - 26, w, 0xe04848, st.error); iy -= 26; }

    // top: title, artist - album, folder (as much as fits)
    char title[300], line2[600], line3[600];
    if (st.title[0]) snprintf(title, sizeof title, "%s", st.title);
    else { snprintf(title, sizeof title, "%s", base_name(st.path)); strip_ext(title); }
    if (st.artist[0] && st.album[0]) snprintf(line2, sizeof line2, "%s \xe2\x80\x94 %s", st.artist, st.album);
    else snprintf(line2, sizeof line2, "%s", st.artist[0] ? st.artist : st.album);
    char dir[PATH_MAXLEN]; snprintf(dir, sizeof dir, "%s", st.path); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    snprintf(line3, sizeof line3, "%s", base_name(dir));
    int tpx = land ? 26 : 30, spx = land ? 20 : 22, y = top + (land ? 12 : 22), lim = iy - 8;
    int tlh = gfx_line_h(FONT_BOLD, tpx), slh = gfx_line_h(FONT_REG, spx);
    int extra = (line2[0] ? slh : 0) + slh;                // keep room for at least line2 + folder
    int tl = (lim - y - extra) / tlh; if (tl > 3) tl = 3; if (tl < 1) tl = 1;
    y += gfx_text_wrap(FONT_BOLD, tpx, m, y, w, tl, C_TEXT, title) * tlh + 6;
    if (line2[0] && y + slh <= lim) {
        int l2 = (lim - y - slh) / slh; if (l2 > 2) l2 = 2; if (l2 < 1) l2 = 1;
        y += gfx_text_wrap(FONT_REG, spx, m, y, w, l2, C_DIM, line2) * slh + 2;
    }
    if ((!line2[0] || strcmp(line3, st.album)) && y + slh <= lim) gfx_text(FONT_REG, spx - 2, m, y, w, C_DIM, line3);
}

#define SET_ROW_H 68
static void draw_settings(void) {
    int top = content_y(), bot = content_bottom(), view = bot - top;
    char v[64];
    struct { const char *label; int kind, id; } rows[] = {   // kind 1 = -/+, 0 = tap to change
        { "Brightness", 1, S_BRIGHT }, { "Screen off after", 0, S_TIMEOUT }, { "Auto-rotate", 0, S_AUTOROT },
        { "Orientation", 0, S_ORIENT }, { "Max volume", 1, S_MAXVOL }, { "DAC filter", 0, S_FILTER }, { "On start", 0, S_RESUME },
    };
    int nrows = sizeof rows / sizeof rows[0];
    content_h[SCR_SETTINGS] = nrows * SET_ROW_H + 2 * 70 + 60;
    finish_scroll(SCR_SETTINGS, top, view);
    gfx_clip(0, top, gfx_w - 8, view);
    int y = top - scroll[SCR_SETTINGS], m = 18, vw = gfx_w * 11 / 20;  // value column width
    for (int i = 0; i < nrows; i++, y += SET_ROW_H) {
        int dim = 0;
        switch (rows[i].id) {
        case S_BRIGHT: snprintf(v, sizeof v, "%d%%", cfg.brightness * 100 / 255); break;
        case S_TIMEOUT: snprintf(v, sizeof v, "%s", timeout_names[cfg.timeout_idx]); break;
        case S_AUTOROT: snprintf(v, sizeof v, "%s", cfg.auto_rotate ? "On" : "Off"); break;
        case S_ORIENT: snprintf(v, sizeof v, "%s", orient_names[cfg.auto_rotate ? gfx_rotation() : cfg.orientation]); dim = cfg.auto_rotate; break;
        case S_FILTER: snprintf(v, sizeof v, "%s", filter_names[cfg.filter]); break;
        case S_MAXVOL: snprintf(v, sizeof v, "%d", cfg.max_volume); break;
        case S_RESUME: snprintf(v, sizeof v, "%s", cfg.resume_play ? "Resume playing" : "Paused"); break;
        }
        int mid = y + SET_ROW_H / 2;
        int lw = rows[i].kind == 1 ? gfx_w - 2 * 48 - 100 - m : gfx_w - 2 * m - 16 - (gfx_text_w(FONT_BOLD, 20, v) < vw ? gfx_text_w(FONT_BOLD, 20, v) : vw);
        gfx_text(FONT_REG, 22, m, mid - 14, lw, dim ? C_DIM : C_TEXT, rows[i].label);
        if (rows[i].kind == 1) {
            int bs = 48;
            gfx_rrect(gfx_w - 2 * bs - 70, mid - bs / 2, bs, bs, 10, C_PANEL); text_center(FONT_BOLD, 30, gfx_w - 2 * bs - 70 + bs / 2, mid - 19, C_ACCENT, "\xe2\x88\x92");
            gfx_rrect(gfx_w - bs - 14, mid - bs / 2, bs, bs, 10, C_PANEL); text_center(FONT_BOLD, 30, gfx_w - bs / 2 - 14, mid - 19, C_ACCENT, "+");
            text_center(FONT_BOLD, 22, gfx_w - bs - 42, mid - 14, C_TEXT, v);
            add_button(gfx_w - 2 * bs - 82, y, bs + 24, SET_ROW_H, B_SET_DEC, rows[i].id);
            add_button(gfx_w - bs - 26, y, bs + 26, SET_ROW_H, B_SET_INC, rows[i].id);
        } else {
            int w = gfx_text_w(FONT_BOLD, 20, v); if (w > vw) w = vw;
            gfx_text(FONT_BOLD, 20, gfx_w - m - w, mid - 13, vw, dim ? C_DIM : C_ACCENT, v);
            if (!dim) add_button(0, y, gfx_w, SET_ROW_H, B_SET_CYCLE, rows[i].id);
        }
        gfx_rect(m, y + SET_ROW_H - 1, gfx_w - m, 1, C_LINE);
    }
    y += 14;
    int bw = gfx_w - 2 * m;
    if (usb_host_connected()) {  // replaces the stock-player button while a computer is connected
        gfx_rrect(m, y, bw, 56, 12, C_ACCENT); gfx_text(FONT_BOLD, 22, m + 18, y + 14, bw - 36, C_BG, "USB storage (copy music)");
        add_button(m, y, bw, 56, B_USB_STORAGE, 0);
    } else {
        gfx_rrect(m, y, bw, 56, 12, C_PANEL); gfx_text(FONT_REG, 22, m + 18, y + 14, bw - 36, C_TEXT, "Switch to stock player");
        add_button(m, y, bw, 56, B_STOCK, 0);
    }
    y += 70;
    gfx_rrect(m, y, bw, 56, 12, C_PANEL); gfx_text(FONT_REG, 22, m + 18, y + 14, bw - 36, 0xff6a6a, "Power off  (or hold power)");
    add_button(m, y, bw, 56, B_POWEROFF, 0);
    y += 70;
    gfx_text(FONT_REG, 16, m, y + 6, 0, C_DIM, "ap80player " VERSION);
    gfx_clip(0, 0, gfx_w, gfx_h);
}

static void draw_usb_prompt(void) {
    int top = content_y(), m = 20, w = gfx_w - 2 * m, land = landscape();
    int y = top + (land ? 16 : 40);
    gfx_text(FONT_BOLD, 26, m, y, w, C_TEXT, "Connected to computer");
    y += 44;
    y += gfx_text_wrap(FONT_REG, 20, m, y, w, 3, C_DIM, "Use USB storage to copy music to the SD card. Playback stops while the card is connected.") * 25 + 16;
    int bh = land ? 60 : 72;
    if (land) {
        int bw = (w - 16) / 2;
        gfx_rrect(m, y, bw, bh, 14, C_ACCENT); text_center(FONT_BOLD, 23, m + bw / 2, y + bh / 2 - 15, C_BG, "USB storage");
        add_button(m, y, bw, bh, B_USB_STORAGE, 0);
        gfx_rrect(m + bw + 16, y, bw, bh, 14, C_PANEL); text_center(FONT_BOLD, 23, m + bw + 16 + bw / 2, y + bh / 2 - 15, C_TEXT, "Just charge");
        add_button(m + bw + 16, y, bw, bh, B_USB_CHARGE, 0);
    } else {
        gfx_rrect(m, y, w, bh, 14, C_ACCENT); text_center(FONT_BOLD, 24, gfx_w / 2, y + bh / 2 - 15, C_BG, "USB storage");
        add_button(m, y, w, bh, B_USB_STORAGE, 0);
        y += bh + 16;
        gfx_rrect(m, y, w, bh, 14, C_PANEL); text_center(FONT_BOLD, 24, gfx_w / 2, y + bh / 2 - 15, C_TEXT, "Just charge");
        add_button(m, y, w, bh, B_USB_CHARGE, 0);
    }
}
static void draw_usb_storage(void) {
    int top = content_y(), m = 20, w = gfx_w - 2 * m, land = landscape();
    int y = top + (land ? 12 : 32);
    gfx_text(FONT_BOLD, 26, m, y, w, C_ACCENT, "USB storage");
    y += 42;
    y += gfx_text_wrap(FONT_REG, 20, m, y, w, 5, C_TEXT,
        "The SD card is now a drive on your computer. Copy your music, then eject the drive on the computer or unplug the cable.") * 25 + 10;
    y += gfx_text_wrap(FONT_REG, 18, m, y, w, 3, C_DIM, "The drive can take a few seconds to appear. Tap Done only after ejecting.") * 22;
    int bh = land ? 56 : 68, by = gfx_h - bh - (land ? 12 : 24);
    gfx_rrect(m, by, w, bh, 14, C_PANEL); text_center(FONT_BOLD, 24, gfx_w / 2, by + bh / 2 - 15, C_TEXT, "Done");
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
        int w = gfx_text_w(FONT_BOLD, 30, toast) + 56; if (w > gfx_w - 20) w = gfx_w - 20;
        int h = 76, y = (content_y() + content_bottom()) / 2 - h / 2;
        gfx_rrect((gfx_w - w) / 2, y, w, h, 16, 0x343a47);
        text_center(FONT_BOLD, 30, gfx_w / 2, y + 20, C_TEXT, toast);
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
    int v = cfg.volume + d; if (v < 0) v = 0; if (v > cfg.max_volume) v = cfg.max_volume;
    cfg.volume = v; hw_set_volume(v);
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

static void settings_change(int id, int d) {
    switch (id) {
    case S_BRIGHT: cfg.brightness += d * 20; if (cfg.brightness < 10) cfg.brightness = 10; if (cfg.brightness > 255) cfg.brightness = 255; hw_set_brightness(cfg.brightness); break;
    case S_TIMEOUT: cfg.timeout_idx = (cfg.timeout_idx + 1) % 6; break;
    case S_FILTER: cfg.filter = (cfg.filter + 1) & 7; hw_set_filter(cfg.filter); break;
    case S_MAXVOL: cfg.max_volume += d * 5; if (cfg.max_volume < 20) cfg.max_volume = 20; if (cfg.max_volume > 100) cfg.max_volume = 100;
        if (cfg.volume > cfg.max_volume) { cfg.volume = cfg.max_volume; hw_set_volume(cfg.volume); } break;
    case S_RESUME: cfg.resume_play = !cfg.resume_play; break;
    case S_AUTOROT:
        cfg.auto_rotate = !cfg.auto_rotate;
        if (cfg.auto_rotate) cfg.orientation = gfx_rotation(); else apply_rotation(cfg.orientation);
        update_accel_power(); break;
    case S_ORIENT: cfg.orientation = (cfg.orientation + 1) & 3; apply_rotation(cfg.orientation); break;
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
    case B_TAB: screen = b->arg; dirty = 1; break;
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
    case EV_VOL_UP: change_volume(+1); break;
    case EV_VOL_DOWN: change_volume(-1); break;
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
    hw_set_brightness(cfg.brightness); hw_set_volume(cfg.volume); hw_set_filter(cfg.filter);
    gfx_set_rotation(cfg.orientation); update_accel_power();
    audio_init();
    headset = hw_headset(); battery = hw_battery(); charging = hw_charging();
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
    long last_tick = 0, last_save = now_ms(); int last_index = -1, last_playing = -1;
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
        if (hw_power_check_long()) { hw_event_t e = { EV_POWER_LONG, 0, 0 }; handle(&e); }
        long t = now_ms();
        static long last_acc;
        if (cfg.auto_rotate && screen_on && t - last_acc >= 250) { last_acc = t; hw_accel_sample(); if (!usb_mode && !touch_active) auto_rotate_tick(t); }
        if (t - last_tick >= 500) {
            last_tick = t;
            int hs = hw_headset();
            if (hs != headset) { headset = hs; dirty = 1; if (!hs) { audio_pause(); show_toast("Headphones unplugged"); } }
            if (usb_mode == 2) { if (usb_storage_done()) usb_leave(); }
            else if (!usb_power()) { usb_prompt_done = 0; if (usb_mode == 1) { usb_mode = 0; dirty = 1; } }
            else if (!usb_mode && !usb_prompt_done && usb_host_connected()) { usb_mode = 1; set_screen_on(1); last_input_ms = t; dirty = 1; }
            static int bat_tick; if (++bat_tick >= 60) { bat_tick = 0; battery = hw_battery(); charging = hw_charging(); dirty = 1; }
            audio_status_t st; audio_status(&st);
            if (st.playing) dirty = 1;
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
