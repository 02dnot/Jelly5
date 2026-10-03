/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
/*
 * Controller input for the Nuvio Player: the DualSense through scePad, plus
 * buttons injected by the payload's command channel ("key" commands), merged
 * into one stream of presses with auto-repeat for held directions.
 *
 * The pad is opened only while a stream plays: while the app holds it, the
 * browser dialog that shows Nuvio's page gets no input.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* scePad button bits. */
enum {
    NUVIO_BTN_L3       = 0x00000002,
    NUVIO_BTN_R3       = 0x00000004,
    NUVIO_BTN_OPTIONS  = 0x00000008,
    NUVIO_BTN_UP       = 0x00000010,
    NUVIO_BTN_RIGHT    = 0x00000020,
    NUVIO_BTN_DOWN     = 0x00000040,
    NUVIO_BTN_LEFT     = 0x00000080,
    NUVIO_BTN_L2       = 0x00000100,
    NUVIO_BTN_R2       = 0x00000200,
    NUVIO_BTN_L1       = 0x00000400,
    NUVIO_BTN_R1       = 0x00000800,
    NUVIO_BTN_TRIANGLE = 0x00001000,
    NUVIO_BTN_CIRCLE   = 0x00002000,
    NUVIO_BTN_CROSS    = 0x00004000,
    NUVIO_BTN_SQUARE   = 0x00008000,
    NUVIO_BTN_TOUCHPAD = 0x00100000,

    NUVIO_BTN_DPAD = NUVIO_BTN_UP | NUVIO_BTN_RIGHT | NUVIO_BTN_DOWN | NUVIO_BTN_LEFT,
};

typedef struct nuvio_input_state {
    uint32_t pressed;    /* went down this poll (D-pad: also auto-repeats) */
    uint32_t released;   /* went up this poll */
    uint32_t held;       /* down now */
    uint32_t repeats;    /* the subset of pressed that is an auto-repeat */
    double   held_for;   /* seconds the current D-pad direction has been held */
    float    l2, r2;     /* Jelly5: how far L2 and R2 are pressed, 0..1 */
} nuvio_input_state;

/* Opens the user's controller. Presses already down are ignored until they
 * are released, so the button that started playback does not act twice. */
void nuvio_input_open(int user_id);
/* Jelly5: the DualSense's adaptive triggers on L2 and R2: on, a resistance that
 * stiffens the further they are pressed (scrubbing); off, free again. */
void nuvio_input_trigger_resistance(int on);
void nuvio_input_close(void);
/* Jelly5: the DualSense light bar (0xRRGGBB); reset gives back the PS5 user's colour. */
void nuvio_input_set_lightbar(uint32_t rgb);
void nuvio_input_reset_lightbar(void);
/* Jelly5: a short rumble (strength 0..255 on both motors), stopped by the poll after
 * ms. EVO found vibration silent from the PS Now slot it ran in; an app of its own
 * (as SDL2's port) sets vibration mode 2 first - the open logs what the pad says. */
void nuvio_input_pulse(int strength, int ms);

/* A press from the command channel, held for hold_ms (0 = one tap). */
void nuvio_input_inject(uint32_t button, int hold_ms);

/* Button bit for a command-channel name ("cross", "left", ...), 0 if unknown. */
uint32_t nuvio_input_button_named(const char *name);

/* Reads the controller once; call every frame. */
void nuvio_input_poll(nuvio_input_state *out);

#ifdef __cplusplus
}
#endif
