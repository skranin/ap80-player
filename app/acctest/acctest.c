// Probe the KX126 accelerometer: enable it, then print ABS x/y/z, events and orientation_status.
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/ioctl.h>
#include <linux/input.h>
#include <poll.h>
#include <time.h>
#include <stdlib.h>
static void wr(const char *p, const char *v) { FILE *f = fopen(p, "w"); if (f) { fputs(v, f); fclose(f); } }
static void rd(const char *p, char *b, int n) { FILE *f = fopen(p, "r"); b[0] = 0; if (f) { if (!fgets(b, n, f)) b[0] = 0; fclose(f); } b[strcspn(b, "\n")] = 0; }
int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 5;
    int fd = open("/dev/input/event2", O_RDONLY | O_NONBLOCK);
    wr("/sys/class/input/input2/delay", "100");
    wr("/sys/class/input/input2/enable", "1");
    struct input_absinfo a[3];
    for (int i = 0; i < 3; i++) { ioctl(fd, EVIOCGABS(i), &a[i]); printf("axis%d min=%d max=%d fuzz=%d flat=%d res=%d\n", i, a[i].minimum, a[i].maximum, a[i].fuzz, a[i].flat, a[i].resolution); }
    int x = 0, y = 0, z = 0, nev = 0; time_t end = time(0) + secs, last = 0;
    while (time(0) < end) {
        struct pollfd p = { fd, POLLIN, 0 }; poll(&p, 1, 200);
        struct input_event e;
        while (read(fd, &e, sizeof e) == sizeof e) { nev++; if (e.type == EV_ABS) { if (e.code == 0) x = e.value; if (e.code == 1) y = e.value; if (e.code == 2) z = e.value; } }
        if (time(0) != last) { last = time(0); char o[32]; rd("/sys/class/input/input2/orientation_status", o, sizeof o);
            for (int i = 0; i < 3; i++) ioctl(fd, EVIOCGABS(i), &a[i]);
            printf("ev=%d x=%d y=%d z=%d  abs=%d,%d,%d  orient=%s\n", nev, x, y, z, a[0].value, a[1].value, a[2].value, o); fflush(stdout); }
    }
    wr("/sys/class/input/input2/enable", "0");
    return 0;
}
