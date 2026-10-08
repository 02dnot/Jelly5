/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Media segments (intro, credits, recap, preview, commercial): what the viewer
 * chose for each type (Innstillinger → Avspilling) and the rules the player
 * follows, as jellyfin-androidtv does (MediaSegmentRepository):
 * - an automatic skip only for a segment of at least 1 s, and only when
 *   playback runs into it (never when the viewer scrubbed or seeked into it);
 * - "Spør" only for a segment of at least 3 s; the button stays until 1 s
 *   before the segment's end and hides 8 s after it came up.
 * Pure (no app state), so the rules are tested on the host.
 */
#pragma once

#include <string>

namespace segments {

enum Type { Intro, Outro, Recap, Preview, Commercial, TypeCount };   /* Outro: the credits */
enum Action { Skip, Ask, Nothing, ActionCount };

constexpr double kSkipMin = 1.0;    /* Android TV's SkipMinDuration */
constexpr double kAskMin = 3.0;     /* AskToSkipMinDuration */
constexpr double kAskHide = 8.0;    /* AskToSkipAutoHideDuration */
constexpr double kEndMargin = 1.0;  /* the button goes 1 s before the end (SkipOverlayView) */
constexpr double kMaxStep = 3.0;    /* more between two ticks than this is a jump (a seek), not playback */
constexpr double kLandSlack = 1.0;  /* where the app's own skip lands: "played into" within this */

/* The defaults: ask, except commercials (skipped). */
inline Action default_action(int t) { return t == Commercial ? Skip : Ask; }

/* From settings without "segments": the old switch "autoSkipIntro" skipped every
 * segment that had a button then (intro, recap, preview). */
inline Action migrated_action(int t, bool auto_skip_intro)
{
    if (auto_skip_intro && (t == Intro || t == Recap || t == Preview))
        return Skip;
    return default_action(t);
}

/* Its settings.json / request key, and back (Jellyfin's type names, any case;
 * the player's "credits" is the credits too). -1: a type the app has no choice for. */
inline const char *key_of(int t)
{
    static const char *const k[] = {"intro", "outro", "recap", "preview", "commercial"};
    return t >= 0 && t < TypeCount ? k[t] : "";
}
inline int type_of(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c = (char)(c - 'A' + 'a');
    if (s == "credits")
        return Outro;
    for (int t = 0; t < TypeCount; t++)
        if (s == key_of(t))
            return t;
    return -1;
}
inline const char *action_key(int a) { return a == Skip ? "skip" : a == Nothing ? "nothing" : "ask"; }
inline int action_of(const std::string &s, int fallback)
{
    return s == "skip" ? Skip : s == "ask" ? Ask : s == "nothing" ? Nothing : fallback;
}

/* What a segment of this length gets (getMediaSegmentAction): too short to skip
 * or to ask about, nothing. */
inline Action effective(Action a, double len)
{
    if (a == Skip && len < kSkipMin)
        return Nothing;
    if (a == Ask && len < kAskMin)
        return Nothing;
    return a;
}

/* Playback ran into a segment starting at `start` between two ticks (prev, pos):
 * it crossed the start moving forward by no more than a tick's worth. */
inline bool entered_naturally(double prev, double pos, double start)
{
    return prev < start && pos >= start && pos - prev <= kMaxStep;
}

/* The app's own skip landed at `target` and the position is there: a segment that
 * starts where it landed (a recap right before the intro, commercials back to back)
 * counts as played into, though the position jumped. */
inline bool landed_into(double target, double pos, double start)
{
    return target >= 0 && pos >= target - kLandSlack && pos <= target + kLandSlack && start >= target - kLandSlack &&
           start <= pos;
}

/* The first tick of a playback that starts at the beginning: the previous
 * position counts as before 0 (a segment at 0 is played into). The engine can
 * report its first position a little late. */
inline double first_prev(double pos, double start_position)
{
    return pos < kMaxStep && start_position < 1.0 ? -0.001 : pos;
}

/* Skipped now (Skip), a button (Ask), or nothing, for a segment the position is
 * in: `natural` when this tick ran into it. A segment to skip that the viewer
 * seeked into is not skipped; it offers the button instead (when long enough). */
inline Action decide(Action chosen, double len, bool natural)
{
    const Action a = effective(chosen, len);
    if (a == Skip)
        return natural ? Skip : effective(Ask, len);
    return a;
}

} // namespace segments
