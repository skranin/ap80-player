#include "bt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/wait.h>

static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static pthread_t th;
static bt_status_t S; static int gen;
// pending requests for the worker
static int want_power = -1, want_scan, want_list;
static char want_pair[18], want_forget[18], want_connect[18], want_disconnect[18];

static void changed(void) { gen++; }   // with lk held

// Runs a shell command, collecting stdout+stderr into out. Returns the exit status.
static int run(const char *cmd, char *out, int n) {
    char full[512]; snprintf(full, sizeof full, "%s 2>&1", cmd);
    FILE *p = popen(full, "r"); if (!p) { if (out) out[0] = 0; return -1; }
    int len = 0; char tmp[512];
    for (size_t k; (k = fread(tmp, 1, sizeof tmp, p)) > 0;)
        if (out && len < n - 1) { if (k > (size_t)(n - 1 - len)) k = n - 1 - len; memcpy(out + len, tmp, k); len += k; }
    if (out) out[len] = 0;
    int rc = pclose(p);
    return WIFEXITED(rc) ? WEXITSTATUS(rc) : -1;
}

static int valid_mac(const char *m) {
    if (strlen(m) != 17) return 0;
    for (int i = 0; i < 17; i++) if (i % 3 == 2 ? m[i] != ':' : !strchr("0123456789abcdefABCDEF", m[i])) return 0;
    return 1;
}

static bt_dev_t *find(bt_dev_t *v, int n, const char *mac) {
    for (int i = 0; i < n; i++) if (!strcasecmp(v[i].mac, mac)) return &v[i];
    return NULL;
}

// "bt-device -l" prints some uuid lines, then JSON: {"DEVICE": [{"MAC": "..", "Name": "..", "Paired": 1, ...}, ...]}
static void read_paired(void) {
    static char out[16384];
    run("/usr/bin/bt-device -l", out, sizeof out);
    bt_dev_t v[BT_MAX_DEV]; int n = 0, paired = 0;
    for (char *sp, *l = strtok_r(out, "\n", &sp); l; l = strtok_r(NULL, "\n", &sp)) {
        char *q = strchr(l, '"'); if (!q) continue;
        char key[16], val[64] = "";
        if (sscanf(q, "\"%15[^\"]\":%*[ \t]\"%63[^\"]\"", key, val) < 1 && sscanf(q, "\"%15[^\"]\"", key) != 1) continue;
        if (!strcmp(key, "MAC")) {
            if (n > 0 && !paired) n--;                 // drop the previous one if it was not paired
            if (n == BT_MAX_DEV || !valid_mac(val)) continue;
            memset(&v[n], 0, sizeof v[n]); snprintf(v[n].mac, sizeof v[n].mac, "%s", val); n++; paired = 0;
        } else if (n && !strcmp(key, "Name")) snprintf(v[n - 1].name, sizeof v[n - 1].name, "%s", val);
        else if (n && !strcmp(key, "Paired")) paired = atoi(strchr(q + 1, ':') + 1) != 0;
    }
    if (n > 0 && !paired) n--;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < n; i++) {
        bt_dev_t *o = find(S.paired, S.npaired, v[i].mac);
        v[i].connected = o ? o->connected : 0;
        if (!v[i].name[0]) snprintf(v[i].name, sizeof v[i].name, "%s", v[i].mac);
    }
    memcpy(S.paired, v, sizeof v); S.npaired = n;
    changed(); pthread_mutex_unlock(&lk);
}

// "hcitool con": "\t> ACL 70:26:05:3E:15:A6 handle 11 state 1 lm MASTER AUTH ENCRYPT"
static void read_connections(void) {
    char out[2048]; run("/usr/bin/hcitool con", out, sizeof out);
    pthread_mutex_lock(&lk);
    int unknown = 0;
    for (int i = 0; i < S.npaired; i++) {
        int c = strcasestr(out, S.paired[i].mac) != NULL;
        if (c != S.paired[i].connected) { S.paired[i].connected = c; changed(); }
    }
    for (char *p = out; (p = strstr(p, "ACL ")); p += 4) {
        char mac[18] = ""; sscanf(p + 4, "%17s", mac);
        if (valid_mac(mac) && !find(S.paired, S.npaired, mac)) unknown = 1;
    }
    pthread_mutex_unlock(&lk);
    if (unknown) read_paired();   // a device we do not know yet (just paired from its side)
}

static void read_power(void) {
    char out[2048]; run("/usr/bin/bt-adapter -i", out, sizeof out);
    char *p = strstr(out, "Powered: ");
    pthread_mutex_lock(&lk); S.powered = p && p[9] == '1'; changed(); pthread_mutex_unlock(&lk);
}

static void set_msg(const char *m) { pthread_mutex_lock(&lk); snprintf(S.msg, sizeof S.msg, "%s", m); changed(); pthread_mutex_unlock(&lk); }

// "hcitool scan": "Scanning ...\n\t70:26:05:3E:15:A6\tWH-1000XM3\n"
static void do_scan(void) {
    pthread_mutex_lock(&lk); S.scanning = 1; S.nfound = 0; S.msg[0] = 0; changed(); pthread_mutex_unlock(&lk);
    char out[4096]; run("timeout 30 /usr/bin/hcitool scan", out, sizeof out);
    pthread_mutex_lock(&lk);
    S.nfound = 0;
    for (char *sp, *l = strtok_r(out, "\n", &sp); l; l = strtok_r(NULL, "\n", &sp)) {
        char mac[18] = "", name[64] = "";
        if (sscanf(l, " %17s %63[^\n]", mac, name) < 1 || !valid_mac(mac)) continue;
        if (find(S.paired, S.npaired, mac) || find(S.found, S.nfound, mac) || S.nfound == BT_MAX_DEV) continue;
        bt_dev_t *d = &S.found[S.nfound++]; memset(d, 0, sizeof *d);
        snprintf(d->mac, sizeof d->mac, "%s", mac);
        snprintf(d->name, sizeof d->name, "%s", name[0] && strcmp(name, "n/a") ? name : mac);
    }
    S.scanning = 0;
    snprintf(S.msg, sizeof S.msg, "%s", S.nfound ? "" : "No new devices found. Is the device in pairing mode?");
    changed(); pthread_mutex_unlock(&lk);
}

static void do_pair(const char *mac) {
    pthread_mutex_lock(&lk); snprintf(S.pairing, sizeof S.pairing, "%s", mac); S.msg[0] = 0; changed(); pthread_mutex_unlock(&lk);
    char cmd[128], out[2048];
    snprintf(cmd, sizeof cmd, "timeout 45 /usr/bin/bt-device --capability=NoInputNoOutput -c %s", mac);
    run(cmd, out, sizeof out);
    read_paired();
    pthread_mutex_lock(&lk);
    int ok = find(S.paired, S.npaired, mac) != NULL;
    if (ok) for (int i = 0; i < S.nfound; i++) if (!strcasecmp(S.found[i].mac, mac)) { S.found[i] = S.found[--S.nfound]; break; }
    S.pairing[0] = 0; changed(); pthread_mutex_unlock(&lk);
    if (ok) {   // trusted devices may connect to us on their own (headphones do when switched on)
        snprintf(cmd, sizeof cmd, "/usr/bin/bt-device --set %s Trusted 1", mac); run(cmd, NULL, 0);
    } else set_msg("Pairing failed. Put the device in pairing mode and try again.");
}

static void write_silence(int fd, int frames) {
    static const char z[4096];
    for (int left = frames * 4; left > 0;) { int k = write(fd, z, left < (int)sizeof z ? left : (int)sizeof z); if (k <= 0) break; left -= k; }
}

static void do_connect(const char *mac) {
    int fd; pid_t pid = bt_aplay(mac, 44100, &fd);
    if (pid < 0) return;
    write_silence(fd, 44100 / 5); close(fd);
    int st; waitpid(pid, &st, 0);
    read_connections();
}

static void *worker(void *arg) {
    (void)arg;
    long next_poll = 0;
    read_power(); read_paired();
    pthread_mutex_lock(&lk);
    for (;;) {
        char mac[18];
        if (want_power >= 0) {
            int on = want_power; want_power = -1; pthread_mutex_unlock(&lk);
            run(on ? "/usr/bin/bt-adapter --set Powered 1" : "/usr/bin/bt-adapter --set Powered 0", NULL, 0);
            read_power(); if (on) read_paired();
            pthread_mutex_lock(&lk);
            if (!on) { for (int i = 0; i < S.npaired; i++) S.paired[i].connected = 0; S.nfound = 0; S.msg[0] = 0; changed(); }
            continue;
        }
        if (!S.powered) { want_scan = 0; want_pair[0] = want_connect[0] = 0; }
        #define TAKE(v) (v[0] ? (snprintf(mac, sizeof mac, "%s", v), v[0] = 0, 1) : 0)
        if (TAKE(want_forget)) {
            pthread_mutex_unlock(&lk);
            char cmd[96]; snprintf(cmd, sizeof cmd, "/usr/bin/bt-device -r %s", mac); run(cmd, NULL, 0); read_paired();
            pthread_mutex_lock(&lk); continue;
        }
        if (TAKE(want_disconnect)) {
            pthread_mutex_unlock(&lk);
            char cmd[96]; snprintf(cmd, sizeof cmd, "/usr/bin/bt-device -d %s", mac); run(cmd, NULL, 0); read_connections();
            pthread_mutex_lock(&lk); continue;
        }
        if (TAKE(want_pair)) { pthread_mutex_unlock(&lk); do_pair(mac); pthread_mutex_lock(&lk); continue; }
        if (TAKE(want_connect)) { pthread_mutex_unlock(&lk); do_connect(mac); pthread_mutex_lock(&lk); continue; }
        if (want_scan) { want_scan = 0; pthread_mutex_unlock(&lk); do_scan(); pthread_mutex_lock(&lk); continue; }
        if (want_list) { want_list = 0; pthread_mutex_unlock(&lk); read_paired(); pthread_mutex_lock(&lk); continue; }
        struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
        long now = t.tv_sec * 1000L + t.tv_nsec / 1000000;
        if (S.powered && now >= next_poll) {
            next_poll = now + 2000;
            pthread_mutex_unlock(&lk); read_connections(); pthread_mutex_lock(&lk); continue;
        }
        clock_gettime(CLOCK_REALTIME, &t); t.tv_sec += 1;
        pthread_cond_timedwait(&cv, &lk, &t);
    }
    return NULL;
}

void bt_init(int power) {
    want_power = power ? 1 : 0;
    pthread_create(&th, NULL, worker, NULL);
}

static void request(char *slot, const char *mac) {
    if (!valid_mac(mac)) return;
    pthread_mutex_lock(&lk); snprintf(slot, 18, "%s", mac); pthread_cond_signal(&cv); pthread_mutex_unlock(&lk);
}
void bt_set_power(int on) { pthread_mutex_lock(&lk); want_power = !!on; pthread_cond_signal(&cv); pthread_mutex_unlock(&lk); }
void bt_scan(void) { pthread_mutex_lock(&lk); if (S.powered && !S.scanning) { want_scan = 1; S.scanning = 1; changed(); } pthread_cond_signal(&cv); pthread_mutex_unlock(&lk); }
void bt_pair(const char *mac) {
    request(want_pair, mac);
    pthread_mutex_lock(&lk); snprintf(S.pairing, sizeof S.pairing, "%s", want_pair); changed(); pthread_mutex_unlock(&lk);
}
void bt_forget(const char *mac) { request(want_forget, mac); }
void bt_connect(const char *mac) { request(want_connect, mac); }
void bt_disconnect(const char *mac) { request(want_disconnect, mac); }
int bt_gen(void) { pthread_mutex_lock(&lk); int g = gen; pthread_mutex_unlock(&lk); return g; }
void bt_status(bt_status_t *st) { pthread_mutex_lock(&lk); *st = S; pthread_mutex_unlock(&lk); }
const char *bt_name(const bt_status_t *st, const char *mac) {
    for (int i = 0; i < st->npaired; i++) if (!strcasecmp(st->paired[i].mac, mac)) return st->paired[i].name;
    return NULL;
}

// Each device gets its own HOME with an .asoundrc, so nothing in /etc is touched.
pid_t bt_aplay(const char *mac, unsigned rate, int *fd) {
    if (!valid_mac(mac)) return -1;
    char home[64], rc[96], env_home[80], r[16];
    snprintf(home, sizeof home, "/tmp/ap80bt_%c%c%c%c%c%c%c%c%c%c%c%c", mac[0], mac[1], mac[3], mac[4], mac[6], mac[7],
             mac[9], mac[10], mac[12], mac[13], mac[15], mac[16]);
    mkdir(home, 0755);
    snprintf(rc, sizeof rc, "%s/.asoundrc", home);
    FILE *f = fopen(rc, "w"); if (!f) return -1;
    fprintf(f, "pcm.ap80bt {\n type bluetooth\n bdaddr %s\n profile a2dp\n}\n", mac);
    fclose(f);
    snprintf(env_home, sizeof env_home, "HOME=%s", home); snprintf(r, sizeof r, "%u", rate);
    char *argv[] = { "aplay", "-q", "-D", "ap80bt", "-t", "raw", "-f", "S16_LE", "-c", "2", "-r", r, NULL };
    char *envp[] = { env_home, "PATH=/usr/bin:/bin:/usr/sbin:/sbin", NULL };
    int p[2]; if (pipe2(p, O_CLOEXEC)) return -1;   // the write end must not leak into other children
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }
    if (!pid) {
        dup2(p[0], 0);
        int log = open("/tmp/ap80bt.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (log >= 0) { dup2(log, 1); dup2(log, 2); }
        for (int i = 3; i < 256; i++) close(i);
        execve("/usr/bin/aplay", argv, envp);
        _exit(127);
    }
    close(p[0]);
    *fd = p[1];
    return pid;
}
