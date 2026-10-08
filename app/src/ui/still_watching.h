/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * "Ser du fortsatt på?" (Innstillinger → Avspilling): after a run of episodes
 * that played on by themselves, the next one waits for the viewer. Falling
 * asleep should not play the whole season, mark it all watched, or keep the
 * server transcoding all night.
 *
 * What counts: episodes started by autoplay (the next-episode countdown or a
 * natural end) since the viewer last pressed anything in the player, and the
 * time played since that press. Any press starts both again. The chain carries
 * them from one episode's player to the next (autoplayCount, autoplayIdle).
 * Header-only and plain, so the rule can be tested on the host.
 */
#pragma once

namespace ui {

struct StillWatching {
    int limit_episodes = 0;     /* ask once this many have autoplayed in a row; 0 = not by count */
    double limit_seconds = 0;   /* ask once this long has played since the last press; 0 = not by time */
    int count = 0;              /* episodes started by autoplay since the last press */
    double idle = 0;            /* seconds played since the last press */

    /* The viewer pressed something: awake. */
    void press()
    {
        count = 0;
        idle = 0;
    }
    /* dt seconds played (not paused, not loading). */
    void played(double dt)
    {
        if (dt > 0)
            idle += dt;
    }
    /* At the moment the next episode would autoplay: ask instead? */
    bool ask() const
    {
        return (limit_episodes > 0 && count >= limit_episodes) || (limit_seconds > 0 && idle >= limit_seconds);
    }
    /* The next episode is starting by itself. */
    void autoplayed() { count++; }
};

} // namespace ui
