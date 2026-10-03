/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings. Audio/subtitle languages, subtitle mode and autoplay live on the
 * Jellyfin account (so every client agrees); the console-side ones (quality
 * cap, automatic intro skipping, language, subtitle look) live in
 * /download0/jelly5/settings.json.
 */
#pragma once

#include "jf/jf_client.h"

namespace settings {

struct Local {
    int max_mbps = 0;            /* 0 = no cap (direct play whatever the network allows) */
    bool auto_skip_intro = false;
    int language = 0;            /* i18n::Choice: 0 follow the PS5, 1 Norsk, 2 English */
    /* How text subtitles look (set in Innstillinger or in the player). */
    int sub_size = 100;          /* % */
    float sub_offset = 0;        /* % of the height, lift from the bottom */
    float sub_background = 0;    /* 0..1 box behind the text */
    bool sub_outline = true;
    bool refresh_120 = true;
    int audio_delay_ms = 0;      /* the sound system's delay: the picture waits this long (A/V sync) */     /* the display at 120 Hz when it can (smoother menus, 24p without judder) */
};

struct All {
    Local local;
    jf::UserPrefs server;
};

/* The current settings (thread-safe copy). */
All get();
void load_local();
void set_local(const Local &l);
/* Reads the account's preferences (after sign-in). */
void load_server(jf::Client &c);
/* Changes the account's preferences (written in the background). */
void set_server(jf::Client &c, const jf::UserPrefs &p);

} // namespace settings
