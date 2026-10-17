// Hardware: buttons/knob/touch input, DAC mixer (volume, output port, filter), backlight, battery, headset.
#pragma once

enum {
    EV_NONE, EV_KEY_PLAY, EV_KEY_NEXT, EV_KEY_PREV, EV_VOL_UP, EV_VOL_DOWN,
    EV_POWER_SHORT, EV_POWER_LONG, EV_TOUCH_DOWN, EV_TOUCH_MOVE, EV_TOUCH_UP,
};
typedef struct { int type, x, y; } hw_event_t;

int hw_init(void);
int hw_input_fds(int *fds, int max);       // fds to poll for input
// Reads pending input from a ready fd; returns the number of events written to out.
int hw_input_read(int fd, hw_event_t *out, int max);
int hw_power_pending_ms(void);             // >0 while power is held (for long-press detection)
int hw_power_check_long(void);             // returns 1 once when the hold passes the long-press threshold

void hw_set_volume(int vol);               // 0..100 (0 = mute); mapped to DAC attenuation
void hw_set_output(int on);                // 1 = 3.5 mm headphone path, 0 = amp off
void hw_set_filter(int f);                 // DAC digital filter 0..7
void hw_set_brightness(int b);             // 0..255
int hw_battery(void);                      // percent, or -1
int hw_charging(void);
int hw_headset(void);                      // 1 if headphones are plugged in

// KX126 accelerometer. Off by default to save power.
void hw_accel_enable(int on);
void hw_accel_sample(void);                // takes a reading (a few ms); call a few times a second
// Gravity in portrait screen axes: +x = toward the right edge, +y = toward the bottom; ~32 per g.
// Returns 0 until a reading has arrived.
int hw_accel_gravity(int *gx, int *gy, int *gz);
