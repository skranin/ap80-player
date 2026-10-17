#include "usb.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mount.h>
#include <sys/ioctl.h>
#include <linux/fs.h>

#define GADGET "/sys/class/android_usb/android0"
#define LUN_FILE GADGET "/f_mass_storage/lun/file"
#define SD_DISK "/dev/mmcblk0"
#define SD_PART "/dev/mmcblk0p1"
#define SD_MOUNT "/mnt/sd_0"
#define SD_FSTYPE "exfat"
#define SD_OPTS "fmask=0022,dmask=0022,iocharset=utf8,namecase=1,errors=remount-ro"  // same as stock

static int exporting;

static int write_str(const char *path, const char *s) {
    FILE *f = fopen(path, "w"); if (!f) return -1;
    int rc = fputs(s, f) < 0 ? -1 : 0; if (fclose(f)) rc = -1; return rc;
}
static int read_str(const char *path, char *buf, int n) {
    FILE *f = fopen(path, "r"); buf[0] = 0; if (!f) return -1;
    if (!fgets(buf, n, f)) buf[0] = 0; fclose(f); buf[strcspn(buf, "\r\n")] = 0; return 0;
}

int usb_host_connected(void) { char s[32]; read_str(GADGET "/state", s, sizeof s); return !strcmp(s, "CONFIGURED"); }
int usb_power(void) { char s[8]; read_str("/sys/class/power_supply/usb/online", s, sizeof s); return s[0] == '1'; }

// The firmware's exFAT driver leaves the VolumeDirty flag set after unmount, which makes macOS
// run a full fsck of the card before mounting it. After a clean unmount, clear it ourselves.
// VolumeFlags (boot sector offset 106) is excluded from the exFAT boot checksum.
static void clear_exfat_dirty(void) {
    int fd = open(SD_PART, O_RDWR | O_SYNC); if (fd < 0) return;
    unsigned char b[2], name[8];
    if (pread(fd, name, 8, 3) == 8 && !memcmp(name, "EXFAT   ", 8) && pread(fd, b, 2, 106) == 2 && (b[0] & 2)) {
        b[0] &= ~2;
        if (pwrite(fd, b, 2, 106) == 2) fsync(fd);
    }
    ioctl(fd, BLKFLSBUF, 0); close(fd);
    fd = open(SD_DISK, O_RDONLY);  // the USB export reads the whole-disk device; drop its cached copy too
    if (fd >= 0) { ioctl(fd, BLKFLSBUF, 0); close(fd); }
}

static void gadget(const char *functions) {
    write_str(GADGET "/enable", "0");
    write_str(GADGET "/functions", functions);
    write_str(GADGET "/enable", "1");
}

int usb_storage_enter(char *msg, int n) {
    sync();
    int tries = 0;
    while (umount2(SD_MOUNT, 0) && tries++ < 15) {
        if (errno == EINVAL) break;                  // not mounted
        usleep(200000);
    }
    if (tries > 15) { snprintf(msg, n, "SD card is busy (%s)", strerror(errno)); return -1; }
    clear_exfat_dirty();
    gadget("mass_storage,adb");
    if (write_str(LUN_FILE, SD_DISK) && write_str(LUN_FILE, SD_PART)) {
        snprintf(msg, n, "Cannot export SD card");
        gadget("adb");
        mount(SD_PART, SD_MOUNT, SD_FSTYPE, MS_RELATIME, SD_OPTS);
        return -1;
    }
    exporting = 1;
    return 0;
}

int usb_storage_done(void) {
    if (!exporting) return 1;
    char f[256]; read_str(LUN_FILE, f, sizeof f);
    return !f[0] || !usb_power();                     // host ejected the medium, or the cable is gone
}

int usb_storage_exit(char *msg, int n) {
    write_str(LUN_FILE, "");
    gadget("adb");
    exporting = 0;
    int rc = -1;
    for (int i = 0; i < 10 && rc; i++) {
        rc = mount(SD_PART, SD_MOUNT, SD_FSTYPE, MS_RELATIME, SD_OPTS);
        if (rc && errno == EBUSY) { rc = 0; break; }  // already mounted
        if (rc) usleep(300000);
    }
    if (rc) { snprintf(msg, n, "Cannot mount SD card (%s)", strerror(errno)); return -1; }
    return 0;
}
