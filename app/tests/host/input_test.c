/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Synthetic tests of the input (nuvio_input.c): the DualSense and the TV
 * remote on fake pads; no console or server needed. Run by input.sh.
 */
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
#include "../../src/nuvio_input.c"
#undef clock_gettime

enum { STANDARD_HANDLE = 10, REMOTE_HANDLE = 20 };
static pad_data standard, remote;
static int standard_available, remote_available, standard_error, remote_error;
static int opens[2], closes[2], output_calls;
static cec_sample queued[64];
static int queue_count;
static uint64_t queue_timestamp;
int scePadRead(int handle, void *out, int count)
{
    assert(handle == REMOTE_HANDLE && count == 64);
    if (remote_error) return -1;
    memcpy(out, queued, sizeof(cec_sample) * (size_t)queue_count);
    int result = queue_count;
    queue_count = 0;
    return result;
}
static void enqueue_at(uint32_t buttons, uint64_t timestamp)
{
    assert(queue_count < 64);
    cec_sample *sample = &queued[queue_count++];
    memset(sample, 0, sizeof *sample);
    memcpy(sample->data, &buttons, sizeof buttons);
    memcpy(sample->data + 0x50, &timestamp, sizeof timestamp);
}
static void enqueue(uint32_t buttons)
{
    enqueue_at(buttons, ++queue_timestamp);
}
static void set_remote_key(void *sample, unsigned code, unsigned length)
{
    unsigned char *bytes = sample;
    bytes[0x6b] = (unsigned char)length;
    bytes[0x6c] = (unsigned char)code;
}
static void enqueue_remote_key(uint32_t buttons, unsigned code, unsigned length)
{
    enqueue(buttons);
    set_remote_key(queued[queue_count - 1].data, code, length);
}

int scePadOpen(int user, int type, int index, void *param)
{
    assert(index == 0 && param == NULL);
    assert(user == (type == PAD_PORT_REMOTE_CONTROL ? 0xff : 7));
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
    queue_count = 0;
    queue_timestamp = 0;
    test_time = 1;
}
static nuvio_input_state poll(void)
{
    nuvio_input_state state;
    nuvio_input_poll(&state);
    return state;
}

static void queue_regressions(void)
{
    /* Reused timestamps must not hide a release followed by another press. */
    reset(1, 1);
    nuvio_input_open(7);
    enqueue_at(NUVIO_BTN_CROSS, 10);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    enqueue_at(0, 10);
    assert(poll().pressed == 0);
    enqueue_at(NUVIO_BTN_CROSS, 10);
    assert(poll().pressed == NUVIO_BTN_CROSS);

    /* Identical held samples, including an unknown timestamp, add no edge. */
    reset(1, 1);
    nuvio_input_open(7);
    enqueue_at(NUVIO_BTN_CROSS, 10);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    enqueue_at(NUVIO_BTN_CROSS, 10);
    assert(poll().pressed == 0);
    enqueue_at(NUVIO_BTN_CROSS, 0);
    assert(poll().pressed == 0);

    /* Zero does not make an older, already superseded sample current again. */
    reset(1, 1);
    nuvio_input_open(7);
    enqueue_at(NUVIO_BTN_CROSS, 100);
    enqueue_at(0, 101);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    enqueue_at(0, 0);
    assert(poll().pressed == 0);
    enqueue_at(NUVIO_BTN_CIRCLE, 99);
    enqueue_at(0, 100);
    assert(poll().pressed == 0);
    enqueue_at(NUVIO_BTN_CIRCLE, 102);
    assert(poll().pressed == NUVIO_BTN_CIRCLE);

    /* A release seen only by the snapshot rearms the next queued press. */
    reset(1, 1);
    nuvio_input_open(7);
    remote.buttons = NUVIO_BTN_CROSS;
    enqueue_at(NUVIO_BTN_CROSS, 10);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    remote.buttons = 0;
    assert(poll().released == NUVIO_BTN_CROSS);
    enqueue_at(NUVIO_BTN_CROSS, 11);
    assert(poll().pressed == NUVIO_BTN_CROSS);

    /* A constantly empty snapshot cannot release a queue-only held button. */
    reset(1, 1);
    nuvio_input_open(7);
    enqueue_at(NUVIO_BTN_CROSS, 10);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    enqueue_at(NUVIO_BTN_CROSS, 11);
    assert(poll().pressed == 0);
    enqueue_at(NUVIO_BTN_CROSS, 12);
    assert(poll().pressed == 0);
    enqueue_at(0, 13);
    assert(poll().pressed == 0);
    enqueue_at(NUVIO_BTN_CROSS, 14);
    assert(poll().pressed == NUVIO_BTN_CROSS);

    /* A remote release/repress between polls acts even if both snapshots hold X. */
    reset(1, 1);
    nuvio_input_open(7);
    remote.buttons = NUVIO_BTN_CROSS;
    enqueue_at(NUVIO_BTN_CROSS, 10);
    nuvio_input_state state = poll();
    assert(state.pressed == NUVIO_BTN_CROSS && state.held == NUVIO_BTN_CROSS);
    enqueue_at(0, 11);
    enqueue_at(NUVIO_BTN_CROSS, 12);
    state = poll();
    assert(state.pressed == NUVIO_BTN_CROSS && state.held == NUVIO_BTN_CROSS);
}

static void injection_queue_regressions(void)
{
    /* An active injected hold suppresses a duplicate remote tap of that button. */
    reset(1, 1);
    nuvio_input_open(7);
    nuvio_input_inject(NUVIO_BTN_CROSS, 1000);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    enqueue_at(NUVIO_BTN_CROSS, 10);
    enqueue_at(0, 11);
    nuvio_input_state state = poll();
    assert(state.pressed == 0 && state.held == NUVIO_BTN_CROSS);
    test_time = 2.1;
    assert(poll().released == NUVIO_BTN_CROSS);

    /* Expiration before this poll lets a fresh queued remote tap act. */
    reset(1, 1);
    nuvio_input_open(7);
    nuvio_input_inject(NUVIO_BTN_CROSS, 1000);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    test_time = 2.1;
    enqueue_at(NUVIO_BTN_CROSS, 10);
    enqueue_at(0, 11);
    state = poll();
    assert(state.pressed == NUVIO_BTN_CROSS && state.held == 0);

    /* An explicit injected tap still re-presses an already injected held X. */
    reset(1, 1);
    nuvio_input_open(7);
    nuvio_input_inject(NUVIO_BTN_CROSS, 1000);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    enqueue_at(NUVIO_BTN_CROSS, 10);
    enqueue_at(0, 11);
    nuvio_input_inject(NUVIO_BTN_CROSS, 0);
    state = poll();
    assert(state.pressed == NUVIO_BTN_CROSS && state.held == NUVIO_BTN_CROSS);
    test_time = 2.1;
    assert(poll().released == NUVIO_BTN_CROSS);
    nuvio_input_close();
}

static void remote_key_regressions(void)
{
    const struct { unsigned code; uint32_t button; } keys[] = {
        {13, NUVIO_BTN_CROSS}, {15, NUVIO_BTN_CIRCLE}
    };
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; ++i) {
        reset(1, 1);
        nuvio_input_open(7);
        set_remote_key(&remote, keys[i].code, 1);
        nuvio_input_state state = poll();
        assert(state.pressed == keys[i].button && state.held == keys[i].button);
        assert(poll().pressed == 0);
        test_time = 3;
        state = poll();
        assert(state.pressed == 0 && state.repeats == 0 && state.held == keys[i].button);
        set_remote_key(&remote, 0, 1);
        assert(poll().released == keys[i].button);

        /* A complete key pulse can happen between snapshots. */
        enqueue_remote_key(0, keys[i].code, 1);
        enqueue_remote_key(0, 0, 1);
        state = poll();
        assert(state.pressed == keys[i].button && state.held == 0);
        assert(poll().pressed == 0);

        /* Queue and snapshot representations share one held-button baseline. */
        enqueue_remote_key(0, keys[i].code, 1);
        set_remote_key(&remote, keys[i].code, 1);
        assert(poll().pressed == keys[i].button);
        enqueue_remote_key(0, keys[i].code, 1);
        assert(poll().pressed == 0);
        test_time = 5;
        assert(poll().pressed == 0);
        enqueue_remote_key(0, 0, 1);
        set_remote_key(&remote, 0, 1);
        assert(poll().released == keys[i].button);

        /* Launch-held suppression follows the decoded key, per device. */
        for (int held_remote = 0; held_remote < 2; ++held_remote) {
            reset(1, 1);
            if (held_remote)
                set_remote_key(&remote, keys[i].code, 1);
            else
                standard.buttons = keys[i].button;
            nuvio_input_open(7);
            if (held_remote)
                enqueue_remote_key(0, keys[i].code, 1);
            state = poll();
            assert(state.pressed == 0 && state.held == 0);
            if (held_remote)
                standard.buttons = keys[i].button;
            else {
                set_remote_key(&remote, keys[i].code, 1);
                enqueue_remote_key(0, keys[i].code, 1);
            }
            assert(poll().pressed == keys[i].button);
            standard.buttons = 0;
            set_remote_key(&remote, 0, 1);
            enqueue_remote_key(0, 0, 1);
            assert(poll().released == keys[i].button);
            if (held_remote)
                set_remote_key(&remote, keys[i].code, 1);
            else
                standard.buttons = keys[i].button;
            assert(poll().pressed == keys[i].button);
        }

        /* System-intercepted samples suppress aliases and regular button bits. */
        reset(1, 1);
        nuvio_input_open(7);
        remote.buttons = 0x80000000u | keys[i].button;
        set_remote_key(&remote, keys[i].code, 1);
        enqueue_remote_key(remote.buttons, keys[i].code, 1);
        state = poll();
        assert(state.pressed == 0 && state.held == 0);
    }

    /* SDL's remote ABI reads the key byte even when uniqueDataLen is zero. */
    reset(1, 1);
    nuvio_input_open(7);
    set_remote_key(&remote, 13, 0);
    enqueue_remote_key(0, 13, 0);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    assert(poll().pressed == 0);
    set_remote_key(&remote, 0, 0);
    enqueue_remote_key(0, 0, 0);
    assert(poll().released == NUVIO_BTN_CROSS);
    enqueue_remote_key(0, 13, 0);
    enqueue_remote_key(0, 0, 0);
    assert(poll().pressed == NUVIO_BTN_CROSS);

    /* A standard Cross bit and remote OK alias describe the same press. */
    reset(1, 1);
    nuvio_input_open(7);
    remote.buttons = NUVIO_BTN_CROSS;
    set_remote_key(&remote, 13, 1);
    enqueue_remote_key(NUVIO_BTN_CROSS, 13, 1);
    assert(poll().pressed == NUVIO_BTN_CROSS);
    enqueue_remote_key(NUVIO_BTN_CROSS, 13, 1);
    assert(poll().pressed == 0);
    remote.buttons = 0;
    set_remote_key(&remote, 0, 1);
    enqueue_remote_key(0, 0, 1);
    assert(poll().released == NUVIO_BTN_CROSS);

    /* Neither headers nor later payload bytes may masquerade as the key byte. */
    reset(1, 1);
    nuvio_input_open(7);
    unsigned char *bytes = (unsigned char *)&remote;
    for (unsigned offset = 0x58; offset < 0x78; ++offset)
        if (offset != 0x6c)
            bytes[offset] = (unsigned char)(offset % 2 ? 13 : 15);
    enqueue(0);
    memcpy(queued[queue_count - 1].data + 0x58, bytes + 0x58, 32);
    nuvio_input_state state = poll();
    assert(state.pressed == 0 && state.held == 0);
    set_remote_key(&remote, 14, 1);
    enqueue_remote_key(0, 14, 1);
    state = poll();
    assert(state.pressed == 0 && state.held == 0);

    /* Device-class key data is interpreted only on the remote-control port. */
    reset(1, 1);
    nuvio_input_open(7);
    set_remote_key(&standard, 13, 1);
    state = poll();
    assert(state.pressed == 0 && state.held == 0);

    /* A DualSense already holding Cross suppresses a second remote OK action. */
    reset(1, 1);
    nuvio_input_open(7);
    standard.buttons = NUVIO_BTN_CROSS;
    assert(poll().pressed == NUVIO_BTN_CROSS);
    set_remote_key(&remote, 13, 1);
    enqueue_remote_key(0, 13, 1);
    state = poll();
    assert(state.pressed == 0 && state.held == NUVIO_BTN_CROSS);
    set_remote_key(&remote, 0, 1);
    enqueue_remote_key(0, 0, 1);
    state = poll();
    assert(state.released == 0 && state.held == NUVIO_BTN_CROSS);
    standard.buttons = 0;
    assert(poll().released == NUVIO_BTN_CROSS);
    nuvio_input_close();
}


int main(void)
{
    queue_regressions();
    remote_key_regressions();
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
    injection_queue_regressions();
    puts("PASS: controller/remote navigation, repeats, launch suppression, ownership and optional failure");
}
