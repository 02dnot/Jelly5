/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Settings. Audio/subtitle languages, subtitle mode and autoplay live on the
 * Jellyfin account (so every client agrees); the console-side ones (quality
 * cap, what to do at intros, credits and other segments, language, subtitle look) live in
 * /download0/jelly5/settings.json. The quality cap is kept per server (the
 * network to a server at home and one far away are not alike): max_mbps is
 * the current server's (use_server).
 */
#pragma once

#include "app/segments.h"
#include "jf/jf_client.h"

#include <functional>
#include <string>

namespace settings {

struct Local {
    int max_mbps = 0;            /* the current server's; 0 = no cap (direct play whatever the network allows) */
    std::string max_mbps_for;    /* the server max_mbps is (use_server); a copy for another is not saved as this one's */
    /* Per segment type (segments::Type): segments::Action. Older settings had only
     * "autoSkipIntro": on, the intro is skipped; off, asked about. */
    int segment[segments::TypeCount] = {segments::default_action(segments::Intro), segments::default_action(segments::Outro),
                                        segments::default_action(segments::Recap), segments::default_action(segments::Preview),
                                        segments::default_action(segments::Commercial)};
    int still_watching = 0;      /* "Spør om du fortsatt ser på": 0 off, 1 after 3 episodes, 2 after 2 hours */
    int language = 0;            /* i18n::Choice: 0 follow the PS5, 1 Norsk, 2 English */
    /* How text subtitles look (set in Innstillinger or in the player). */
    int sub_size = 100;          /* % */
    float sub_offset = 0;        /* % of the height, lift from the bottom */
    float sub_background = 0;    /* 0..1 box behind the text */
    bool sub_outline = true;
    bool refresh_120 = true;
    int upscale = 0;             /* EVO_AGC_UPSCALE_*: 0 off, 1 Skarp (FSR 1), 2 AI (Anime4K); video smaller than the screen, SDR only */
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
/* Reads the account's preferences (after sign-in); dropped unless current() still
 * says this account is the one in use. */
void load_server(jf::Client &c, const std::function<bool()> &current);
/* Changes the account's preferences (written in the background, once they were
 * read from this account: before that, on the console only). */
void set_server(jf::Client &c, const jf::UserPrefs &p);

} // namespace settings
