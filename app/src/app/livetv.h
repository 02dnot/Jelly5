/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Live TV ("Direkte-TV"): the server's channels and guide, shared by the Live TV
 * tab (ui/livetv), the home screen's "Direkte nå" and the player (zapping, the
 * channel list over the picture). The server does the TV: its tuners (M3U/IPTV,
 * HDHomeRun), its guide data (XMLTV, Schedules Direct) and its recordings; this
 * keeps what it said and loads more in the background, never on a frame.
 *
 * The guide is a snapshot (GuideRef) that is replaced, never changed, so a screen
 * holds one for as long as it draws it. Works for Jellyfin and Emby alike (Emby
 * shows channels only with Premiere).
 */
#pragma once

#include "jf/jf_client.h"

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace livetv {

struct Guide {
    std::vector<jf::Item> channels;                            /* the server's order, favourites first */
    /* By channel id, by start. A list is never changed once published: snapshots share them. */
    std::map<std::string, std::shared_ptr<const std::vector<jf::Item>>> programs;
    int64_t from = 0, to = 0;                                  /* the programmes cover [from, to) */
    std::vector<jf::Item> recordings;                          /* made, newest first */
    std::vector<jf::Item> timers;                              /* set for later, soonest first */
    bool loaded = false;                                       /* the channels came (an empty list too) */
    bool failed = false;                                       /* the last load got no answer */
    const jf::Item *channel(const std::string &id) const;
    const std::vector<jf::Item> *programs_of(const std::string &channel_id) const;   /* null: none */
    /* What airs on a channel at t, and what comes after it (null: not known). */
    const jf::Item *on_at(const std::string &channel_id, int64_t t) const;
    const jf::Item *after(const std::string &channel_id, int64_t t) const;
};
using GuideRef = std::shared_ptr<const Guide>;

/* Unix seconds now (UTC), and the start of the half hour it is in. */
int64_t now();
int64_t half_hour(int64_t t);

/* A new session (another account): everything of the last one is forgotten. */
void attach(jf::Client *c);
/* Asks the server, once per session, whether this user has Live TV (blocking:
 * call it off the render thread). Later calls answer from what it said. */
bool check();
/* What check() found (false until it has asked). */
bool available();

/* The guide as last loaded (never null; empty before the first load). Its version
 * changes with every new snapshot. */
GuideRef guide();
unsigned version();
bool loading();
/* Loads the channels and the next hours in the background, when what is held is
 * older than a few minutes (or force). */
void refresh(bool force = false);
/* The guide was scrolled on: programmes up to `to` (in the background). */
void extend(int64_t to);

/* Favourite channels: shown at once, written to the server behind. */
void set_favorite(const std::string &channel_id, bool on);

/* Recording a programme (series: every episode of it on that channel), or
 * cancelling one. done(ok) runs on a worker, after the guide shows the change. */
void record(const jf::Item &program, bool series, std::function<void(bool)> done);
void cancel(const jf::Item &program, bool series, std::function<void(bool)> done);

/* The channels watched, for "previous channel" and where the guide opens: this
 * user's, kept on the console. */
void watched(const std::string &channel_id);
std::string last_channel();
std::string previous_channel();
/* The file it keeps them in (end_account removes it with the user's others). */
std::string history_file(const std::string &user_id);

/* A channel's logo (its Primary image) at a width; empty when it has none. */
std::string logo_url(const jf::Item &channel, int width);
/* A programme's (or recording's) picture: its own, else its series', else none. */
std::string picture_url(const jf::Item &program, int width);

/* "21:30" (local time). */
std::string clock(int64_t utc);
/* "I dag", "I morgen", "fre. 9. okt." for the day of t (local time). */
std::string day_label(int64_t utc);
/* The programme's kind in a word ("Film", "Sport" ...), empty when it has none. */
std::string kind_label(const jf::Item &program);
/* Its colour (a thin mark on its cell), 0 when it has none. */
uint32_t kind_color(const jf::Item &program);

} // namespace livetv
