/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * SyncPlay ("Se sammen"): watching in step with others through Jellyfin.
 *
 * The server keeps a group with a play queue and tells every member what to
 * do and when: "unpause at 12:00:03.250 at 41:02". Jelly5 joins or creates a
 * group, plays what the group's queue says (pausing until everyone is ready),
 * carries out the group's commands at the agreed moment (the server's clock,
 * measured against ours), and sends its own pause, play and seek to the group
 * instead of doing them alone. Messages arrive on the remote control socket
 * (app/remote); requests are Jellyfin's /SyncPlay endpoints.
 */
#pragma once

#include "jf/jf_client.h"

#include <cstdint>
#include <string>
#include <vector>

struct cJSON;

namespace syncplay {

struct Group {
    std::string id, name, state;            /* state: Idle, Waiting, Paused, Playing */
    std::vector<std::string> participants;
};

/* The session's client (after sign-in); leaves any group of the last one. */
void attach(jf::Client *client);

bool active();                              /* in a group */
std::string group_name();

/* Off the render thread: they talk to the server. */
std::vector<Group> list();
bool create(const std::string &name);
bool join(const std::string &group_id);
void leave();
/* The session ends (another account, signed out): the group is left here at
 * once and the server told in the background, with the client of the time. */
void leave_async();
/* Out of the group here only (as leave_async), the server not told: whether there
 * was one. A sign-out sends the Leave itself, before its logout. */
bool forget_here();

/* In a group, Play asks the group to play this (everyone starts it). */
bool play(const jf::Item &item);

/* The player, while a group item plays. */
void player_started(double position_s, bool playing);       /* opened: Ready (paused) */
void player_stopped();                                      /* closed: the group stops waiting for it */
bool ready_asked();          /* the group set its queue anew around what plays: Ready again */
void request_pause(bool pause, double position_s);
void request_seek(double position_s);
void request_next();
bool queue_has_next();       /* the group's queue goes on after what plays now */
void seeked(double position_s);                             /* a group seek done: Ready */
void buffering(bool stalled, double position_s, bool playing);   /* stalled: Buffering; again: Ready */

/* From app/remote: a SyncPlayGroupUpdate or SyncPlayCommand message's Data. */
void on_group_update(const cJSON *data);
void on_command(const cJSON *data);

/* Seconds on the monotonic clock (now_s) when the server's UTC time is iso. */
double local_time_of(const std::string &iso);

} // namespace syncplay
