/* Synthetic tests of the production input owner; no console or server needed.
 * SPDX-License-Identifier: GPL-3.0-or-later */
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <stdio.h>
#include <time.h>
#include "evo_boot_trace.h"

/* Consume diagnostic arguments like an app build, without console imports. */
static void test_trace(const char *format, ...) { (void)format; }
#undef evo_bt
#define evo_bt(...) test_trace(__VA_ARGS__)

static double test_time = 1;
static int test_clock_gettime(clockid_t id, struct timespec *out)
{
    (void)id;
    out->tv_sec = (time_t)test_time;
    out->tv_nsec = (long)((test_time - out->tv_sec) * 1e9);
    return 0;
}
#define clock_gettime test_clock_gettime
#include "../src/nuvio_input.c"
#undef clock_gettime

enum { STANDARD_HANDLE = 10, REMOTE_HANDLE = 20 };
static pad_data standard, remote;
static int standard_available, remote_available, standard_error, remote_error;
static int opens[2], closes[2], output_calls;

int scePadOpen(int user, int type, int index, void *param)
{
    assert(user == 7 && index == 0 && param == NULL);
    assert(type == 0 || type == 16);
    int which = type == 16;
    ++opens[which];
    return (which ? remote_available : standard_available) ?
        (which ? REMOTE_HANDLE : STANDARD_HANDLE) : -1;
}
int scePadReadState(int handle, pad_data *out)
{
    assert(handle == STANDARD_HANDLE || handle == REMOTE_HANDLE);
    if (handle == REMOTE_HANDLE ? remote_error : standard_error)
        return -1;
    *out = handle == REMOTE_HANDLE ? remote : standard;
    return 0;
}
int scePadClose(int handle)
{
    assert(handle == STANDARD_HANDLE || handle == REMOTE_HANDLE);
    ++closes[handle == REMOTE_HANDLE];
    return 0;
}
static int output(int handle)
{
    assert(handle == STANDARD_HANDLE); /* Never send effects to the TV remote. */
    ++output_calls;
    return 0;
}
int scePadSetVibrationMode(int handle, int mode) { assert(mode == 2); return output(handle); }
int scePadSetTriggerEffect(int handle, const pad_trigger_param *p) { (void)p; return output(handle); }
int scePadSetVibration(int handle, const void *p) { (void)p; return output(handle); }
int scePadSetLightBar(int handle, const ScePadColor *p) { (void)p; return output(handle); }
int scePadResetLightBar(int handle) { return output(handle); }

static void reset(int have_standard, int have_remote)
{
    nuvio_input_close();
    memset(&standard, 0, sizeof standard);
    memset(&remote, 0, sizeof remote);
    memset(standard.sticks, 128, sizeof standard.sticks);
    memset(remote.sticks, 128, sizeof remote.sticks);
    memset(opens, 0, sizeof opens);
    memset(closes, 0, sizeof closes);
    standard_available = have_standard;
    remote_available = have_remote;
    standard_error = remote_error = output_calls = 0;
    test_time = 1;
}
static nuvio_input_state poll(void)
{
    nuvio_input_state state;
    nuvio_input_poll(&state);
    return state;
}

int main(void)
{
    /* HDMI navigation shares the shell/player buttons and their repeat policy. */
    reset(1, 1);
    nuvio_input_open(7);
    assert(opens[0] == 1 && opens[1] == 1);
    for (size_t i = 0; i < 6; ++i) {
        const uint32_t buttons[] = {NUVIO_BTN_UP, NUVIO_BTN_DOWN, NUVIO_BTN_LEFT,
            NUVIO_BTN_RIGHT, NUVIO_BTN_CROSS, NUVIO_BTN_CIRCLE};
        remote.buttons = buttons[i];
        assert(poll().pressed == buttons[i]);
        assert(poll().pressed == 0);
        remote.buttons = 0;
        assert(poll().released == buttons[i]);
    }
    remote.buttons = NUVIO_BTN_DOWN;
    assert(poll().pressed == NUVIO_BTN_DOWN);
    test_time = 1.39;
    assert(poll().pressed == 0);
    test_time = 1.41;
    nuvio_input_state state = poll();
    assert(state.pressed == NUVIO_BTN_DOWN && state.repeats == NUVIO_BTN_DOWN);
    remote.buttons = 0;
    poll();
    remote.buttons = NUVIO_BTN_CROSS;
    assert(poll().pressed == NUVIO_BTN_CROSS);
    test_time = 3;
    assert(poll().pressed == 0); /* Held OK never repeatedly activates. */

    /* A second device holding the same button causes no duplicate or early release. */
    standard.buttons = NUVIO_BTN_CROSS;
    assert(poll().pressed == 0);
    remote.buttons = 0;
    assert(poll().released == 0 && poll().held == NUVIO_BTN_CROSS);
    standard.buttons = 0;
    assert(poll().released == NUVIO_BTN_CROSS);
    remote.buttons = 0x80000000u; /* Intercepted/unknown flags are not UI actions. */
    assert(poll().held == 0 && poll().pressed == 0);
    remote.buttons |= NUVIO_BTN_CROSS;
    assert(poll().held == 0 && poll().pressed == 0); /* System-owned OK is not ours. */
    remote.buttons = NUVIO_BTN_UP;
    poll();
    remote_error = 1;
    assert(poll().released == NUVIO_BTN_UP && poll().held == 0);
    nuvio_input_open(7);
    assert(opens[0] == 1 && opens[1] == 1);
    nuvio_input_close();
    assert(closes[0] == 1 && closes[1] == 1);
    nuvio_input_close();
    assert(closes[0] == 1 && closes[1] == 1);

    /* Launch suppression is per source, in both directions. */
    for (int which = 0; which < 2; ++which) {
        reset(1, 1);
        pad_data *held = which ? &remote : &standard;
        pad_data *other = which ? &standard : &remote;
        held->buttons = NUVIO_BTN_CROSS;
        nuvio_input_open(7);
        assert(poll().pressed == 0 && poll().released == 0);
        other->buttons = NUVIO_BTN_CROSS;
        assert(poll().pressed == NUVIO_BTN_CROSS);
        other->buttons = held->buttons = 0;
        assert(poll().released == NUVIO_BTN_CROSS);
        held->buttons = NUVIO_BTN_CROSS;
        assert(poll().pressed == NUVIO_BTN_CROSS);
    }

    /* Remote is optional; it also works without a DualSense. */
    for (int have_standard = 0; have_standard < 2; ++have_standard) {
        reset(have_standard, !have_standard);
        nuvio_input_open(7);
        (have_standard ? &standard : &remote)->buttons = NUVIO_BTN_CIRCLE;
        assert(poll().pressed == NUVIO_BTN_CIRCLE);
        nuvio_input_set_lightbar(0xff1234);
        nuvio_input_reset_lightbar();
        nuvio_input_pulse(40, 20);
        nuvio_input_trigger_resistance(1);
        assert(have_standard || output_calls == 0);
        nuvio_input_close();
        assert(closes[0] == have_standard && closes[1] == !have_standard);
    }
    reset(0, 0);
    nuvio_input_open(7);
    assert(poll().held == 0);
    nuvio_input_inject(NUVIO_BTN_CROSS, 0);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    assert(poll().released == NUVIO_BTN_CROSS);
    nuvio_input_close();
    assert(closes[0] == 0 && closes[1] == 0);
    puts("PASS: controller/remote navigation, repeats, launch suppression, ownership and optional failure");
}
