/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "nuvio_input.h"

#include "evo_boot_trace.h"

#include <math.h>
#include <pthread.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ScePadData, as EVO Player reads it. */
typedef struct {
    uint32_t buttons;
    uint8_t  sticks[4];      /* left x, left y, right x, right y; 128 = centre */
    uint8_t  analog[4];      /* L2, R2 */
    float    orientation[4];
    float    acceleration[3];
    float    angular_velocity[3];
    uint8_t  touch[24];
    uint8_t  rest[72];
} pad_data;

int scePadReadState(int handle, pad_data *data);
int scePadOpen(int user_id, int type, int index, void *param);
int scePadClose(int handle);
int scePadSetVibrationMode(int handle, int mode);

/* scePadSetTriggerEffect's parameter (Sony's layout, as Steamworks'
 * isteamdualsense.h carries it: 120 bytes). */
typedef struct {
    uint32_t mode;                 /* 0 off, 1 feedback, 2 weapon, 3 vibration, 4 multi-position, 5 slope */
    uint8_t  padding[4];
    uint8_t  data[48];             /* per mode, see nuvio_input_trigger_resistance */
} pad_trigger_command;
typedef struct {
    uint8_t  trigger_mask;         /* 1 L2, 2 R2 */
    uint8_t  padding[7];
    pad_trigger_command command[2];
} pad_trigger_param;
_Static_assert(sizeof(pad_trigger_param) == 120, "ScePadTriggerEffectParam is 120 bytes");
int scePadSetTriggerEffect(int handle, const pad_trigger_param *param);
int scePadSetVibration(int handle, const void *param);
typedef struct ScePadVibration {
    uint8_t large, small;   /* motors, 0..255 */
} ScePadVibration;

/* Held D-pad: first repeat after 400 ms, then every 90 ms. */
#define REPEAT_DELAY 0.40
#define REPEAT_EVERY 0.09
/* Stick deflection that counts as a D-pad press (of 127). */
#define STICK_ON  88
#define STICK_OFF 60

static int s_pad = -1;
/* The public PS5 pad ABI has a separate remote-control port (16).
 * HDMI Device Link routing here still needs a firmware/TV hardware test.
 * See ps5-native-gamepad-input-research/include/ps5_pad.hpp. */
enum { PAD_PORT_STANDARD = 0, PAD_PORT_REMOTE_CONTROL = 16 };
static int s_remote_pad = -1;
static uint32_t s_remote_ignore;
static uint32_t s_last;
static uint32_t s_ignore;        /* down at open; ignored until released */
static uint32_t s_stick;         /* directions the left stick is holding */
static double s_dir_since;
static double s_next_repeat;

static pthread_mutex_t s_inject_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t s_inject_tap;    /* one-frame presses waiting */
static uint32_t s_inject_hold;   /* held until s_inject_until */
static double s_inject_until;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

void nuvio_input_open(int user_id)
{
    pad_data pad;

    if (s_pad < 0) {
        s_pad = scePadOpen(user_id, PAD_PORT_STANDARD, 0, NULL);
        if (s_pad >= 0)
            evo_bt("input: vibration mode 2 rc=%#x", (unsigned)scePadSetVibrationMode(s_pad, 2));
    }
    evo_bt("input: pad open -> %d", s_pad);
    memset(&pad, 0, sizeof pad);
    s_ignore = (s_pad >= 0 && scePadReadState(s_pad, &pad) == 0) ? pad.buttons : 0;
    if (s_remote_pad < 0)
        s_remote_pad = scePadOpen(user_id, PAD_PORT_REMOTE_CONTROL, 0, NULL);
    evo_bt("input: remote-control pad open -> %d", s_remote_pad);
    memset(&pad, 0, sizeof pad);
    s_remote_ignore = (s_remote_pad >= 0 && scePadReadState(s_remote_pad, &pad) == 0) ? pad.buttons : 0;
    s_last = 0;
    s_stick = 0;
    s_dir_since = s_next_repeat = 0.0;
}

typedef struct ScePadColor {
    uint8_t r, g, b, a;
} ScePadColor;
int scePadSetLightBar(int handle, const ScePadColor *param);
int scePadResetLightBar(int handle);

void nuvio_input_set_lightbar(uint32_t rgb)
{
    if (s_pad < 0)
        return;
    ScePadColor c = {(uint8_t)(rgb >> 16), (uint8_t)(rgb >> 8), (uint8_t)rgb, 255};
    const int rc = scePadSetLightBar(s_pad, &c);
    static int logged;
    if (!logged++)
        evo_bt("input: light bar %06x rc=%#x", (unsigned)rgb, (unsigned)rc);
}

static double s_rumble_until;

/* One attempt: both triggers set to `mode` with `data`; the PS5's answer. */
static int set_triggers(uint32_t mode, const uint8_t *data, int n)
{
    pad_trigger_param p;
    memset(&p, 0, sizeof p);
    p.trigger_mask = 0x03;
    for (int i = 0; i < 2; i++) {
        p.command[i].mode = mode;
        memcpy(p.command[i].data, data, (size_t)n);
    }
    return scePadSetTriggerEffect(s_pad, &p);
}

/* Plain feedback (the only mode some consoles take) is one resistance from a
 * position on. Following the trigger and asking for more the deeper it is gives
 * the throttle feel anyway: zone 0..9 of the travel, its strength 1..8. */
static const uint8_t kZoneStrength[10] = {1, 1, 2, 2, 3, 3, 4, 5, 6, 7};
static int s_follow;             /* the effect is on and follows the triggers */
static int s_zone[2] = {-1, -1}; /* the zone each trigger's resistance was set for */

static void follow_triggers(const uint8_t analog[2])
{
    pad_trigger_param p;
    memset(&p, 0, sizeof p);
    for (int i = 0; i < 2; i++) {
        /* A zone is left only a little past its edge, so a finger held against
         * the resistance on a boundary does not make it flicker. */
        const int v = analog[i] * 10;
        int z = s_zone[i];
        if (z < 0 || v >= (z + 1) * 256 + 96 || v < z * 256 - 96)
            z = v / 256 > 9 ? 9 : v / 256;
        if (z == s_zone[i])
            continue;
        s_zone[i] = z;
        p.trigger_mask |= (uint8_t)(1 << i);
        p.command[i].mode = 1;
        p.command[i].data[0] = (uint8_t)z;
        p.command[i].data[1] = kZoneStrength[z];
    }
    if (p.trigger_mask) {
        const int rc = scePadSetTriggerEffect(s_pad, &p);
        static int logged;
        if (logged < 12) {   /* does the console take the updates, and how often */
            logged++;
            evo_bt("input: trigger follow L2 z%d R2 z%d rc=%#x", s_zone[0], s_zone[1], (unsigned)rc);
        }
    }
}

void nuvio_input_trigger_resistance(int on)
{
    if (s_pad < 0)
        return;
    static int mode_ok = -1;   /* the first that the console took, for the next time */
    s_follow = 0;
    s_zone[0] = s_zone[1] = -1;
    if (!on) {
        const uint8_t none[1] = {0};
        set_triggers(0, none, 1);
        return;
    }
    /* Stiffer the further it goes: multiple-position feedback (mode 4, a strength
     * 0..8 for each of positions 0..9) or slope feedback (mode 5: start, end, start
     * strength, end strength) if the console takes them, else plain feedback
     * (mode 1: position, strength) that follow_triggers moves along. Each answer
     * is logged once. */
    static const uint8_t multi[10] = {1, 1, 2, 2, 3, 3, 4, 5, 6, 7};
    static const uint8_t slope[4] = {0, 9, 1, 7};
    static const uint8_t plain[2] = {0, 1};
    struct { uint32_t mode; const uint8_t *data; int n; } tries[] = {{4, multi, 10}, {5, slope, 4}, {1, plain, 2}};
    static int logged;
    for (int i = 0; i < 3; i++) {
        if (mode_ok >= 0 && (int)tries[i].mode != mode_ok)
            continue;
        const int rc = set_triggers(tries[i].mode, tries[i].data, tries[i].n);
        if (logged < 3) {
            logged++;
            evo_bt("input: trigger effect mode %u rc=%#x", tries[i].mode, (unsigned)rc);
        }
        if (rc >= 0) {
            mode_ok = (int)tries[i].mode;
            s_follow = mode_ok == 1;
            return;
        }
    }
}

void nuvio_input_pulse(int strength, int ms)
{
    if (s_pad < 0)
        return;
    const uint8_t v = (uint8_t)(strength < 0 ? 0 : strength > 255 ? 255 : strength);
    ScePadVibration p = {v, v};
    const int rc = scePadSetVibration(s_pad, &p);
    static int logged;
    if (!logged++)
        evo_bt("input: first vibration rc=%#x", (unsigned)rc);
    s_rumble_until = now_s() + ms / 1000.0;
}

void nuvio_input_reset_lightbar(void)
{
    if (s_pad >= 0)
        scePadResetLightBar(s_pad);
}

void nuvio_input_close(void)
{
    if (s_pad >= 0) {
        static const ScePadVibration off = {0, 0};
        scePadSetVibration(s_pad, &off);   /* never leave a motor running */
        s_rumble_until = 0;
        scePadClose(s_pad);
    }
    s_pad = -1;
    if (s_remote_pad >= 0)
        scePadClose(s_remote_pad);
    s_remote_pad = -1;
    s_remote_ignore = 0;
    s_follow = 0;
    s_last = s_ignore = s_stick = 0;
}

void nuvio_input_inject(uint32_t button, int hold_ms)
{
    pthread_mutex_lock(&s_inject_lock);
    if (hold_ms > 0) {
        s_inject_hold |= button;
        s_inject_until = now_s() + hold_ms / 1000.0;
    } else {
        s_inject_tap |= button;
    }
    pthread_mutex_unlock(&s_inject_lock);
}

uint32_t nuvio_input_button_named(const char *name)
{
    static const struct { const char *name; uint32_t bit; } k[] = {
        {"cross", NUVIO_BTN_CROSS},       {"circle", NUVIO_BTN_CIRCLE},
        {"square", NUVIO_BTN_SQUARE},     {"triangle", NUVIO_BTN_TRIANGLE},
        {"up", NUVIO_BTN_UP},             {"down", NUVIO_BTN_DOWN},
        {"left", NUVIO_BTN_LEFT},         {"right", NUVIO_BTN_RIGHT},
        {"options", NUVIO_BTN_OPTIONS},   {"touchpad", NUVIO_BTN_TOUCHPAD},
        {"l1", NUVIO_BTN_L1},             {"r1", NUVIO_BTN_R1},
        {"l2", NUVIO_BTN_L2},             {"r2", NUVIO_BTN_R2},
        {"l3", NUVIO_BTN_L3},             {"r3", NUVIO_BTN_R3},
    };
    for (unsigned i = 0; name && i < sizeof k / sizeof k[0]; i++)
        if (!strcasecmp(name, k[i].name))
            return k[i].bit;
    return 0;
}

/* The left stick as a D-pad, with hysteresis so a stick resting near the
 * threshold does not chatter. */
static uint32_t stick_dirs(const pad_data *pad)
{
    const int x = (int)pad->sticks[0] - 128;
    const int y = (int)pad->sticks[1] - 128;
    uint32_t d = 0;
    int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
    int on_x = (s_stick & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) ? STICK_OFF : STICK_ON;
    int on_y = (s_stick & (NUVIO_BTN_UP | NUVIO_BTN_DOWN)) ? STICK_OFF : STICK_ON;

    /* One axis at a time: the dominant one. */
    if (ax >= ay && ax > on_x)
        d = x < 0 ? NUVIO_BTN_LEFT : NUVIO_BTN_RIGHT;
    else if (ay > ax && ay > on_y)
        d = y < 0 ? NUVIO_BTN_UP : NUVIO_BTN_DOWN;
    s_stick = d;
    return d;
}

void nuvio_input_poll(nuvio_input_state *out)
{
    pad_data pad;
    uint32_t now_buttons = 0;
    const double t = now_s();

    memset(out, 0, sizeof *out);
    if (s_rumble_until > 0 && t >= s_rumble_until && s_pad >= 0) {   /* the pulse ends */
        static const ScePadVibration off = {0, 0};
        scePadSetVibration(s_pad, &off);
        s_rumble_until = 0;
    }
    memset(&pad, 0, sizeof pad);
    if (s_pad >= 0 && scePadReadState(s_pad, &pad) == 0) {
        out->l2 = pad.analog[0] / 255.f;
        out->r2 = pad.analog[1] / 255.f;
        now_buttons = pad.buttons;
        if (s_follow)
            follow_triggers(pad.analog);
        if (!(now_buttons & NUVIO_BTN_DPAD))
            now_buttons |= stick_dirs(&pad);
    }

    /* Buttons held when the pad was opened stay ignored until let go. */
    s_ignore &= now_buttons;
    now_buttons &= ~s_ignore;

    /* Keep the launch-button suppression per device: a held remote OK must
     * not suppress a fresh DualSense X (or the reverse). Remote input has no
     * stick/trigger interpretation, rumble, light bar or adaptive effects. */
    memset(&pad, 0, sizeof pad);
    uint32_t remote_buttons = 0;
    if (s_remote_pad >= 0 && scePadReadState(s_remote_pad, &pad) == 0) {
        const uint32_t known = NUVIO_BTN_DPAD | NUVIO_BTN_CROSS | NUVIO_BTN_CIRCLE |
                               NUVIO_BTN_SQUARE | NUVIO_BTN_TRIANGLE | NUVIO_BTN_OPTIONS |
                               NUVIO_BTN_L1 | NUVIO_BTN_R1 | NUVIO_BTN_L2 | NUVIO_BTN_R2 |
                               NUVIO_BTN_L3 | NUVIO_BTN_R3 | NUVIO_BTN_TOUCHPAD;
        /* The system owns intercepted samples (e.g. its keyboard/dialogs). */
        if (!(pad.buttons & 0x80000000u))
            remote_buttons = pad.buttons & known;
    }
    s_remote_ignore &= remote_buttons;
    now_buttons |= remote_buttons & ~s_remote_ignore;

    pthread_mutex_lock(&s_inject_lock);
    if (s_inject_hold && t >= s_inject_until)
        s_inject_hold = 0;
    now_buttons |= s_inject_hold;
    {
        /* A tap is down for exactly one poll; one already down is re-pressed. */
        uint32_t tap = s_inject_tap;
        s_inject_tap = 0;
        out->pressed |= tap;
        now_buttons |= tap;
        s_last &= ~tap;
    }
    pthread_mutex_unlock(&s_inject_lock);

    out->pressed |= now_buttons & ~s_last;
    out->released = s_last & ~now_buttons;
    out->held = now_buttons;

    /* Auto-repeat for one held direction (L2/R2 too: they scrub in the player). */
    {
        const uint32_t kRepeat = NUVIO_BTN_DPAD | NUVIO_BTN_L2 | NUVIO_BTN_R2;
        const uint32_t dir = now_buttons & kRepeat;
        if (out->pressed & kRepeat) {
            s_dir_since = t;
            s_next_repeat = t + REPEAT_DELAY;
        } else if (dir && t >= s_next_repeat && s_dir_since > 0.0) {
            out->pressed |= dir;
            out->repeats |= dir;
            s_next_repeat = t + REPEAT_EVERY;
        }
        if (!dir)
            s_dir_since = 0.0;
        out->held_for = dir && s_dir_since > 0.0 ? t - s_dir_since : 0.0;
    }
    s_last = now_buttons;
}
