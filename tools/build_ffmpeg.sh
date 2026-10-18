#!/bin/sh
# Builds a small static FFmpeg (decoders and demuxers for audio only) for the player.
# Output: app/player/third_party/ffmpeg/{include,lib}. Run once from the repo root:
#   tools/build_ffmpeg.sh
# FFmpeg is LGPL 2.1; it is linked statically and this repo's source is public, so it can be relinked.
set -e
D=$(cd "$(dirname "$0")/.." && pwd)
VER=7.1.1
SHA=733984395e0dbbe5c046abda2dc49a5544e7e0e1e2366bba849222ae9e3a03b1
DL="$D/app/player/third_party/ffmpeg-dl"
mkdir -p "$DL"
[ -f "$DL/ffmpeg-$VER.tar.xz" ] || curl -sSfL -o "$DL/ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"
echo "$SHA  $DL/ffmpeg-$VER.tar.xz" | shasum -a 256 -c -

DEMUXERS=mov,ogg,ape,wv,tta,tak,aiff,asf,dsf,iff,mpc,mpc8,aac,matroska,w64,caf,wav,flac,mp3
DECODERS=alac,aac,aac_latm,vorbis,opus,ape,wavpack,tta,tak,wmav1,wmav2,wmalossless,wmapro,mpc7,mpc8,\
dsd_lsbf,dsd_msbf,dsd_lsbf_planar,dsd_msbf_planar,flac,mp3,mp3float,mp2,mp2float,\
pcm_s16le,pcm_s16be,pcm_s24le,pcm_s24be,pcm_s32le,pcm_s32be,pcm_f32le,pcm_f32be,pcm_f64le,pcm_f64be,pcm_u8,pcm_s8
PARSERS=aac,aac_latm,mpegaudio,flac,vorbis,opus

docker run --rm -v "$D/app/player/third_party":/t ap80-tc sh -ec "
  command -v gcc >/dev/null || { apt-get update -qq && apt-get install -y -qq gcc >/dev/null; }   # configure needs a host compiler
  rm -rf /tmp/src && mkdir /tmp/src && tar -xJf /t/ffmpeg-dl/ffmpeg-$VER.tar.xz -C /tmp/src
  cd /tmp/src/ffmpeg-$VER
  ./configure --prefix=/t/ffmpeg --enable-cross-compile --cross-prefix=mipsel-linux-gnu- --arch=mips --target-os=linux \
    --extra-cflags='-march=mips32r2 -O2' --cpu=generic --disable-asm \
    --enable-static --disable-shared --disable-programs --disable-doc --disable-network --disable-everything \
    --disable-avdevice --disable-avfilter --disable-swscale --disable-postproc \
    --disable-iconv --disable-zlib --disable-bzlib --disable-lzma --disable-xlib --disable-sdl2 --disable-vaapi --disable-vdpau \
    --enable-protocol=file --enable-demuxer=$DEMUXERS --enable-decoder=$DECODERS --enable-parser=$PARSERS
  make -j\$(nproc) && rm -rf /t/ffmpeg && make install
"
ls -la "$D/app/player/third_party/ffmpeg/lib"
