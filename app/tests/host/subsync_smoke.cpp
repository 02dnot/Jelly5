/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Host test of subtitle auto-sync against a test server (reads only): the
 * film "Synktest" (speech-like bursts; an embedded track 1.7 s late, an
 * external .srt 3.2 s early, see subsync.sh) is synced the way the console
 * does it: jf::Client's audio-only URL per window and the server's file of
 * the embedded track, then the file itself for comparison.
 */
#include "jf_client.h"
#include "jf_http.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

extern "C" {
#include "evo_subsync.h"
int glue_run(const char *media_path, const char *audio_url, const char *cue_url, double duration_s,
             int sub_stream, const double *cs, const double *ce, int n, evo_subsync_result_t *res);
int glue_cues(const char *url, double **cs, double **ce);
}

static int s_failed = 0;

static void expect(const char *what, const evo_subsync_result_t &r, double want)
{
    const bool ok = r.status == EVO_SUBSYNC_OK && std::fabs(r.delay_s - want) < 0.1 && std::fabs(r.scale - 1.0) < 1e-6;
    std::printf("  %-4s %s: status %d, %+.2f s (want %+.2f), scale %.5f, %.1f s\n", ok ? "ok" : "FAIL", what,
                r.status, r.delay_s, want, r.scale, r.elapsed_s);
    if (!ok)
        s_failed++;
}

int main()
{
    const char *server = std::getenv("SYNC_URL"), *user = std::getenv("SYNC_USER"), *pass = std::getenv("SYNC_PASS");
    if (!server || !user || !pass)
        return 2;
    jf::Client c(server, "jelly5-dev-mac", "Mac (Jelly5 dev)");
    std::string name, version;
    jf::Kind kind = jf::Kind::Jellyfin;
    if (!c.public_info(&name, &version, nullptr, &kind))
        return 1;
    c.set_kind(kind);
    if (!c.authenticate(user, pass))
        return 1;
    std::printf("server: %s %s\n", jf::kind_key(kind), version.c_str());
    const std::vector<jf::Item> found = c.search("Synktest", "Movie", 5);
    if (found.empty()) {
        std::printf("  FAIL no \"Synktest\" on this server\n");
        return 1;
    }
    const std::string id = found[0].id;
    jf::Playback pb;
    if (!c.playback_info(id, 0, -1, -2, &pb) || pb.versions.empty() || pb.play_method != "DirectPlay") {
        std::printf("  FAIL PlaybackInfo: %s\n", c.last_error().c_str());
        return 1;
    }
    const jf::Version &v = pb.versions[0];
    std::string audio_url, cue_url, ext_url;
    int embedded = -1;
    double duration = 0;
    for (const jf::MediaStream &m : v.streams) {
        if (m.type == "Audio" && audio_url.empty())
            audio_url = c.sync_audio_url(pb.item_id, v, m);
        if (m.type == "Subtitle" && !m.is_external) {
            cue_url = c.subtitle_file_url(pb.item_id, v, m);
            embedded = jf::Client::container_index(v, m);
        }
        if (m.type == "Subtitle" && m.is_external)
            ext_url = m.delivery_url;
    }
    duration = found[0].runtime_ticks / 1e7;
    std::printf("  audio URL %s, subtitle file %s (stream %d), external %s, %.0f s\n", audio_url.empty() ? "none" : "ok",
                cue_url.empty() ? "none" : "ok", embedded, ext_url.empty() ? "none" : "ok", duration);
    if (audio_url.empty() || cue_url.empty() || ext_url.empty() || embedded != 2) {
        std::printf("  FAIL the server's sources\n");
        return 1;
    }

    double *cs = nullptr, *ce = nullptr;
    const int n = glue_cues(ext_url.c_str(), &cs, &ce);
    std::printf("  external track: %d cues\n", n);
    evo_subsync_result_t r;
    glue_run(pb.url.c_str(), audio_url.c_str(), "", duration, -1, cs, ce, n, &r);
    expect("external, audio from the server", r, 3.2);
    glue_run(pb.url.c_str(), audio_url.c_str(), cue_url.c_str(), duration, embedded, nullptr, nullptr, 0, &r);
    expect("embedded, audio and cues from the server", r, -1.7);
    glue_run(pb.url.c_str(), "", "", 0, -1, cs, ce, n, &r);
    expect("external, the file itself (fallback)", r, 3.2);
    std::free(cs);
    std::free(ce);
    std::printf("%s\n", s_failed ? "FAILED" : "all ok");
    return s_failed ? 1 : 0;
}
