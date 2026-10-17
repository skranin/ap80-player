// Playback engine: a queue of files decoded on a worker thread and written to ALSA hw:0,0.
#pragma once

#define PATH_MAXLEN 1024

typedef struct {
    int has_queue, playing, index, count;
    double pos, dur;                    // seconds
    int rate, bits, channels;
    char path[PATH_MAXLEN];
    char title[256], artist[256], album[256], format[16];
    char error[128];
} audio_status_t;

int audio_init(void);
void audio_shutdown(void);
// Replaces the queue (takes ownership of paths: an array of malloc'd strings) and starts at index/pos.
void audio_set_queue(char **paths, int count, int index, double pos, int autoplay);
void audio_toggle(void);
void audio_play(void);
void audio_pause(void);
void audio_next(void);
void audio_prev(void);
void audio_seek(double sec);
void audio_status(audio_status_t *st);
int audio_idle_seconds(void);           // seconds since output was last active, -1 if the amp is already off
void audio_amp_off(void);               // power the headphone amp down if nothing is playing
int audio_files_closed(void);           // 1 when no track file is open (safe to unmount the SD card)
int audio_is_audio_file(const char *name);
