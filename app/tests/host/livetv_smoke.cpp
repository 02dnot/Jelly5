/*
 * Jelly5 — Jellyfin for PS5
 * Host test of Live TV against a server with channels (tests/host/livetv.sh):
 * the channels and what is on now, the guide for the next hours, and how each
 * channel would play with the PS5 profile. Every live stream it opens is closed
 * again (the stopped report carries its LiveStreamId), and the server is asked
 * afterwards that none is left open. --record also sets and cancels a recording.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_client.h"
#include "jf_http.h"

extern "C" {
#include "cJSON.h"
}

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <set>
#include <sys/time.h>

static int s_failed = 0;

static void check(bool ok, const char *what, const std::string &detail = std::string())
{
    std::printf("  %-4s %s%s%s\n", ok ? "ok" : "FAIL", what, detail.empty() ? "" : ": ", detail.c_str());
    if (!ok)
        s_failed++;
}

static std::string hm(int64_t utc)
{
    time_t t = (time_t)utc;
    struct tm lt;
    localtime_r(&t, &lt);
    char b[8];
    std::strftime(b, sizeof b, "%H:%M", &lt);
    return b;
}

/* Sessions of this device that still hold a live stream (NowPlayingItem a channel). */
static int open_live_sessions(jf::Client &c)
{
    std::string body;
    if (!c.get_json("/Sessions?DeviceId=" + jf::Client::escape(c.device_id()), &body))
        return -1;
    int n = 0;
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *s;
    cJSON_ArrayForEach(s, j) {
        const cJSON *np = cJSON_GetObjectItemCaseSensitive(s, "NowPlayingItem");
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(np, "Type");
        if (cJSON_IsString(t) && std::strcmp(t->valuestring, "TvChannel") == 0)
            n++;
    }
    cJSON_Delete(j);
    return n;
}

static double now_s()
{
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return tv.tv_sec + tv.tv_usec / 1e6;
}

/* Reads the stream as the PS5 would (ffprobe, which needs it to start): what it carries,
 * and how long the server took to start sending it. */
static std::string probe(const std::string &url, double *secs)
{
    const double t0 = now_s();
    const std::string cmd = "ffprobe -v error -rw_timeout 25000000 -analyzeduration 1500000 -show_entries "
                            "stream=codec_name,width,height,field_order -of csv=p=0 '" + url + "' 2>/dev/null";   /* (joining a stream
                            * mid-picture warns: only the result counts; an error names the URL and its token) */
    std::string out;
    if (FILE *f = popen(cmd.c_str(), "r")) {
        char line[256];
        while (std::fgets(line, sizeof line, f)) {
            std::string l = line;
            while (!l.empty() && (l.back() == '\n' || l.back() == ','))
                l.pop_back();
            out += (out.empty() ? "" : " ") + l;
        }
        pclose(f);
    }
    *secs = now_s() - t0;
    if (out.empty())
        out = "(no stream: the server answered with an error)";
    return out;
}

static void dates()
{
    std::printf("Dates (no server)\n");
    check(jf::utc_of("1970-01-01T00:00:00Z") == 0, "epoch");
    check(jf::utc_of("2026-10-08T22:15:00.0000000Z") == 1791497700, "Jellyfin's date");
    check(jf::utc_of("2024-02-29T12:00:00Z") == 1709208000, "leap day");
    check(jf::iso_of(1791497700) == "2026-10-08T22:15:00Z", "back as a query value", jf::iso_of(1791497700));
    check(jf::utc_of("") == 0 && jf::utc_of("soon") == 0, "no date");
    std::printf("\n");
}

int main(int argc, char **argv)
{
    dates();
    bool record = false, probing = false;
    for (int i = 1; i < argc; i++) {
        record = record || std::strcmp(argv[i], "--record") == 0;
        probing = probing || std::strcmp(argv[i], "--probe") == 0;
    }
    const char *server = std::getenv("JF_URL"), *user = std::getenv("JF_USER"), *pass = std::getenv("JF_PASS");
    if (!server || !user || !pass) {
        std::printf("JF_URL, JF_USER, JF_PASS needed\n");
        return 2;
    }
    jf::Client c(server, "jelly5-livetv-test", "Jelly5 Live TV test");
    std::string name, version;
    jf::Kind kind = jf::Kind::Jellyfin;
    if (!c.public_info(&name, &version, nullptr, &kind)) {
        std::printf("server not reachable: %s\n", c.last_error().c_str());
        return 2;
    }
    c.set_kind(kind);
    std::printf("%s %s (%s)\n", name.c_str(), version.c_str(), jf::kind_key(kind));
    if (!c.authenticate(user, pass) || !c.validate()) {
        std::printf("sign-in failed: %s\n", c.last_error().c_str());
        return 2;
    }

    std::printf("Live TV\n");
    const bool available = c.live_tv_available();
    if (!available && kind == jf::Kind::Emby) {   /* Emby lists no channels without Premiere */
        std::printf("  --   not available (Emby shows Live TV only with Premiere): the tab stays hidden\n");
        return s_failed ? 1 : 0;
    }
    check(available, "available to this user", available ? "" : c.last_error());
    if (!available)
        return s_failed ? 1 : 0;
    std::printf("  can record: %s\n", c.can_record() ? "yes" : "no");

    jf::Page page = c.channels(0, 200, true);
    check(page.ok && !page.items.empty(), "channels", std::to_string(page.items.size()) + " of " +
                                                          std::to_string(page.total));
    int with_now = 0, with_logo = 0;
    std::vector<std::string> ids;
    for (const jf::Item &ch : page.items) {
        ids.push_back(ch.id);
        with_logo += !ch.primary_tag.empty();
        const jf::Item *p = ch.now_on();
        if (p)
            with_now++;
        std::printf("    %3s %-16s %s\n", ch.channel_number.c_str(), ch.name.c_str(),
                    p ? (hm(p->start_utc) + "-" + hm(p->end_utc) + " " + p->name).c_str() : "-");
        if (p)
            check(p->start_utc > 0 && p->end_utc > p->start_utc && p->channel_id == ch.id, "  its programme's times");
    }
    check(with_now > 0, "what is on now", std::to_string(with_now));
    check(with_logo > 0, "channel logos", std::to_string(with_logo));
    check(page.items.front().type == "TvChannel", "type TvChannel", page.items.front().type);

    const int64_t now = (int64_t)time(nullptr);
    const int64_t from = now - now % 1800, to = from + 4 * 3600;
    std::vector<jf::Item> progs;
    check(c.programs(ids, from, to, &progs) && !progs.empty(), "guide, 4 hours", std::to_string(progs.size()) + " programmes");
    std::set<std::string> chans;
    int outside = 0, flags = 0, images = 0;
    const jf::Item *future = nullptr;
    for (const jf::Item &p : progs) {
        chans.insert(p.channel_id);
        if (p.end_utc <= from || p.start_utc >= to)
            outside++;
        flags += p.is_movie || p.is_sports || p.is_news || p.is_kids || p.is_series;
        images += !p.primary_tag.empty();
        if (!future && p.start_utc > now + 600)
            future = &p;
    }
    check(outside == 0, "every programme inside the window");
    check(chans.size() >= page.items.size() - 1, "programmes for every channel",
          std::to_string(chans.size()) + " channels");
    check(flags > 0, "categories (movie, sports, news, kids, series)", std::to_string(flags));
    std::printf("  programme images: %d of %zu\n", images, progs.size());
    if (future) {
        jf::Item p;
        check(c.program(future->id, &p) && p.id == future->id && p.start_utc == future->start_utc,
              "one programme", p.name);
    }

    std::printf("Playback (PS5 profile)\n");
    for (const jf::Item &ch : page.items) {
        jf::Playback pb;
        if (!c.playback_info(ch.id, 0, -1, -2, &pb, 0, std::string(), true)) {
            std::printf("    %3s %-16s no way to play: %s\n", ch.channel_number.c_str(), ch.name.c_str(),
                        c.last_error().c_str());
            continue;
        }
        std::string video;
        for (const jf::MediaStream &m : pb.streams)
            if (m.type == "Video" || m.type == "Audio")
                video += (video.empty() ? "" : "+") + m.codec + (m.height ? std::to_string(m.height) : "");
        std::printf("    %3s %-16s %-12s live=%d id=%s %s %s\n", ch.channel_number.c_str(), ch.name.c_str(),
                    pb.play_method.c_str(), pb.live, pb.live_stream_id.empty() ? "-" : "yes", video.c_str(),
                    pb.transcode_reasons.c_str());
        check(pb.live && !pb.live_stream_id.empty(), "  live, with its stream id");
        /* The engine knows a live URL by its stream id (evo_stream_io: reconnect at EOF, a short probe). */
        check(pb.url.find("iveStreamId=") != std::string::npos, "  its URL names the live stream",
              pb.url.substr(c.server().size(), pb.url.find('?') - c.server().size()));
        c.report_start(pb, 0);
        if (probing) {
            double secs = 0;
            const std::string got = probe(pb.url, &secs);
            const bool video = got.rfind("h264", 0) == 0 || got.rfind("hevc", 0) == 0 || got.rfind("mpeg2video", 0) == 0;
            char d[64];
            std::snprintf(d, sizeof d, "%.1f s: ", secs);
            if (ch.name.find("Offline") != std::string::npos)   /* (the test list's dead channel) */
                check(!video, "  a dead channel gives no picture", std::string(d) + got.substr(0, 120));
            else
                check(video, "  the stream plays", std::string(d) + got.substr(0, 120));
        }
        c.report_stopped(pb, 0);
        c.stop_encoding(pb);
    }
    const int left = open_live_sessions(c);
    check(left == 0, "no live stream left open", std::to_string(left));

    std::vector<jf::Item> recs = c.recordings(20);
    std::printf("Recordings: %zu\n", recs.size());
    for (const jf::Item &r : recs) {
        jf::Playback pb;
        const bool ok = c.playback_info(r.id, 0, -1, -2, &pb);
        std::printf("    %-10s %-24s %s-%s %-12s live=%d\n", r.type.c_str(), r.name.c_str(), hm(r.start_utc).c_str(),
                    hm(r.end_utc).c_str(), ok ? pb.play_method.c_str() : "-", pb.live);
        check(ok, "  it plays", ok ? "" : c.last_error());
        if (ok && probing) {
            double secs = 0;
            const std::string got = probe(pb.url, &secs);
            check(got.rfind("h264", 0) == 0 || got.rfind("hevc", 0) == 0, "  its stream", got.substr(0, 80));
        }
        if (ok) {   /* (no playback reports: they would mark it watched) */
            c.close_live_stream(pb);
            c.stop_encoding(pb);
        }
    }
    std::vector<jf::Item> timers = c.timers();
    std::printf("Timers: %zu\n", timers.size());
    for (const jf::Item &t : timers)
        std::printf("    %-24s %s %s-%s %s%s\n", t.name.c_str(), t.channel_name.c_str(), hm(t.start_utc).c_str(),
                    hm(t.end_utc).c_str(), t.timer_status.c_str(), t.series_timer_id.empty() ? "" : " (series)");

    if (record && future && c.can_record()) {
        std::printf("Recording (writes)\n");
        check(c.record(future->id, false), "record one programme", c.last_error());
        jf::Item p;
        check(c.program(future->id, &p) && !p.timer_id.empty(), "it has a timer", p.timer_id);
        check(!p.timer_id.empty() && c.cancel_timer(p.timer_id), "cancel it");
        check(c.program(future->id, &p) && p.timer_id.empty(), "the timer is gone");
        if (p.is_series) {
            check(c.record(future->id, true), "record the series", c.last_error());
            check(c.program(future->id, &p) && !p.series_timer_id.empty(), "it has a series timer");
            check(!p.series_timer_id.empty() && c.cancel_series_timer(p.series_timer_id), "cancel the series");
        }
    }

    std::printf("\n%s\n", s_failed ? "FAILED" : "all ok");
    return s_failed ? 1 : 0;
}
