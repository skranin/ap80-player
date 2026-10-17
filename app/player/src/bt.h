// Bluetooth control. The stock init scripts already run BlueZ 4 (bluetoothd, bt-agent); we drive it
// with its command-line tools on a worker thread so the UI never waits for them.
#pragma once
#include <sys/types.h>

#define BT_MAX_DEV 16
typedef struct { char mac[18], name[64]; int connected; } bt_dev_t;
typedef struct {
    int powered, scanning;
    char pairing[18];                         // device being paired, "" if none
    bt_dev_t paired[BT_MAX_DEV]; int npaired;
    bt_dev_t found[BT_MAX_DEV]; int nfound;   // last scan, minus paired devices
    char msg[96];                             // outcome of the last scan or pairing
} bt_status_t;

void bt_init(int power);
void bt_set_power(int on);
void bt_scan(void);
void bt_pair(const char *mac);
void bt_forget(const char *mac);
void bt_connect(const char *mac);            // opens a short stream so the device connects now
void bt_disconnect(const char *mac);
int bt_gen(void);                            // changes whenever the status changes
void bt_status(bt_status_t *st);
const char *bt_name(const bt_status_t *st, const char *mac);   // NULL if not paired

// Starts aplay on the device's ALSA "bluetooth" pcm (A2DP, codec chosen by the plugin), reading
// raw S16_LE stereo at this rate from the returned pipe. Returns the pid, or -1.
pid_t bt_aplay(const char *mac, unsigned rate, int *fd);
