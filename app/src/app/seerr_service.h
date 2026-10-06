/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Seerr for the Jellyfin account in use: its settings, its session and
 * whether it is there. Off unless turned on in Innstillinger: then nothing
 * is ever sent to Seerr and the app is as it was.
 *
 * Kept in /download0/jelly5/seerr.json: per Jellyfin server whether Seerr
 * is on and its address; per account how it signs in and its session
 * cookie (as accounts.json keeps the Jellyfin tokens); for the console,
 * whether it has Internet.
 *
 * Signing in needs nothing by default: Seerr starts a Quick Connect, the
 * Jellyfin account in use approves it, and Seerr opens that user's session
 * (Seerr 3.4 and later). The Jellyfin password or a local Seerr account
 * are the fallbacks. Everything runs on worker threads; the render thread
 * only reads snapshot().
 */
#pragma once

#include "jf/jf_client.h"
#include "seerr/seerr_client.h"

#include <memory>
#include <string>

namespace seerr_service {

enum class Auth { QuickConnect, JellyfinPassword, Local, Count };

enum class State {
    Off,            /* turned off, or no address */
    Connecting,
    Ready,          /* signed in */
    SignedOut,      /* Seerr answers, but has no session for us (automatic sign-in failed or off) */
    Unreachable,    /* no answer at its address (tried again every 30 s) */
};

/* Why it is SignedOut (what the settings say about it). */
enum class Why {
    None,
    NeedPassword,   /* the method is a password: the viewer types it */
    AutoFailed,     /* Quick Connect did not work (Seerr before 3.4, Quick Connect off ...) */
    NeedApproval,   /* Quick Connect not yet approved by the viewer for this address */
    WrongPassword,
    SignedOut,      /* the viewer signed out: nothing automatic until they sign in */
};

struct Config {
    bool enabled = false;           /* for this Jellyfin server */
    std::string url;
    Auth auth = Auth::QuickConnect; /* for this account */
};

struct Snapshot {
    State state = State::Off;
    std::string version;            /* Seerr's */
    seerr::User user;
    seerr::PublicSettings settings;
    Why why = Why::None;
    std::string error;              /* why it is not Ready, for the log */
    bool testing = false;
    std::string test;               /* the last connection test, in the interface's language */
};

/* After a Jellyfin sign-in: loads this account's settings and connects if Seerr is on. */
void attach(jf::Client *client);
/* The account is going away (switch, sign-out): its requests' results are dropped. */
void detach();

/* A title's status as this session knows it: a request or a withdrawal made
 * here moves it at once, before Seerr's lists are read again. it: a Seerr item
 * (jf::Item::external()). */
int status_of(const jf::Item &it);
/* After a request or a withdrawal here: the title's new status (seerr::Status),
 * shown everywhere at once; bumps changes(). */
void note_status(int tmdb_id, bool tv, int status);
/* Bumped by note_status: the lists that show statuses read them again. */
unsigned changes();

/* The titles Discover and search show, without those the administrator hides
 * (available, requested, blocklisted: Seerr's own pages' rules). */
std::vector<seerr::Title> visible(std::vector<seerr::Title> titles);

Config config();
/* Saves; connects again when the address, the sign-in or "on" changed. */
void set_config(const Config &c);
Snapshot snapshot();
/* Bumped on every change of snapshot(): the screens showing it redraw. */
unsigned generation();
bool ready();
/* Seerr's tab shows: on, and signed in once this session (it stays through a
 * reconnection rather than vanishing under the viewer). */
bool available();
/* The signed-in client (null unless Ready). Shared: a request still running
 * keeps its client when the settings change under it. */
std::shared_ptr<seerr::Client> client();

/* What the address field starts from: the Jellyfin server's host on Seerr's
 * port (a development build's SEERR_URL, when it has one). */
std::string suggested_url();

/* Connects again (and signs in by Quick Connect when that is the method). */
void reconnect();
/* The viewer approves Quick Connect for the Seerr address in the settings (the
 * account row): from then on, and only for that address, the console approves
 * Seerr's Quick Connect codes with this Jellyfin account by itself. */
void approve_quick_connect();
/* A request found the session gone (Seerr: 401/403): sign in again. */
void session_lost();
/* The Jellyfin password (user empty: the account's own name) or a local account. */
void sign_in(const std::string &user, const std::string &password);
void sign_out();
/* Checks the address, the session and the pictures; the line ends up in snapshot().test. */
void test();

/* The interface's language changed: names and overviews follow. */
void set_language();
/* TMDB's genre names, for to_item (blocking: call from a worker; kept per language). */
void load_genres();

/* A TMDB picture as the console may load it: through Seerr's image cache. */
std::string image_url(const std::string &path, const char *size);

/* A Seerr title as the app's screens draw it: a jf::Item with ext filled in
 * (its id "seerr:movie:<tmdb>" or "seerr:tv:<tmdb>", never a server's). */
jf::Item to_item(const seerr::Title &t);

} // namespace seerr_service
