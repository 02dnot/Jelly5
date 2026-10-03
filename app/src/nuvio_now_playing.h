/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * What the headless player (music behind the menus) is playing, for the app
 * to draw: the request (title, artist, cover, queue) and the live status.
 * track changes with every new track.
 */
#pragma once

#include "nuvio_osd.h"

bool nuvio_player_now_playing(NuvioStatus *st, NuvioRequest *req, unsigned *track);
