/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Remote control ("Spill på PS5"): Jelly5 keeps Jellyfin's /socket open for
 * the signed-in session and tells the server it plays video and audio and
 * takes playstate commands. A phone (or the web client) can then send a
 * title to the PS5, pause, seek, skip and stop it, and show a message.
 * Commands queue here; the app takes them on its own thread (main loop, or
 * the player while something plays).
 */
#pragma once

#include "jf/jf_client.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace remote {

struct Command {
    enum Kind { Play, Pause, Unpause, PlayPause, Stop, Seek, Next, Previous, Rewind, FastForward, Message };
    Kind kind = Play;
    /* Play */
    std::vector<std::string> item_ids;
    int start_index = 0;
    int64_t start_ticks = 0;
    std::string play_command;          /* PlayNow, PlayShuffle, PlayInstantMix, PlayNext, PlayLast */
    /* Seek */
    int64_t seek_ticks = 0;
    /* Message */
    std::string header, text;
};

/* Starts listening for this client's session (the last session's listener
 * stops). alive: false once the session has changed. */
void start(jf::Client *client, std::function<bool()> alive);

/* The next command, if any (any thread). */
bool take(Command *out);
/* Puts a command back at the front (the player stops for a new Play, which
 * the main loop then takes). */
void put_back(const Command &c);

} // namespace remote
