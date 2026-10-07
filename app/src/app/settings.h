/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings. Audio/subtitle languages, subtitle mode and autoplay live on the
 * Jellyfin account (so every client agrees); the console-side ones (quality
 * cap, automatic intro skipping, language, subtitle look) live in
 * /download0/jelly5/settings.json. The quality cap is kept per server (the
 * network to a server at home and one far away are not alike): max_mbps is
 * the current server's (use_server).
 */
#pragma once

#include "jf/jf_client.h"

#include <string>

namespace settings {

struct Local {
    int max_mbps = 0;            /* the current server's; 0 = no cap (direct play whatever the network allows) */
    std::string max_mbps_for;    /* the server max_mbps is (use_server); a copy for another is not saved as this one's */
    bool auto_skip_intro = false;
    int language = 0;            /* i18n::Choice: 0 follow the PS5, 1 Norsk, 2 English */
    /* How text subtitles look (set in Innstillinger or in the player). */
    int sub_size = 100;          /* % */
    float sub_offset = 0;        /* % of the height, lift from the bottom */
    float sub_background = 0;    /* 0..1 box behind the text */
    bool sub_outline = true;
    bool refresh_120 = true;
    int audio_delay_ms = 0;      /* the sound system's delay: the picture waits this long (A/V sync) */
    bool night_mode = false;     /* compress loud and quiet together, dialogue lifted */
    bool hdmi_bitstream = false; /* Dolby Digital (Plus) and DTS to the TV/receiver undecoded (jelly5_bitstream) */
    bool theme_music = true;     /* a title's theme song, quietly, on its page */
    bool check_updates = false;  /* opt-in: ask GitHub for a newer release at start */     /* the display at 120 Hz when it can (smoother menus, 24p without judder) */
};

struct All {
    Local local;
    jf::UserPrefs server;
};

/* The current settings (thread-safe copy). */
All get();
void load_local();
/* Saves the console's settings; max_mbps for the current server. */
void set_local(const Local &l);
/* The server signed in to (its Id, else its address): its quality cap from now on. */
void use_server(const std::string &key);
/* Reads the account's preferences (after sign-in). */
void load_server(jf::Client &c);
/* Changes the account's preferences (written in the background). */
void set_server(jf::Client &c, const jf::UserPrefs &p);

} // namespace settings
