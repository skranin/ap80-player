// Decoder for everything that is not FLAC/MP3/WAV (M4A/ALAC/AAC, Ogg, Opus, APE, WavPack, ...),
// built on a minimal static FFmpeg (see tools/build_ffmpeg.sh).
#pragma once
#include <stdint.h>

typedef struct ffdec ffdec_t;
typedef struct {
    unsigned rate, ch, bits; uint64_t total;   // total length in frames, 0 if unknown
    char title[256], artist[256], album[256], codec[16];
} ffdec_info_t;

ffdec_t *ffdec_open(const char *path, ffdec_info_t *info);
int ffdec_read(ffdec_t *f, int32_t *out, int n);   // up to n stereo frames, 24-bit in 32-bit containers
int ffdec_seek(ffdec_t *f, uint64_t frame);         // 0 on success
void ffdec_close(ffdec_t *f);
