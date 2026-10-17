// ap80player: a simple music player UI for the Hidizs AP80 Pro-X.
// Screens: Library (folder browser), Player (now playing), Settings.
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
#define C_TEXT   0xeeeeee
#define C_DIM    0x8a8f9c
#define C_ACCENT 0xff8a3d
#define C_PRESS  0x2b303b

// ---------- settings / state ----------
static struct {
    int volume, max_volume, brightness, timeout_idx, filter, resume_play;
    char browse[PATH_MAXLEN];
    char queue_root[PATH_MAXLEN]; int queue_recursive;
    char track[PATH_MAXLEN]; double pos;
} cfg = { 25, 100, 120, 2, 0, 0, MUSIC_ROOT, "", 0, "", 0 };

static const int timeouts[] = { 15, 30, 60, 120, 300, 0 };
static const char *timeout_names[] = { "15 s", "30 s", "1 min", "2 min", "5 min", "Never" };
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
    fprintf(f, "volume=%d\nmax_volume=%d\nbrightness=%d\ntimeout=%d\nfilter=%d\nresume_play=%d\nbrowse=%s\nqueue_root=%s\nqueue_recursive=%d\ntrack=%s\npos=%.1f\n",
            cfg.volume, cfg.max_volume, cfg.brightness, cfg.timeout_idx, cfg.filter, cfg.resume_play, cfg.browse,
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
    cfg.filter &= 7;
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
enum { SCR_LIBRARY, SCR_PLAYER, SCR_SETTINGS };
static int screen = SCR_LIBRARY, prev_screen = SCR_LIBRARY;
static int screen_on = 1, dirty = 1;
static long last_input_ms, toast_until_ms; static char toast[128];
static int battery = -1, charging, headset;
static int list_scroll, list_content_h;           // library list scroll (px)
static int touch_active, touch_dragging, touch_x0, touch_y0, touch_scroll0, press_id = -1;

typedef struct { int x, y, w, h, id, arg; } button_t;
static button_t buttons[128]; static int nbuttons;
enum { B_NONE, B_BACK_DIR, B_TO_PLAYER, B_PLAY_ALL, B_ENTRY, B_TO_LIBRARY, B_TO_SETTINGS, B_PLAYPAUSE, B_NEXT, B_PREV,
       B_SEEK, B_SET_BACK, B_SET_DEC, B_SET_INC, B_SET_CYCLE, B_STOCK, B_POWEROFF, B_USB_STORAGE, B_USB_CHARGE, B_USB_DONE };
enum { S_BRIGHT, S_TIMEOUT, S_FILTER, S_MAXVOL, S_RESUME };

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

// ---------- drawing helpers ----------
static void draw_chevron_left(int x, int y, int s, uint32_t c) { for (int i = 0; i < 3; i++) { gfx_tri(x + s / 2 + i, y - s / 2, x + i, y, x + s / 2 + i, y + s / 2, c); } gfx_tri(x + s / 2 + 3, y - s / 2 + 3, x + 3 + 3, y, x + s / 2 + 3, y + s / 2 - 3, C_BG); }
static void draw_play_icon(int cx, int cy, int s, uint32_t c) { gfx_tri(cx - s / 3, cy - s / 2, cx - s / 3, cy + s / 2, cx + s * 2 / 3 - s / 6, cy, c); }
static void draw_pause_icon(int cx, int cy, int s, uint32_t c) { gfx_rect(cx - s / 2, cy - s / 2, s / 3, s, c); gfx_rect(cx + s / 6, cy - s / 2, s / 3, s, c); }
static void draw_skip_icon(int cx, int cy, int s, int dir, uint32_t c) {
    if (dir > 0) { gfx_tri(cx - s / 2, cy - s / 2, cx - s / 2, cy + s / 2, cx + s / 3, cy, c); gfx_rect(cx + s / 3, cy - s / 2, s / 6 + 1, s, c); }
    else { gfx_tri(cx + s / 2, cy - s / 2, cx + s / 2, cy + s / 2, cx - s / 3, cy, c); gfx_rect(cx - s / 3 - s / 6, cy - s / 2, s / 6 + 1, s, c); }
}
static void draw_folder_icon(int x, int y, uint32_t c) { gfx_rrect(x, y + 3, 10, 6, 2, c); gfx_rrect(x, y + 6, 24, 16, 3, c); }

static void draw_status_bar(void) {
    gfx_rect(0, 0, SCR_W, 28, C_PANEL);
    char s[64];
    audio_status_t st; audio_status(&st);
    if (st.has_queue) { if (st.playing) draw_play_icon(16, 14, 12, C_ACCENT); else draw_pause_icon(16, 14, 11, C_DIM); }
    snprintf(s, sizeof s, "Vol %d%s", cfg.volume, headset ? "" : "  · no headphones");
    gfx_text(FONT_REG, 15, 32, 5, 220, headset ? C_DIM : C_ACCENT, s);
    if (battery >= 0) {
        snprintf(s, sizeof s, "%s%d%%", charging ? "+" : "", battery);
        int w = gfx_text_w(FONT_REG, 15, s), bx = SCR_W - 34;
        gfx_text(FONT_REG, 15, bx - w - 6, 5, 0, C_DIM, s);
        gfx_rect(bx, 8, 24, 12, C_DIM); gfx_rect(bx + 1, 9, 22, 10, C_PANEL); gfx_rect(bx + 24, 11, 2, 6, C_DIM);
        gfx_rect(bx + 2, 10, 20 * battery / 100, 8, battery <= 15 ? 0xe04848 : C_TEXT);
    }
}

static void draw_header(const char *title, int back_id, const char *back_label, int right_id) {
    gfx_rect(0, 28, SCR_W, 56, C_BG);
    int tx = 16;
    if (back_id) {
        int w = 24 + (back_label ? gfx_text_w(FONT_REG, 18, back_label) + 6 : 0);
        draw_chevron_left(14, 56, 18, C_ACCENT);
        if (back_label) gfx_text(FONT_REG, 18, 34, 45, 0, C_ACCENT, back_label);
        add_button(0, 28, w + 24, 56, back_id, 0);
        tx = w + 30;
    }
    int rw = right_id ? 56 : 0;
    gfx_text(FONT_BOLD, 20, tx, 43, SCR_W - tx - rw - 8, C_TEXT, title);
    if (right_id == B_TO_PLAYER) {
        gfx_rrect(SCR_W - 52, 38, 40, 36, 8, C_PANEL); draw_play_icon(SCR_W - 30, 56, 16, C_ACCENT);
        add_button(SCR_W - 64, 28, 64, 56, B_TO_PLAYER, 0);
    } else if (right_id == B_TO_SETTINGS) {
        gfx_text(FONT_REG, 30, SCR_W - 46, 39, 0, C_DIM, "\xe2\x9a\x99");  // U+2699 gear
        add_button(SCR_W - 64, 28, 64, 56, B_TO_SETTINGS, 0);
    }
    gfx_rect(0, 83, SCR_W, 1, C_LINE);
}

// ---------- screens ----------
#define LIST_Y 84
#define ROW_H 60

static void draw_library(void) {
    audio_status_t st; audio_status(&st);
    int at_root = !strcmp(cfg.browse, MUSIC_ROOT);
    draw_header(at_root ? "Library" : base_name(cfg.browse), at_root ? 0 : B_BACK_DIR, NULL, st.has_queue ? B_TO_PLAYER : 0);
    gfx_clip(0, LIST_Y, SCR_W, SCR_H - LIST_Y);
    int rows = nents + (nents ? 1 : 0);
    list_content_h = rows * ROW_H;
    int max_scroll = list_content_h - (SCR_H - LIST_Y); if (max_scroll < 0) max_scroll = 0;
    if (list_scroll > max_scroll) list_scroll = max_scroll; if (list_scroll < 0) list_scroll = 0;
    if (!nents) gfx_text(FONT_REG, 18, 20, LIST_Y + 30, SCR_W - 40, C_DIM, "No music in this folder");
    for (int r = 0; r < rows; r++) {
        int y = LIST_Y + r * ROW_H - list_scroll;
        if (y + ROW_H < LIST_Y || y > SCR_H) continue;
        int id = r == 0 ? B_PLAY_ALL : B_ENTRY, arg = r - 1;
        if (r == 0) {
            gfx_circle(34, y + ROW_H / 2, 16, C_ACCENT); draw_play_icon(36, y + ROW_H / 2, 13, C_BG);
            gfx_text(FONT_BOLD, 19, 62, y + 18, SCR_W - 80, C_ACCENT, has_audio_here || at_root ? "Play all" : "Play all (with subfolders)");
        } else {
            entry_t *e = &ents[arg];
            char name[512]; snprintf(name, sizeof name, "%s", e->name);
            int playing_this = 0;
            if (e->is_dir) draw_folder_icon(22, y + 17, C_DIM);
            else {
                char full[PATH_MAXLEN]; snprintf(full, sizeof full, "%s/%s", cfg.browse, e->name);
                playing_this = st.has_queue && !strcmp(full, st.path);
                strip_ext(name);
                gfx_text(FONT_REG, 22, 24, y + 15, 0, playing_this ? C_ACCENT : C_DIM, "\xe2\x99\xaa");  // U+266A note
            }
            gfx_text(FONT_REG, 19, 62, y + 18, SCR_W - 62 - (e->is_dir ? 34 : 12), playing_this ? C_ACCENT : C_TEXT, name);
            if (e->is_dir) gfx_text(FONT_REG, 22, SCR_W - 26, y + 15, 0, C_DIM, "\xe2\x80\xba");  // U+203A ›
        }
        gfx_rect(62, y + ROW_H - 1, SCR_W - 62, 1, C_LINE);
        add_button(0, y < LIST_Y ? LIST_Y : y, SCR_W, ROW_H - (y < LIST_Y ? LIST_Y - y : 0), id, arg);
    }
    if (max_scroll > 0) {  // scrollbar
        int vh = SCR_H - LIST_Y, th = vh * vh / list_content_h; if (th < 30) th = 30;
        gfx_rrect(SCR_W - 5, LIST_Y + (vh - th) * list_scroll / max_scroll, 3, th, 1, C_LINE);
    }
    gfx_clip(0, 0, SCR_W, SCR_H);
}

static void draw_player(void) {
    audio_status_t st; audio_status(&st);
    draw_header("", B_TO_LIBRARY, "Library", B_TO_SETTINGS);
    if (!st.has_queue) { gfx_text(FONT_REG, 18, 20, 140, SCR_W - 40, C_DIM, "Nothing is playing"); return; }
    char title[300], line2[600], line3[600];
    if (st.title[0]) snprintf(title, sizeof title, "%s", st.title);
    else { snprintf(title, sizeof title, "%s", base_name(st.path)); strip_ext(title); }
    if (st.artist[0] && st.album[0]) snprintf(line2, sizeof line2, "%s \xe2\x80\x94 %s", st.artist, st.album);
    else snprintf(line2, sizeof line2, "%s", st.artist[0] ? st.artist : st.album);
    char dir[PATH_MAXLEN]; snprintf(dir, sizeof dir, "%s", st.path); char *sl = strrchr(dir, '/'); if (sl) *sl = 0;
    snprintf(line3, sizeof line3, "%s", base_name(dir));
    int y = 108;
    int n = gfx_text_wrap(FONT_BOLD, 26, 20, y, SCR_W - 40, 3, C_TEXT, title);
    y += n * gfx_line_h(FONT_BOLD, 26) + 8;
    if (line2[0]) { y += 26 * gfx_text_wrap(FONT_REG, 18, 20, y, SCR_W - 40, 2, C_DIM, line2) + 2; }
    if (!line2[0] || strcmp(line3, st.album)) { gfx_text(FONT_REG, 16, 20, y, SCR_W - 40, C_DIM, line3); }
    char info[96];
    if (st.rate) snprintf(info, sizeof info, "%s  \xc2\xb7  %.1f kHz  \xc2\xb7  %d bit    %d / %d", st.format, st.rate / 1000.0, st.bits, st.index + 1, st.count);
    else snprintf(info, sizeof info, "%d / %d", st.index + 1, st.count);
    gfx_text(FONT_REG, 15, 20, 300, SCR_W - 40, C_DIM, info);
    if (st.error[0]) gfx_text(FONT_REG, 15, 20, 280, SCR_W - 40, 0xe04848, st.error);
    // progress
    int px = 20, pw = SCR_W - 40, py = 334;
    double frac = st.dur > 0 ? st.pos / st.dur : 0; if (frac > 1) frac = 1; if (frac < 0) frac = 0;
    gfx_rrect(px, py, pw, 6, 3, C_LINE); gfx_rrect(px, py, (int)(pw * frac), 6, 3, C_ACCENT);
    gfx_circle(px + (int)(pw * frac), py + 3, 8, C_TEXT);
    add_button(px - 10, py - 18, pw + 20, 42, B_SEEK, 0);
    char t1[16], t2[16]; fmt_time(t1, sizeof t1, st.pos); fmt_time(t2, sizeof t2, st.dur);
    gfx_text(FONT_REG, 15, px, py + 14, 0, C_DIM, t1);
    gfx_text(FONT_REG, 15, px + pw - gfx_text_w(FONT_REG, 15, t2), py + 14, 0, C_DIM, t2);
    // controls
    int cy = 420;
    draw_skip_icon(80, cy, 26, -1, C_TEXT); add_button(30, cy - 40, 100, 80, B_PREV, 0);
    gfx_circle(SCR_W / 2, cy, 40, C_ACCENT);
    if (st.playing) draw_pause_icon(SCR_W / 2, cy, 28, C_BG); else draw_play_icon(SCR_W / 2 + 2, cy, 30, C_BG);
    add_button(SCR_W / 2 - 50, cy - 50, 100, 100, B_PLAYPAUSE, 0);
    draw_skip_icon(SCR_W - 80, cy, 26, 1, C_TEXT); add_button(SCR_W - 130, cy - 40, 100, 80, B_NEXT, 0);
}

static void draw_settings(void) {
    draw_header("Settings", B_SET_BACK, NULL, 0);
    gfx_text(FONT_REG, 13, SCR_W - 96, 50, 0, C_DIM, "ap80player " VERSION);
    char v[64]; int y = LIST_Y, rh = 54;
    struct { const char *label; int kind, id; } rows[] = {
        { "Brightness", 1, S_BRIGHT }, { "Screen off after", 0, S_TIMEOUT }, { "DAC filter", 0, S_FILTER },
        { "Max volume", 1, S_MAXVOL }, { "On start", 0, S_RESUME },
    };
    for (unsigned i = 0; i < sizeof rows / sizeof rows[0]; i++, y += rh) {
        switch (rows[i].id) {
        case S_BRIGHT: snprintf(v, sizeof v, "%d%%", cfg.brightness * 100 / 255); break;
        case S_TIMEOUT: snprintf(v, sizeof v, "%s", timeout_names[cfg.timeout_idx]); break;
        case S_FILTER: snprintf(v, sizeof v, "%s", filter_names[cfg.filter]); break;
        case S_MAXVOL: snprintf(v, sizeof v, "%d", cfg.max_volume); break;
        case S_RESUME: snprintf(v, sizeof v, "%s", cfg.resume_play ? "Resume playing" : "Paused"); break;
        }
        gfx_text(FONT_REG, 18, 16, y + 16, 150, C_TEXT, rows[i].label);
        if (rows[i].kind == 1) {
            gfx_rrect(SCR_W - 150, y + 7, 40, 40, 8, C_PANEL); gfx_text(FONT_BOLD, 24, SCR_W - 137, y + 11, 0, C_ACCENT, "\xe2\x88\x92");
            gfx_rrect(SCR_W - 52, y + 7, 40, 40, 8, C_PANEL); gfx_text(FONT_BOLD, 24, SCR_W - 40, y + 11, 0, C_ACCENT, "+");
            int w = gfx_text_w(FONT_REG, 18, v); gfx_text(FONT_REG, 18, SCR_W - 81 - w / 2, y + 16, 0, C_TEXT, v);
            add_button(SCR_W - 160, y, 70, rh, B_SET_DEC, rows[i].id); add_button(SCR_W - 70, y, 70, rh, B_SET_INC, rows[i].id);
        } else {
            int w = gfx_text_w(FONT_REG, 17, v); if (w > 190) w = 190;
            gfx_text(FONT_REG, 17, SCR_W - 16 - w, y + 17, 190, C_ACCENT, v);
            add_button(0, y, SCR_W, rh, B_SET_CYCLE, rows[i].id);
        }
        gfx_rect(16, y + rh - 1, SCR_W - 16, 1, C_LINE);
    }
    y += 12;
    if (usb_host_connected()) {  // replaces the stock-player button while a computer is connected
        gfx_rrect(16, y, SCR_W - 32, 46, 10, C_ACCENT); gfx_text(FONT_BOLD, 18, 32, y + 12, 0, C_BG, "USB storage (copy music)");
        add_button(16, y, SCR_W - 32, 46, B_USB_STORAGE, 0); y += 54;
    } else {
    gfx_rrect(16, y, SCR_W - 32, 46, 10, C_PANEL); gfx_text(FONT_REG, 18, 32, y + 12, 0, C_TEXT, "Switch to stock player");
    add_button(16, y, SCR_W - 32, 46, B_STOCK, 0); y += 54;
    }
    gfx_rrect(16, y, SCR_W - 32, 46, 10, C_PANEL); gfx_text(FONT_REG, 18, 32, y + 12, 0, 0xe06060, "Power off  (or hold power)");
    add_button(16, y, SCR_W - 32, 46, B_POWEROFF, 0);
}

static void draw_usb_prompt(void) {
    gfx_rect(0, 28, SCR_W, SCR_H - 28, C_BG);
    gfx_text(FONT_BOLD, 24, 20, 90, SCR_W - 40, C_TEXT, "Connected to computer");
    gfx_text_wrap(FONT_REG, 17, 20, 130, SCR_W - 40, 3, C_DIM, "Use USB storage to copy music to the SD card. Playback stops while the card is connected.");
    gfx_rrect(20, 240, SCR_W - 40, 64, 12, C_ACCENT); gfx_text(FONT_BOLD, 21, 40, 259, 0, C_BG, "USB storage");
    add_button(20, 240, SCR_W - 40, 64, B_USB_STORAGE, 0);
    gfx_rrect(20, 320, SCR_W - 40, 64, 12, C_PANEL); gfx_text(FONT_REG, 21, 40, 339, 0, C_TEXT, "Just charge");
    add_button(20, 320, SCR_W - 40, 64, B_USB_CHARGE, 0);
}
static void draw_usb_storage(void) {
    gfx_rect(0, 28, SCR_W, SCR_H - 28, C_BG);
    gfx_text(FONT_BOLD, 24, 20, 90, SCR_W - 40, C_ACCENT, "USB storage");
    gfx_text_wrap(FONT_REG, 18, 20, 136, SCR_W - 40, 6, C_TEXT,
        "The SD card is now a drive on your computer. Copy your music, then eject the drive on the computer or unplug the cable.");
    gfx_text_wrap(FONT_REG, 15, 20, 262, SCR_W - 40, 2, C_DIM, "The drive can take a few seconds to appear.");
    gfx_text_wrap(FONT_REG, 15, 20, 310, SCR_W - 40, 3, C_DIM, "Tap Done only after ejecting, or files being copied may be damaged.");
    gfx_rrect(20, 380, SCR_W - 40, 60, 12, C_PANEL); gfx_text(FONT_REG, 21, 40, 397, 0, C_TEXT, "Done");
    add_button(20, 380, SCR_W - 40, 60, B_USB_DONE, 0);
}

static void render(void) {
    nbuttons = 0;
    gfx_clear(C_BG);
    if (usb_mode == 1) draw_usb_prompt();
    else if (usb_mode == 2) draw_usb_storage();
    else if (screen == SCR_LIBRARY) draw_library();
    else if (screen == SCR_PLAYER) draw_player();
    else draw_settings();
    draw_status_bar();
    if (toast_until_ms > now_ms()) {
        int w = gfx_text_w(FONT_BOLD, 24, toast) + 48;
        gfx_rrect((SCR_W - w) / 2, 200, w, 64, 14, 0x2e333e);
        gfx_text(FONT_BOLD, 24, (SCR_W - w) / 2 + 24, 216, 0, C_TEXT, toast);
    }
    gfx_present();
    dirty = 0;
}

// ---------- actions ----------
static void set_screen_on(int on) {
    if (on == screen_on) return;
    screen_on = on;
    // Backlight only. jzfb's SET_SLEEP suspends the controller even with a frame in flight; that
    // buffer then stays "busy" forever and a later GET_EMPTY_BUF spins in the kernel (hung UI).
    if (on) { hw_set_brightness(cfg.brightness); dirty = 1; }
    else hw_set_brightness(0);
}
static void change_volume(int d) {
    int v = cfg.volume + d; if (v < 0) v = 0; if (v > cfg.max_volume) v = cfg.max_volume;
    cfg.volume = v; hw_set_volume(v);
    char s[32]; snprintf(s, sizeof s, v ? "Volume %d" : "Muted", v); if (screen_on) show_toast(s);
}
static void enter_dir(const char *dir) { snprintf(cfg.browse, sizeof cfg.browse, "%s", dir); lib_load(cfg.browse); list_scroll = 0; dirty = 1; }

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
    lib_load(cfg.browse); list_scroll = 0;
    if (cfg.track[0] && access(cfg.track, R_OK) == 0) start_queue(cfg.queue_root, cfg.queue_recursive, cfg.track, cfg.pos, 0);
    show_toast("SD card ready");
}

static void settings_change(int id, int d) {
    switch (id) {
    case S_BRIGHT: cfg.brightness += d * 20; if (cfg.brightness < 10) cfg.brightness = 10; if (cfg.brightness > 255) cfg.brightness = 255; hw_set_brightness(cfg.brightness); break;
    case S_TIMEOUT: cfg.timeout_idx = (cfg.timeout_idx + 1) % 6; break;
    case S_FILTER: cfg.filter = (cfg.filter + 1) & 7; hw_set_filter(cfg.filter); break;
    case S_MAXVOL: cfg.max_volume += d * 5; if (cfg.max_volume < 20) cfg.max_volume = 20; if (cfg.max_volume > 100) cfg.max_volume = 100;
        if (cfg.volume > cfg.max_volume) { cfg.volume = cfg.max_volume; hw_set_volume(cfg.volume); } break;
    case S_RESUME: cfg.resume_play = !cfg.resume_play; break;
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
    case B_TO_PLAYER: screen = SCR_PLAYER; dirty = 1; break;
    case B_TO_LIBRARY: screen = SCR_LIBRARY; dirty = 1; break;
    case B_TO_SETTINGS: prev_screen = screen; screen = SCR_SETTINGS; dirty = 1; break;
    case B_SET_BACK: screen = prev_screen; dirty = 1; break;
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
    case B_SEEK: audio_status(&st); if (st.dur > 0) { double f = (double)(tx - 20) / (SCR_W - 40); if (f < 0) f = 0; if (f > 1) f = 1; audio_seek(f * st.dur); } dirty = 1; break;
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
        touch_active = 1; touch_dragging = 0; touch_x0 = e->x; touch_y0 = e->y; touch_scroll0 = list_scroll;
        { button_t *b = hit(e->x, e->y); press_id = b ? b->id : -1; }
        break;
    case EV_TOUCH_MOVE:
        if (!touch_active) break;
        if (!touch_dragging && abs(e->y - touch_y0) > 12) touch_dragging = 1;
        if (touch_dragging && screen == SCR_LIBRARY) { list_scroll = touch_scroll0 - (e->y - touch_y0); dirty = 1; last_input_ms = t; }
        break;
    case EV_TOUCH_UP:
        if (!touch_active) break;
        touch_active = 0; press_id = -1;
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
            for (int j = 0; j < k; j++) handle(&ev[j]);
        }
        if (hw_power_check_long()) { hw_event_t e = { EV_POWER_LONG, 0, 0 }; handle(&e); }
        long t = now_ms();
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
