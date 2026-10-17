#include "audio.h"
#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <pthread.h>
#include <tinyalsa/asoundlib.h>

#define DR_FLAC_IMPLEMENTATION
#define DR_MP3_IMPLEMENTATION
#define DR_WAV_IMPLEMENTATION
#include "../third_party/dr_flac.h"
#include "../third_party/dr_mp3.h"
#include "../third_party/dr_wav.h"

#define PERIOD_FRAMES 2048
#define PERIOD_COUNT 4

enum { DEC_NONE, DEC_FLAC, DEC_MP3, DEC_WAV };
typedef struct {
    int kind; drflac *flac; drmp3 mp3; drwav wav;
    unsigned rate, ch, bits; uint64_t total, pos;
    char title[256], artist[256], album[256];
} dec_t;

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t th;
static char **q; static int qn, qi;
static int playing, quit, load_pending, autoplay_after_load;
static double load_pos = 0, want_seek = -1;
static dec_t dec; static int dec_open;
static struct pcm *pcm; static unsigned pcm_rate;
static int amp_on; static time_t last_active;
static char err[128];

static long mono_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec; }

int audio_is_audio_file(const char *n) {
    const char *e = strrchr(n, '.'); if (!e) return 0;
    return !strcasecmp(e, ".flac") || !strcasecmp(e, ".mp3") || !strcasecmp(e, ".wav");
}

// ---------- tags ----------
static void copy_tag(char *dst, const char *src, int n) { if (n > 255) n = 255; memcpy(dst, src, n); dst[n] = 0; }

static void flac_meta(void *ud, drflac_metadata *m) {
    dec_t *d = ud;
    if (m->type != DRFLAC_METADATA_BLOCK_TYPE_VORBIS_COMMENT) return;
    drflac_vorbis_comment_iterator it; drflac_uint32 len; const char *c;
    drflac_init_vorbis_comment_iterator(&it, m->data.vorbis_comment.commentCount, m->data.vorbis_comment.pComments);
    while ((c = drflac_next_vorbis_comment(&it, &len))) {
        if (len > 6 && !strncasecmp(c, "TITLE=", 6)) copy_tag(d->title, c + 6, len - 6);
        else if (len > 7 && !strncasecmp(c, "ARTIST=", 7)) copy_tag(d->artist, c + 7, len - 7);
        else if (len > 6 && !strncasecmp(c, "ALBUM=", 6)) copy_tag(d->album, c + 6, len - 6);
    }
}

static int put_utf8(char *o, uint32_t c) {
    if (c < 0x80) { o[0] = c; return 1; }
    if (c < 0x800) { o[0] = 0xc0 | c >> 6; o[1] = 0x80 | (c & 63); return 2; }
    o[0] = 0xe0 | c >> 12; o[1] = 0x80 | (c >> 6 & 63); o[2] = 0x80 | (c & 63); return 3;
}
// ID3 "latin1" text in Russian libraries is almost always cp1251, so bytes >= 0x80 are decoded as cp1251.
static uint32_t cp1251(unsigned char b) {
    if (b >= 0xc0) return 0x410 + (b - 0xc0);
    if (b == 0xa8) return 0x401; if (b == 0xb8) return 0x451;
    return b < 0x80 ? b : '?';
}
static void id3_text(char *dst, const unsigned char *p, int n) {
    int o = 0; if (n < 1) { dst[0] = 0; return; }
    int enc = p[0]; p++; n--;
    if (enc == 1 || enc == 2) {
        int be = enc == 2;
        if (n >= 2 && p[0] == 0xff && p[1] == 0xfe) { be = 0; p += 2; n -= 2; }
        else if (n >= 2 && p[0] == 0xfe && p[1] == 0xff) { be = 1; p += 2; n -= 2; }
        for (int i = 0; i + 1 < n && o < 250; i += 2) {
            uint32_t c = be ? (p[i] << 8 | p[i + 1]) : (p[i + 1] << 8 | p[i]);
            if (!c) break; if (c >= 0xd800 && c < 0xe000) c = '?';
            o += put_utf8(dst + o, c);
        }
    } else if (enc == 3) {
        for (int i = 0; i < n && p[i] && o < 254; i++) dst[o++] = p[i];
    } else {
        for (int i = 0; i < n && p[i] && o < 250; i++) o += put_utf8(dst + o, cp1251(p[i]));
    }
    dst[o] = 0;
}
static void read_id3(const char *path, dec_t *d) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    unsigned char h[10];
    if (fread(h, 1, 10, f) != 10 || memcmp(h, "ID3", 3) || h[3] < 3 || h[3] > 4) { fclose(f); return; }
    long size = (h[6] & 127) << 21 | (h[7] & 127) << 14 | (h[8] & 127) << 7 | (h[9] & 127);
    if (size > 512 * 1024) size = 512 * 1024;  // tags only, skip giant embedded covers
    unsigned char *b = malloc(size); if (!b) { fclose(f); return; }
    size = fread(b, 1, size, f); fclose(f);
    for (long i = 0; i + 10 <= size;) {
        const unsigned char *fr = b + i;
        if (!fr[0]) break;
        long fs = h[3] == 4 ? ((fr[4] & 127) << 21 | (fr[5] & 127) << 14 | (fr[6] & 127) << 7 | (fr[7] & 127))
                            : ((long)fr[4] << 24 | fr[5] << 16 | fr[6] << 8 | fr[7]);
        if (fs <= 0 || i + 10 + fs > size) break;
        int tn = fs > 600 ? 600 : (int)fs;
        if (!memcmp(fr, "TIT2", 4)) id3_text(d->title, fr + 10, tn);
        else if (!memcmp(fr, "TPE1", 4)) id3_text(d->artist, fr + 10, tn);
        else if (!memcmp(fr, "TALB", 4)) id3_text(d->album, fr + 10, tn);
        i += 10 + fs;
    }
    free(b);
}

// ---------- decoders ----------
static void dec_close(dec_t *d) {
    if (d->kind == DEC_FLAC) drflac_close(d->flac);
    else if (d->kind == DEC_MP3) drmp3_uninit(&d->mp3);
    else if (d->kind == DEC_WAV) drwav_uninit(&d->wav);
    d->kind = DEC_NONE;
}
static int dec_open_file(dec_t *d, const char *path) {
    memset(d, 0, sizeof *d);
    const char *e = strrchr(path, '.'); if (!e) return -1;
    if (!strcasecmp(e, ".flac")) {
        d->flac = drflac_open_file_with_metadata(path, flac_meta, d, NULL);
        if (!d->flac) return -1;
        d->kind = DEC_FLAC; d->rate = d->flac->sampleRate; d->ch = d->flac->channels; d->bits = d->flac->bitsPerSample; d->total = d->flac->totalPCMFrameCount;
    } else if (!strcasecmp(e, ".mp3")) {
        if (!drmp3_init_file(&d->mp3, path, NULL)) return -1;
        d->kind = DEC_MP3; d->rate = d->mp3.sampleRate; d->ch = d->mp3.channels; d->bits = 16; d->total = drmp3_get_pcm_frame_count(&d->mp3);
        read_id3(path, d);
    } else if (!strcasecmp(e, ".wav")) {
        if (!drwav_init_file(&d->wav, path, NULL)) return -1;
        d->kind = DEC_WAV; d->rate = d->wav.sampleRate; d->ch = d->wav.channels; d->bits = d->wav.bitsPerSample; d->total = d->wav.totalPCMFrameCount;
    } else return -1;
    if (!d->ch || !d->rate) { dec_close(d); return -1; }
    return 0;
}
// Reads up to n frames as stereo, 24-bit samples in 32-bit containers (S24_LE).
static int dec_read(dec_t *d, int32_t *out, int n) {
    static int32_t tmp[PERIOD_FRAMES * 8]; static int16_t tmp16[PERIOD_FRAMES * 8];
    unsigned ch = d->ch > 8 ? 8 : d->ch; uint64_t got = 0;
    if (d->kind == DEC_FLAC) got = drflac_read_pcm_frames_s32(d->flac, n, tmp);
    else if (d->kind == DEC_WAV) got = drwav_read_pcm_frames_s32(&d->wav, n, tmp);
    else if (d->kind == DEC_MP3) { got = drmp3_read_pcm_frames_s16(&d->mp3, n, tmp16); for (uint64_t i = 0; i < got * ch; i++) tmp[i] = (int32_t)tmp16[i] << 16; }
    for (uint64_t i = 0; i < got; i++) {
        int32_t l = tmp[i * ch], r = ch > 1 ? tmp[i * ch + 1] : l;
        out[i * 2] = l >> 8; out[i * 2 + 1] = r >> 8;
    }
    d->pos += got; return (int)got;
}
static void dec_seek(dec_t *d, uint64_t frame) {
    if (frame > d->total) frame = d->total;
    int ok = 0;
    if (d->kind == DEC_FLAC) ok = drflac_seek_to_pcm_frame(d->flac, frame);
    else if (d->kind == DEC_MP3) ok = drmp3_seek_to_pcm_frame(&d->mp3, frame);
    else if (d->kind == DEC_WAV) ok = drwav_seek_to_pcm_frame(&d->wav, frame);
    if (ok) d->pos = frame;
}

// ---------- output ----------
static void pcm_release(void) { if (pcm) { pcm_close(pcm); pcm = NULL; } }
static int pcm_ensure(unsigned rate) {
    if (pcm && pcm_rate == rate) return 0;
    pcm_release();
    if (!amp_on) { hw_set_output(1); amp_on = 1; usleep(30000); }
    struct pcm_config cfg = { .channels = 2, .rate = rate, .period_size = PERIOD_FRAMES, .period_count = PERIOD_COUNT, .format = PCM_FORMAT_S24_LE };
    pcm = pcm_open(0, 0, PCM_OUT, &cfg);
    if (!pcm || !pcm_is_ready(pcm)) {
        snprintf(err, sizeof err, "Audio output error: %s", pcm ? pcm_get_error(pcm) : "open");
        fprintf(stderr, "%s\n", err); pcm_release(); return -1;
    }
    pcm_rate = rate; return 0;
}

// ---------- worker ----------
static void *worker(void *arg) {
    (void)arg;
    static int32_t buf[PERIOD_FRAMES * 2];
    pthread_mutex_lock(&lk);
    while (!quit) {
        if (load_pending) {
            load_pending = 0;
            char *path = qi >= 0 && qi < qn ? strdup(q[qi]) : NULL;
            double pos = load_pos; int ap = autoplay_after_load;
            if (dec_open) { dec_close(&dec); dec_open = 0; }
            pthread_mutex_unlock(&lk);
            dec_t nd; int rc = path ? dec_open_file(&nd, path) : -1;
            if (!rc && pos > 0) dec_seek(&nd, (uint64_t)(pos * nd.rate));
            pthread_mutex_lock(&lk);
            if (!path) { playing = 0; err[0] = 0; }          // queue cleared: all files closed
            else if (rc) {
                snprintf(err, sizeof err, "Cannot play %s", path ? (strrchr(path, '/') ? strrchr(path, '/') + 1 : path) : "(none)");
                if (ap && qi + 1 < qn) { qi++; load_pos = 0; load_pending = 1; autoplay_after_load = 1; }
                else playing = 0;
            } else if (load_pending) { dec_close(&nd); }   // superseded while opening
            else { dec = nd; dec_open = 1; playing = ap; err[0] = 0; }
            free(path);
            continue;
        }
        if (want_seek >= 0 && dec_open) { dec_seek(&dec, (uint64_t)(want_seek * dec.rate)); want_seek = -1; }
        if (!playing || !dec_open) {
            if (pcm) { pthread_mutex_unlock(&lk); pcm_release(); pthread_mutex_lock(&lk); last_active = mono_s(); }
            if (!load_pending && want_seek < 0 && !quit && !(playing && dec_open)) pthread_cond_wait(&cv, &lk);
            continue;
        }
        unsigned rate = dec.rate;
        pthread_mutex_unlock(&lk);
        int n = 0, ok = pcm_ensure(rate) == 0;
        if (ok) {
            n = dec_read(&dec, buf, PERIOD_FRAMES);
            if (n > 0 && pcm_writei(pcm, buf, n) < 0) { fprintf(stderr, "pcm_writei: %s\n", pcm_get_error(pcm)); pcm_release(); }
        }
        pthread_mutex_lock(&lk);
        last_active = mono_s();
        if (!ok) { playing = 0; continue; }
        if (n <= 0 && !load_pending && want_seek < 0) {          // end of track
            if (qi + 1 < qn) { qi++; load_pos = 0; load_pending = 1; autoplay_after_load = 1; }
            else { playing = 0; load_pos = 0; load_pending = 1; autoplay_after_load = 0; }  // end of queue: rewind, stop
        }
    }
    pthread_mutex_unlock(&lk);
    pcm_release();
    return NULL;
}

int audio_init(void) { last_active = mono_s(); return pthread_create(&th, NULL, worker, NULL); }

void audio_shutdown(void) {
    pthread_mutex_lock(&lk); quit = 1; pthread_cond_signal(&cv); pthread_mutex_unlock(&lk);
    pthread_join(th, NULL);
    if (dec_open) dec_close(&dec);
    hw_set_output(0); amp_on = 0;
}

void audio_set_queue(char **paths, int count, int index, double pos, int autoplay) {
    pthread_mutex_lock(&lk);
    for (int i = 0; i < qn; i++) free(q[i]); free(q);
    q = paths; qn = count; qi = index < 0 || index >= count ? 0 : index;
    load_pos = pos; load_pending = 1; autoplay_after_load = autoplay && count > 0; want_seek = -1;
    if (!count) playing = 0;
    pthread_cond_signal(&cv); pthread_mutex_unlock(&lk);
}

static void cmd_play(int p) {
    pthread_mutex_lock(&lk);
    if (qn) { if (load_pending) autoplay_after_load = p; else playing = p && dec_open; }
    pthread_cond_signal(&cv); pthread_mutex_unlock(&lk);
}
void audio_play(void) { cmd_play(1); }
void audio_pause(void) { cmd_play(0); }
void audio_toggle(void) {
    pthread_mutex_lock(&lk); int p = load_pending ? autoplay_after_load : playing; pthread_mutex_unlock(&lk);
    cmd_play(!p);
}
static void jump(int to) {
    pthread_mutex_lock(&lk);
    if (to >= 0 && to < qn) { int p = load_pending ? autoplay_after_load : playing; qi = to; load_pos = 0; load_pending = 1; autoplay_after_load = p; want_seek = -1; }
    pthread_cond_signal(&cv); pthread_mutex_unlock(&lk);
}
void audio_next(void) { pthread_mutex_lock(&lk); int t = qi + 1; pthread_mutex_unlock(&lk); jump(t); }
void audio_prev(void) {
    pthread_mutex_lock(&lk);
    double pos = dec_open && dec.rate ? (double)dec.pos / dec.rate : 0; int t = qi - 1;
    if (pos > 3 || t < 0) { want_seek = 0; pthread_cond_signal(&cv); pthread_mutex_unlock(&lk); return; }
    pthread_mutex_unlock(&lk); jump(t);
}
void audio_seek(double sec) {
    pthread_mutex_lock(&lk); if (dec_open) want_seek = sec < 0 ? 0 : sec; pthread_cond_signal(&cv); pthread_mutex_unlock(&lk);
}

int audio_files_closed(void) {
    pthread_mutex_lock(&lk); int c = !dec_open && !load_pending && !pcm; pthread_mutex_unlock(&lk); return c;
}

int audio_idle_seconds(void) {
    pthread_mutex_lock(&lk); int idle = playing ? 0 : (int)(mono_s() - last_active); int on = amp_on; pthread_mutex_unlock(&lk);
    return on ? idle : -1;
}

void audio_status(audio_status_t *st) {
    memset(st, 0, sizeof *st);
    pthread_mutex_lock(&lk);
    st->has_queue = qn > 0; st->count = qn; st->index = qi;
    st->playing = load_pending ? autoplay_after_load : playing;
    if (qn && qi < qn) snprintf(st->path, sizeof st->path, "%s", q[qi]);
    if (dec_open && !load_pending) {
        st->pos = want_seek >= 0 ? want_seek : (double)dec.pos / dec.rate; st->dur = (double)dec.total / dec.rate;
        st->rate = dec.rate; st->bits = dec.bits; st->channels = dec.ch;
        snprintf(st->title, sizeof st->title, "%s", dec.title); snprintf(st->artist, sizeof st->artist, "%s", dec.artist);
        snprintf(st->album, sizeof st->album, "%s", dec.album);
        snprintf(st->format, sizeof st->format, "%s", dec.kind == DEC_FLAC ? "FLAC" : dec.kind == DEC_MP3 ? "MP3" : "WAV");
    } else st->pos = load_pos;
    snprintf(st->error, sizeof st->error, "%s", err);
    pthread_mutex_unlock(&lk);
}

void audio_amp_off(void) {
    pthread_mutex_lock(&lk);
    if (!playing && !pcm && amp_on) { hw_set_output(0); amp_on = 0; }
    pthread_mutex_unlock(&lk);
}
