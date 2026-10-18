#include "hw.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <errno.h>
#include <sys/mount.h>
#include <linux/input.h>
#include <tinyalsa/asoundlib.h>

#define OUTPUT_PORT_HP  2   // what stock uses for 3.5 mm during playback
#define OUTPUT_PORT_OFF 5   // disables the amp power regulator
#define LONG_PRESS_MS 1500

static struct mixer *mx;
static pthread_mutex_t mx_lock = PTHREAD_MUTEX_INITIALIZER;
static int in_fds[4], in_n, touch_fd = -1;
static int touch_x, touch_y, touch_down, touch_reported;
static long power_down_at; static int power_long_fired;

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000; }

static void mixer_set(const char *name, int v) {
    pthread_mutex_lock(&mx_lock);
    struct mixer_ctl *c = mx ? mixer_get_ctl_by_name(mx, name) : NULL;
    if (c) for (unsigned i = 0; i < mixer_ctl_get_num_values(c); i++) mixer_ctl_set_value(c, i, v);
    else fprintf(stderr, "mixer: no control '%s'\n", name);
    pthread_mutex_unlock(&mx_lock);
}

static int read_int(const char *path, int def) {
    FILE *f = fopen(path, "r"); if (!f) return def;
    int v = def; if (fscanf(f, "%d", &v) != 1) v = def; fclose(f); return v;
}
static void write_int(const char *path, int v) { FILE *f = fopen(path, "w"); if (f) { fprintf(f, "%d\n", v); fclose(f); } }

int hw_init(void) {
    mx = mixer_open(0);
    if (!mx) fprintf(stderr, "mixer_open failed\n");
    // event0 = volume knob, event1 = in-line remote, event5 = buttons, event6 = touch
    const char *devs[] = { "/dev/input/event0", "/dev/input/event1", "/dev/input/event5", "/dev/input/event6" };
    for (int i = 0; i < 4; i++) {
        int fd = open(devs[i], O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        if (i == 3) touch_fd = fd;
        in_fds[in_n++] = fd;
    }
    return 0;
}

int hw_input_fds(int *fds, int max) { int n = in_n < max ? in_n : max; memcpy(fds, in_fds, n * sizeof(int)); return n; }

static int map_key(int code) {
    switch (code) {
    case KEY_PLAYPAUSE: case KEY_PLAYCD: case KEY_PAUSECD: case KEY_MEDIA: return EV_KEY_PLAY;
    case KEY_PREVIOUSSONG: case KEY_FASTFORWARD: return EV_KEY_NEXT;   // kernel labels Next/Prev swapped
    case KEY_NEXTSONG: case KEY_REWIND: return EV_KEY_PREV;
    case KEY_RIGHT: case KEY_VOLUMEUP: return EV_VOL_UP;
    case KEY_LEFT: case KEY_VOLUMEDOWN: return EV_VOL_DOWN;
    }
    return EV_NONE;
}

int hw_input_read(int fd, hw_event_t *out, int max) {
    struct input_event ev; int n = 0;
    while (n < max && read(fd, &ev, sizeof ev) == sizeof ev) {
        if (fd == touch_fd) {
            if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
                touch_down = ev.value;
                if (!ev.value && touch_reported) { out[n++] = (hw_event_t){ EV_TOUCH_UP, touch_x, touch_y }; touch_reported = 0; }
            } else if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X)) touch_x = ev.value;
            else if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y)) touch_y = ev.value;
            else if (ev.type == EV_SYN && ev.code == SYN_REPORT && touch_down) {
                out[n++] = (hw_event_t){ touch_reported ? EV_TOUCH_MOVE : EV_TOUCH_DOWN, touch_x, touch_y };
                touch_reported = 1;
            }
            continue;
        }
        if (ev.type != EV_KEY) continue;
        if (ev.code == KEY_POWER) {
            if (ev.value == 1) { power_down_at = now_ms(); power_long_fired = 0; }
            else if (ev.value == 0 && power_down_at) {
                if (!power_long_fired) out[n++] = (hw_event_t){ EV_POWER_SHORT, 0, 0 };
                power_down_at = 0;
            }
            continue;
        }
        int t = map_key(ev.code);
        if (t != EV_NONE && ev.value != 0) out[n++] = (hw_event_t){ t, 0, 0 };  // press and autorepeat
    }
    return n;
}

int hw_power_pending_ms(void) { return power_down_at ? (int)(now_ms() - power_down_at) : 0; }
int hw_power_check_long(void) {
    if (power_down_at && !power_long_fired && now_ms() - power_down_at >= LONG_PRESS_MS) { power_long_fired = 1; return 1; }
    return 0;
}

// ES92xx volume is attenuation in 0.5 dB steps: 0 = 0 dB (loudest), 255 = mute.
// UI 1..100 maps to -79 dB .. 0 dB, 0.8 dB per UI step.
void hw_set_volume(int vol) {
    if (vol < 0) vol = 0; if (vol > 100) vol = 100;
    int att = vol == 0 ? 255 : (100 - vol) * 8 / 5;
    mixer_set("Left Playback Volume", att);
    mixer_set("Right Playback Volume", att);
}
void hw_set_output(int on) { mixer_set("Output Port Switch", on ? OUTPUT_PORT_HP : OUTPUT_PORT_OFF); }
void hw_set_filter(int f) { mixer_set("Digital Filter", f & 7); }
void hw_set_brightness(int b) { write_int("/sys/class/backlight/pwm-backlight.0/brightness", b); }
int hw_battery(void) { return read_int("/sys/class/power_supply/battery/capacity", -1); }
int hw_charging(void) {
    FILE *f = fopen("/sys/class/power_supply/battery/status", "r"); char s[32] = "";
    if (f) { if (!fgets(s, sizeof s, f)) s[0] = 0; fclose(f); }
    return !strncmp(s, "Charging", 8) || !strncmp(s, "Full", 4);
}
int hw_headset(void) {
    if (getenv("AP80_FAKE_HEADSET")) return 1;   // for the qemu preview, which has no headset switch
    return read_int("/sys/class/switch/headset/state", 0) != 0;
}

// ---------- accelerometer ----------
// The kx126 driver's event stream is unusable here (it waits for a data-ready interrupt that never
// fires, and its polled path only reports an X-axis flip). Its debug hook still reads any register:
// writing an address to reg_read logs "kx126_sysfs_reg_read reg 0x9 val 0xe2" through dynamic debug,
// which we pick up from /dev/kmsg. We read only the high bytes of X/Y/Z (about 32 per g at +-4 g).
#define ACC_SYS "/sys/class/input/input2"
#define DYNDBG "/sys/kernel/debug/dynamic_debug/control"
static int acc_on, acc_kmsg = -1, acc_hi[3], acc_got, acc_have;

static void write_str(const char *path, const char *v) { FILE *f = fopen(path, "w"); if (f) { fputs(v, f); fclose(f); } }

void hw_accel_enable(int on) {
    on = !!on;
    if (on == acc_on) return;
    if (on) {
        if (access(DYNDBG, W_OK)) mount("none", "/sys/kernel/debug", "debugfs", 0, NULL);
        write_str(DYNDBG, "func kx126_sysfs_reg_read +p");
        write_int(ACC_SYS "/delay", 100);
        write_int(ACC_SYS "/enable", 1);
        acc_kmsg = open("/dev/kmsg", O_RDONLY | O_NONBLOCK);
        if (acc_kmsg >= 0) lseek(acc_kmsg, 0, SEEK_END);
    } else {
        write_int(ACC_SYS "/enable", 0);
        write_str(DYNDBG, "func kx126_sysfs_reg_read -p");
        if (acc_kmsg >= 0) close(acc_kmsg);
        acc_kmsg = -1;
    }
    acc_on = on; acc_have = 0;
}

void hw_accel_sample(void) {
    if (!acc_on || acc_kmsg < 0) return;
    static const char *regs[3] = { "0x09", "0x0b", "0x0d" };   // XOUT_H, YOUT_H, ZOUT_H
    for (int i = 0; i < 3; i++) write_str(ACC_SYS "/reg_read", regs[i]);
    char rec[512]; int n;
    acc_got = 0;
    while ((n = read(acc_kmsg, rec, sizeof rec - 1)) != 0) {
        if (n < 0) { if (errno == EPIPE) continue; break; }   // EPIPE: records were overwritten
        rec[n] = 0;
        char *m = strstr(rec, "kx126_sysfs_reg_read reg "); unsigned reg, val;
        if (!m || sscanf(m, "kx126_sysfs_reg_read reg %x val %x", &reg, &val) != 2) continue;
        int v = (signed char)val;
        if (reg == 0x09) { acc_hi[0] = v; acc_got |= 1; }
        else if (reg == 0x0b) { acc_hi[1] = v; acc_got |= 2; }
        else if (reg == 0x0d) { acc_hi[2] = v; acc_got |= 4; }
    }
    if (acc_got == 7) acc_have = 1;
}

// Measured on the device: held upright, gravity reads chip -X; turned left (top edge left, so the
// left edge is down), chip -Y.
int hw_accel_gravity(int *gx, int *gy, int *gz) {
    if (!acc_have) return 0;
    *gx = acc_hi[1]; *gy = -acc_hi[0]; *gz = acc_hi[2];
    return 1;
}
