FROM --platform=linux/amd64 debian:bookworm
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    crossbuild-essential-mipsel gcc mtd-utils qemu-user-static xorriso genisoimage \
    python3 python3-pip file make cmake pkg-config ca-certificates squashfs-tools \
 && rm -rf /var/lib/apt/lists/*
RUN pip3 install --break-system-packages ubi_reader
WORKDIR /work
