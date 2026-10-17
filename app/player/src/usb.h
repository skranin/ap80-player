// USB mass storage: hand the SD card to a connected computer and take it back.
#pragma once

int usb_host_connected(void);       // 1 when enumerated by a computer (not just a charger)
int usb_power(void);                // 1 when USB power is present
// Unmounts the SD card and exports it over USB (adb stays on). Returns 0 or -1 with msg filled in.
int usb_storage_enter(char *msg, int n);
// 1 when the computer ejected the card or the cable was removed.
int usb_storage_done(void);
// Stops exporting and remounts the SD card. Returns 0 or -1.
int usb_storage_exit(char *msg, int n);
