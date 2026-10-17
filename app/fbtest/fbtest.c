// Dummy test: draw a pattern on /dev/fb0, then log key/touch events for N seconds.
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/fb.h>
#include <linux/input.h>

static uint32_t *fb; static int W, H, S;
// Panel scans 480-px lines top->bottom, first line at the RIGHT edge.
#define PW 360
#define PH 480
static inline void px(int x, int y, uint32_t c) { if (x >= 0 && x < PW && y >= 0 && y < PH) fb[(PW - 1 - x) * PH + y] = c; }
static void rect(int x, int y, int w, int h, uint32_t c) {
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) px(i, j, c);
}
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 15;
    int fd = open("/dev/fb0", O_RDWR); if (fd < 0) { perror("fb0"); return 1; }
    struct fb_var_screeninfo v; struct fb_fix_screeninfo f;
    ioctl(fd, FBIOGET_VSCREENINFO, &v); ioctl(fd, FBIOGET_FSCREENINFO, &f);
    W = v.xres; H = v.yres; S = f.line_length / 4;
    printf("fb %dx%d bpp=%d stride=%d R%d G%d B%d\n", W, H, v.bits_per_pixel, f.line_length, v.red.offset, v.green.offset, v.blue.offset);
    fb = mmap(0, f.line_length * v.yres_virtual, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    for (int y = 0; y < PH; y++) for (int x = 0; x < PW; x++) px(x, y, 0x202020 + ((x * 160 / PW) << 8));
    rect(0, 0, PW, 40, 0xff0000);          // red    = top
    rect(0, PH - 40, PW, 40, 0x0000ff);    // blue   = bottom
    rect(0, 40, 30, PH - 80, 0x00ff00);    // green  = left
    rect(40, 50, 40, 40, 0xffffff);        // white square = top-left
    #define JZFB_SET_SLEEP 0x80044620
    #define JZFB_GET_EMPTY_BUF 0x80044660
    #define JZFB_START_DMA 0x80044661
    int z = 0, eb = -1;
    printf("set_sleep(0) rc=%d\n", ioctl(fd, JZFB_SET_SLEEP, &z));
    printf("get_empty_buf rc=%d idx=%d\n", ioctl(fd, JZFB_GET_EMPTY_BUF, &eb), eb);
    printf("start_dma rc=%d\n", ioctl(fd, JZFB_START_DMA, 0));
    v.xoffset = 0; v.yoffset = 0;
    printf("pan rc=%d\n", ioctl(fd, FBIOPAN_DISPLAY, &v));
    printf("unblank rc=%d\n", ioctl(fd, FBIOBLANK, FB_BLANK_UNBLANK));
    printf("pan rc=%d\n", ioctl(fd, FBIOPAN_DISPLAY, &v));
    int fds[8]; struct pollfd p[8]; int n = 0; char path[32];
    for (int i = 0; i < 8; i++) { snprintf(path, sizeof path, "/dev/input/event%d", i);
        int e = open(path, O_RDONLY | O_NONBLOCK); if (e >= 0) { fds[n] = i; p[n].fd = e; p[n].events = POLLIN; n++; } }
    time_t end = time(0) + secs; int tx = -1, ty = -1;
    printf("logging input for %ds\n", secs); fflush(stdout);
    while (time(0) < end) {
        if (poll(p, n, 500) <= 0) continue;
        for (int k = 0; k < n; k++) if (p[k].revents & POLLIN) {
            struct input_event ev;
            while (read(p[k].fd, &ev, sizeof ev) == sizeof ev) {
                if (fds[k] >= 2 && fds[k] <= 4) continue;  // skip accelerometer noise
                if (ev.type == EV_SYN) continue;
                printf("event%d type=%d code=%d value=%d\n", fds[k], ev.type, ev.code, ev.value); fflush(stdout);
                if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_X || ev.code == ABS_X)) tx = ev.value;
                if (ev.type == EV_ABS && (ev.code == ABS_MT_POSITION_Y || ev.code == ABS_Y)) ty = ev.value;
                if (tx >= 0 && ty >= 0) { rect(tx - 6, ty - 6, 12, 12, 0xffffffff); ioctl(fd, FBIOPAN_DISPLAY, &v); }
            }
        }
    }
    return 0;
}
