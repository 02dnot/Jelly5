/*
 * Jelly5 — host smoke test for the Jellyfin client against a real server.
 * Read-only apart from sign-in: lists the home rows and asks PlaybackInfo
 * (with the PS5 profile) how each "continue watching" item would play.
 *   tests/host/run.sh
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_client.h"

#include <cstdio>
#include <cstdlib>

int main()
{
    const char *server = std::getenv("JF_URL"), *user = std::getenv("JF_USER"), *pass = std::getenv("JF_PASS");
    if (!server || !user || !pass) {
        std::fprintf(stderr, "JF_URL, JF_USER and JF_PASS must be set\n");
        return 2;
    }
    jf::Client c(server, "jelly5-dev-mac", "Mac (Jelly5 dev)");
    std::string name, version;
    if (!c.public_info(&name, &version)) {
        std::fprintf(stderr, "server unreachable: %s\n", c.last_error().c_str());
        return 1;
    }
    std::printf("server: %s (Jellyfin %s)\n", name.c_str(), version.c_str());
    if (!c.authenticate(user, pass)) {
        std::fprintf(stderr, "sign-in failed: %s\n", c.last_error().c_str());
        return 1;
    }
    std::printf("signed in as %s\n\n", c.user_name().c_str());

    int direct = 0, total = 0;
    for (const jf::Item &it : c.resume(12)) {
        jf::Playback pb;
        const bool ok = c.playback_info(it.id, it.position_ticks, -1, -2, &pb);
        const jf::MediaStream *v = nullptr, *a = nullptr;
        for (const auto &s : pb.streams) {
            if (!v && s.type == "Video") v = &s;
            if (!a && s.type == "Audio" && s.index == pb.default_audio) a = &s;
        }
        total++;
        if (ok && pb.play_method == "DirectPlay") direct++;
        std::printf("%-44.44s %-5s %-9s %-6s %4dx%-4d %-8s %-6s %dch  %s %s\n",
                    (it.series_name.empty() ? it.name : it.series_name + " - " + it.name).c_str(),
                    pb.container.c_str(), v ? v->codec.c_str() : "-", v ? v->video_range_type.c_str() : "-",
                    v ? v->width : 0, v ? v->height : 0, a ? a->codec.c_str() : "-",
                    a ? a->language.c_str() : "", a ? a->channels : 0,
                    ok ? pb.play_method.c_str() : "ERROR", ok ? pb.transcode_reasons.c_str() : c.last_error().c_str());
    }
    std::printf("\n%d of %d would play directly on the PS5\n", direct, total);
    auto segs = c.next_up(1);
    if (!segs.empty()) {
        auto s = c.segments(segs[0].id);
        std::printf("segments for %s: %zu\n", segs[0].name.c_str(), s.size());
    }
    return 0;
}
