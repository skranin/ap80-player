#include "ffdec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>

struct ffdec {
    AVFormatContext *fc; AVCodecContext *cc; AVPacket *pkt; AVFrame *fr;
    int si, fpos, flushed, eof; unsigned rate;
    int64_t start;     // stream start time, in stream time base
    int64_t skip_to;   // after a seek: drop decoded audio before this frame (-1 = none)
    int dec, acc_n; int64_t acc_l, acc_r;   // DSD: average this many decoded samples per output sample
};

static void tag(AVDictionary *m, const char *key, char *dst) {
    AVDictionaryEntry *e = dst[0] ? NULL : av_dict_get(m, key, NULL, 0);
    if (e && e->value[0]) snprintf(dst, 256, "%s", e->value);
}

static const char *codec_label(enum AVCodecID id, const char *container) {
    switch (id) {
    case AV_CODEC_ID_ALAC: return "ALAC";
    case AV_CODEC_ID_AAC: case AV_CODEC_ID_AAC_LATM: return "AAC";
    case AV_CODEC_ID_VORBIS: return "Vorbis";
    case AV_CODEC_ID_OPUS: return "Opus";
    case AV_CODEC_ID_APE: return "APE";
    case AV_CODEC_ID_WAVPACK: return "WavPack";
    case AV_CODEC_ID_TTA: return "TTA";
    case AV_CODEC_ID_TAK: return "TAK";
    case AV_CODEC_ID_WMALOSSLESS: return "WMA Lossless";
    case AV_CODEC_ID_WMAV1: case AV_CODEC_ID_WMAV2: case AV_CODEC_ID_WMAPRO: return "WMA";
    case AV_CODEC_ID_MUSEPACK7: case AV_CODEC_ID_MUSEPACK8: return "Musepack";
    case AV_CODEC_ID_DSD_LSBF: case AV_CODEC_ID_DSD_MSBF: case AV_CODEC_ID_DSD_LSBF_PLANAR: case AV_CODEC_ID_DSD_MSBF_PLANAR: return "DSD";
    case AV_CODEC_ID_FLAC: return "FLAC";
    case AV_CODEC_ID_MP2: return "MP2";
    case AV_CODEC_ID_MP3: return "MP3";
    default: break;
    }
    if (!strncmp(container, "aiff", 4)) return "AIFF";
    if (!strncmp(container, "caf", 3)) return "CAF";
    return "PCM";
}
static int lossy(enum AVCodecID id) {
    return id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_AAC_LATM || id == AV_CODEC_ID_VORBIS || id == AV_CODEC_ID_OPUS ||
           id == AV_CODEC_ID_WMAV1 || id == AV_CODEC_ID_WMAV2 || id == AV_CODEC_ID_WMAPRO || id == AV_CODEC_ID_MUSEPACK7 ||
           id == AV_CODEC_ID_MUSEPACK8 || id == AV_CODEC_ID_MP2 || id == AV_CODEC_ID_MP3;
}

void ffdec_close(ffdec_t *f) {
    if (!f) return;
    av_frame_free(&f->fr); av_packet_free(&f->pkt);
    avcodec_free_context(&f->cc); avformat_close_input(&f->fc);
    free(f);
}

ffdec_t *ffdec_open(const char *path, ffdec_info_t *info) {
    memset(info, 0, sizeof *info);
    av_log_set_level(AV_LOG_ERROR);
    ffdec_t *f = calloc(1, sizeof *f); if (!f) return NULL;
    f->skip_to = -1;
    const AVCodec *codec = NULL;
    if (avformat_open_input(&f->fc, path, NULL, NULL) < 0) goto fail;
    if (avformat_find_stream_info(f->fc, NULL) < 0) goto fail;
    f->si = av_find_best_stream(f->fc, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (f->si < 0 || !codec) goto fail;
    for (unsigned i = 0; i < f->fc->nb_streams; i++) if ((int)i != f->si) f->fc->streams[i]->discard = AVDISCARD_ALL;   // cover art etc.
    AVStream *st = f->fc->streams[f->si];
    if (!(f->cc = avcodec_alloc_context3(codec)) || avcodec_parameters_to_context(f->cc, st->codecpar) < 0) goto fail;
    f->cc->pkt_timebase = st->time_base;
    f->start = st->start_time != AV_NOPTS_VALUE ? st->start_time : 0;
    if (avcodec_open2(f->cc, codec, NULL) < 0) goto fail;
    if (!(f->pkt = av_packet_alloc()) || !(f->fr = av_frame_alloc())) goto fail;

    f->rate = f->cc->sample_rate;
    info->ch = f->cc->ch_layout.nb_channels;
    if (!f->rate || !info->ch) goto fail;
    enum AVCodecID id = st->codecpar->codec_id;
    // DSD comes out as PCM at 1/8 of the DSD rate (352.8 kHz for DSD64). Bring it to <= 192 kHz,
    // which both the DAC and the Bluetooth plugin take.
    f->dec = 1;
    if (id >= AV_CODEC_ID_DSD_LSBF && id <= AV_CODEC_ID_DSD_MSBF_PLANAR) while (f->rate / f->dec > 192000) f->dec *= 2;
    info->rate = f->rate / f->dec;
    info->bits = lossy(id) ? 16 : st->codecpar->bits_per_raw_sample ? st->codecpar->bits_per_raw_sample
               : f->cc->bits_per_raw_sample ? f->cc->bits_per_raw_sample : st->codecpar->bits_per_coded_sample;
    if (id >= AV_CODEC_ID_DSD_LSBF && id <= AV_CODEC_ID_DSD_MSBF_PLANAR) info->bits = 1;
    if (!info->bits || info->bits > 32) info->bits = av_get_bytes_per_sample(f->cc->sample_fmt) * 8;
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0) info->total = av_rescale_q(st->duration, st->time_base, (AVRational){ 1, info->rate });
    else if (f->fc->duration != AV_NOPTS_VALUE && f->fc->duration > 0) info->total = av_rescale(f->fc->duration, info->rate, AV_TIME_BASE);
    tag(f->fc->metadata, "title", info->title); tag(st->metadata, "title", info->title);
    tag(f->fc->metadata, "artist", info->artist); tag(st->metadata, "artist", info->artist);
    tag(f->fc->metadata, "album_artist", info->artist);
    tag(f->fc->metadata, "album", info->album); tag(st->metadata, "album", info->album);
    snprintf(info->codec, sizeof info->codec, "%s", codec_label(id, f->fc->iformat->name));
    if (f->dec > 1 || info->bits == 1) snprintf(info->codec, sizeof info->codec, "DSD%u", f->rate * 8 / 44100);   // DSD64, DSD128, ...
    return f;
fail:
    ffdec_close(f);
    return NULL;
}

// One sample of channel c at index i, as a 24-bit value.
static inline int32_t sample(const AVFrame *fr, int planar, int nch, int c, int i) {
    int k = planar ? i : i * nch + c;
    const uint8_t *p = fr->extended_data[planar ? c : 0];
    double d;
    switch (fr->format) {
    case AV_SAMPLE_FMT_U8: case AV_SAMPLE_FMT_U8P: return ((int32_t)p[k] - 128) << 16;
    case AV_SAMPLE_FMT_S16: case AV_SAMPLE_FMT_S16P: return (int32_t)((const int16_t *)p)[k] << 8;
    case AV_SAMPLE_FMT_S32: case AV_SAMPLE_FMT_S32P: return ((const int32_t *)p)[k] >> 8;
    case AV_SAMPLE_FMT_FLT: case AV_SAMPLE_FMT_FLTP: d = ((const float *)p)[k]; break;
    case AV_SAMPLE_FMT_DBL: case AV_SAMPLE_FMT_DBLP: d = ((const double *)p)[k]; break;
    default: return 0;
    }
    if (d > 1) d = 1; if (d < -1) d = -1;
    return (int32_t)(d * 8388607.0);
}

// Gets the next decoded frame into f->fr. Returns 0, or -1 at the end of the stream / on error.
static int next_frame(ffdec_t *f) {
    for (;;) {
        int r = avcodec_receive_frame(f->cc, f->fr);
        if (r == 0) return 0;
        if (r != AVERROR(EAGAIN)) return -1;   // AVERROR_EOF after the flush, or a decoder error
        if (f->flushed) return -1;
        r = av_read_frame(f->fc, f->pkt);
        if (r < 0) { avcodec_send_packet(f->cc, NULL); f->flushed = 1; continue; }   // drain the decoder
        if (f->pkt->stream_index == f->si) avcodec_send_packet(f->cc, f->pkt);     // a bad packet is skipped
        av_packet_unref(f->pkt);
    }
}

int ffdec_read(ffdec_t *f, int32_t *out, int n) {
    int got = 0;
    while (got < n && !f->eof) {
        if (f->fpos >= f->fr->nb_samples) {
            if (next_frame(f)) { f->eof = 1; break; }
            f->fpos = 0;
            if (f->skip_to >= 0 && f->fr->pts != AV_NOPTS_VALUE) {
                int64_t start = av_rescale_q(f->fr->pts - f->start, f->fc->streams[f->si]->time_base, (AVRational){ 1, f->rate });
                if (start + f->fr->nb_samples <= f->skip_to) { f->fr->nb_samples = 0; continue; }   // still before the target
                if (start < f->skip_to) f->fpos = (int)(f->skip_to - start);
            }
            f->skip_to = -1;
            continue;
        }
        int nch = f->fr->ch_layout.nb_channels, planar = av_sample_fmt_is_planar(f->fr->format);
        if (f->dec == 1) {
            int k = f->fr->nb_samples - f->fpos; if (k > n - got) k = n - got;
            for (int i = 0; i < k; i++) {
                int s = f->fpos + i;
                int32_t l = sample(f->fr, planar, nch, 0, s), r = nch > 1 ? sample(f->fr, planar, nch, 1, s) : l;
                out[(got + i) * 2] = l; out[(got + i) * 2 + 1] = r;
            }
            f->fpos += k; got += k;
        } else for (; f->fpos < f->fr->nb_samples && got < n; f->fpos++) {
            int s = f->fpos;
            f->acc_l += sample(f->fr, planar, nch, 0, s); f->acc_r += nch > 1 ? sample(f->fr, planar, nch, 1, s) : sample(f->fr, planar, nch, 0, s);
            if (++f->acc_n == f->dec) {
                out[got * 2] = (int32_t)(f->acc_l / f->dec); out[got * 2 + 1] = (int32_t)(f->acc_r / f->dec); got++;
                f->acc_l = f->acc_r = 0; f->acc_n = 0;
            }
        }
    }
    return got;
}

int ffdec_seek(ffdec_t *f, uint64_t frame) {
    AVStream *st = f->fc->streams[f->si];
    frame *= f->dec; f->acc_l = f->acc_r = 0; f->acc_n = 0;   // decoded (not output) frames from here on
    int64_t ts = av_rescale_q(frame, (AVRational){ 1, f->rate }, st->time_base);
    ts += f->start;
    if (av_seek_frame(f->fc, f->si, ts, AVSEEK_FLAG_BACKWARD) < 0) return -1;
    avcodec_flush_buffers(f->cc);
    f->fr->nb_samples = 0; f->fpos = 0; f->flushed = 0; f->eof = 0;
    f->skip_to = frame;
    return 0;
}
