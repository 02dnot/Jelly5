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
int scePadSetMotionSensorState(int handle, int enable);

/* scePadSetTriggerEffect's parameter (Sony's layout, as Steamworks'
 * isteamdualsense.h carries it: 120 bytes). */
typedef struct {
    uint32_t mode;                 /* 0 off, 5 slope feedback */
    uint8_t  padding[4];
    uint8_t  data[48];             /* slope: start position, end position, start strength, end strength */
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
        s_pad = scePadOpen(user_id, 0, 0, NULL);
        if (s_pad >= 0) {
            evo_bt("input: vibration mode 2 rc=%#x", (unsigned)scePadSetVibrationMode(s_pad, 2));
            evo_bt("input: motion sensor rc=%#x", (unsigned)scePadSetMotionSensorState(s_pad, 1));
        }
    }
    evo_bt("input: pad open -> %d", s_pad);
    memset(&pad, 0, sizeof pad);
    s_ignore = (s_pad >= 0 && scePadReadState(s_pad, &pad) == 0) ? pad.buttons : 0;
    s_last = s_ignore;
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

/* Tilt, for the glass's light: gravity as the accelerometer sees it, against a
 * slowly following rest pose, so it is how far the pad leans from the way it is
 * usually held (whatever that is), and drifts back when held still. */
static float s_fast[3], s_rest[3], s_tilt_x, s_tilt_y;
static int s_tilt_primed;

static void tilt_update(const pad_data *pad, double t)
{
    static double last;
    const float dt = last > 0 ? (float)(t - last) : 0.f;
    last = t;
    const float *a = pad->acceleration;
    if (a[0] == 0.f && a[1] == 0.f && a[2] == 0.f)
        return;   /* no motion data (sensor off or not a DualSense) */
    if (!s_tilt_primed) {
        for (int i = 0; i < 3; i++)
            s_fast[i] = s_rest[i] = a[i];
        s_tilt_primed = 1;
        return;
    }
    const float kf = dt > 0 ? 1.f - expf(-dt * 12.f) : 1.f, kr = dt > 0 ? 1.f - expf(-dt * 0.25f) : 0.f;
    for (int i = 0; i < 3; i++) {
        s_fast[i] += (a[i] - s_fast[i]) * kf;
        s_rest[i] += (s_fast[i] - s_rest[i]) * kr;
    }
    const float x = (s_fast[0] - s_rest[0]) / 0.35f, y = (s_fast[2] - s_rest[2]) / 0.35f;
    s_tilt_x = x < -1.f ? -1.f : x > 1.f ? 1.f : x;
    s_tilt_y = y < -1.f ? -1.f : y > 1.f ? 1.f : y;
}

void nuvio_input_trigger_resistance(int on)
{
    if (s_pad < 0)
        return;
    pad_trigger_param p;
    memset(&p, 0, sizeof p);
    p.trigger_mask = 0x03;
    for (int i = 0; i < 2; i++) {
        p.command[i].mode = on ? 5u : 0u;   /* slope feedback: light at first, stiffer further in */
        if (on) {
            p.command[i].data[0] = 0;   /* from the very start */
            p.command[i].data[1] = 9;   /* to the end */
            p.command[i].data[2] = 3;   /* strength 3 */
            p.command[i].data[3] = 8;   /* up to 8, the most */
        }
    }
    const int rc = scePadSetTriggerEffect(s_pad, &p);
    static int logged;
    if (!logged++)
        evo_bt("input: trigger effect %s rc=%#x", on ? "on" : "off", (unsigned)rc);
}

void nuvio_input_tilt(float *x, float *y)
{
    *x = s_tilt_x;
    *y = s_tilt_y;
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
        tilt_update(&pad, t);
        out->l2 = pad.analog[0] / 255.f;
        out->r2 = pad.analog[1] / 255.f;
        now_buttons = pad.buttons;
        if (!(now_buttons & NUVIO_BTN_DPAD))
            now_buttons |= stick_dirs(&pad);
    }

    /* Buttons held when the pad was opened stay ignored until let go. */
    s_ignore &= now_buttons;
    now_buttons &= ~s_ignore;

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
