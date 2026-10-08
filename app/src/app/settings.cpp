/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/settings.h"
#include "app/i18n.h"
#include "app/spawn.h"
#include "jf/json_num.h"

#include <algorithm>

#include "evo_boot_trace.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sys/stat.h>
#include <thread>

extern "C" {
#include "cJSON.h"
}

namespace settings {
namespace {

constexpr const char *kFile = "/download0/jelly5/settings.json";
std::mutex s_lock;
All s_all;
/* Quality caps per server; a server without one gets the cap kept from before
 * they were per server (the old "maxMbps"), so an update changes nothing. */
std::map<std::string, int> s_quality;
int s_quality_default = 0;
std::string s_server;
/* Saving to the server reads the whole configuration and posts it back: one save
 * at a time, and only the latest change, so an older one cannot land last. */
std::mutex s_save_lock;
std::atomic<unsigned> s_save_gen{0};
/* The account s_all.server was read from (null: not read yet, or it failed).
 * Only its client writes them back: before that they are the defaults, not
 * the account's, and set_prefs would put all of them on its server. */
const jf::Client *s_prefs_of = nullptr;

} // namespace

All get()
{
    std::lock_guard<std::mutex> g(s_lock);
    return s_all;
}

void load_local()
{
    std::string body;
    if (FILE *f = std::fopen(kFile, "rb")) {
        char buf[1024];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            body.append(buf, n);
        std::fclose(f);
    }
    cJSON *j = cJSON_Parse(body.c_str());
    if (!j)
        return;
    std::lock_guard<std::mutex> g(s_lock);
    /* cJSON_GetNumberValue gives NaN for a missing key: to_int makes it 0. */
    s_quality_default =
        std::max(0, jf::to_int<int>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "maxMbps"))));
    s_quality.clear();
    const cJSON *q;
    cJSON_ArrayForEach(q, cJSON_GetObjectItemCaseSensitive(j, "maxMbpsByServer"))
        if (q->string && cJSON_IsNumber(q))
            s_quality[q->string] = std::max(0, jf::to_int<int>(q->valuedouble));
    auto at = s_quality.find(s_server);
    s_all.local.max_mbps = at != s_quality.end() ? at->second : s_quality_default;
    s_all.local.max_mbps_for = s_server;
    /* Each segment type's choice; missing (older settings): the default, or what
     * the old "autoSkipIntro" switch did (on: every segment with a skip button,
     * intro, recap and preview, was skipped). */
    {
        const cJSON *seg = cJSON_GetObjectItemCaseSensitive(j, "segments");
        const bool old_auto = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "autoSkipIntro"));
        for (int t = 0; t < segments::TypeCount; t++) {
            int a = segments::migrated_action(t, old_auto);
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(seg, segments::key_of(t));
            if (cJSON_IsString(v))
                a = segments::action_of(v->valuestring, a);
            s_all.local.segment[t] = a;
        }
    }
    /* Missing (older settings) or unknown: off. */
    s_all.local.still_watching =
        jf::to_int<int>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "stillWatching")));
    if (s_all.local.still_watching < 0 || s_all.local.still_watching > 2)
        s_all.local.still_watching = 0;
    s_all.local.language = jf::to_int<int>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "language")));
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "nightMode"))
        s_all.local.night_mode = cJSON_IsTrue(v);
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "hdmiBitstream"))
        s_all.local.hdmi_bitstream = cJSON_IsTrue(v);
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "themeMusic"))
        s_all.local.theme_music = cJSON_IsTrue(v);
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "checkUpdates"))
        s_all.local.check_updates = cJSON_IsTrue(v);
    const int delay = jf::to_int<int>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "audioDelayMs")));
    s_all.local.audio_delay_ms = std::max(-500, std::min(500, delay));
    s_all.local.upscale = jf::to_int<int>(cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "upscale")));
    if (s_all.local.upscale < 0 || s_all.local.upscale > 2)
        s_all.local.upscale = 0;
    if (const cJSON *hz = cJSON_GetObjectItemCaseSensitive(j, "refresh120"))
        s_all.local.refresh_120 = cJSON_IsTrue(hz);
    if (const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "subtitles")) {
        Local &l = s_all.local;
        const cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(st, "size")))
            l.sub_size = std::max(50, std::min(200, jf::to_int<int>(v->valuedouble)));
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(st, "offset")))
            l.sub_offset = (float)std::max(0.0, std::min(40.0, v->valuedouble));
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(st, "background")))
            l.sub_background = (float)std::max(0.0, std::min(1.0, v->valuedouble));
        if (cJSON_IsBool(v = cJSON_GetObjectItemCaseSensitive(st, "outline")))
            l.sub_outline = cJSON_IsTrue(v);
    }
    if (s_all.local.language < 0 || s_all.local.language >= i18n::ChoiceCount)
        s_all.local.language = 0;
    cJSON_Delete(j);
}

void set_local(const Local &l)
{
    std::map<std::string, int> quality;
    int quality_default;
    {
        std::lock_guard<std::mutex> g(s_lock);
        const int current = s_all.local.max_mbps;
        s_all.local = l;
        if (l.max_mbps_for != s_server) {   /* read for another server (it changed since): keep this one's */
            s_all.local.max_mbps = current;
            s_all.local.max_mbps_for = s_server;
        } else if (!s_server.empty()) {
            s_quality[s_server] = l.max_mbps;
        } else {
            s_quality_default = l.max_mbps;
        }
        quality = s_quality;
        quality_default = s_quality_default;
    }
    mkdir("/download0/jelly5", 0777);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "maxMbps", quality_default);
    cJSON *by = cJSON_CreateObject();
    for (const auto &kv : quality)
        cJSON_AddNumberToObject(by, kv.first.c_str(), kv.second);
    cJSON_AddItemToObject(j, "maxMbpsByServer", by);
    /* "autoSkipIntro" too, for an older Jelly5 reading this file. */
    cJSON_AddBoolToObject(j, "autoSkipIntro", l.segment[segments::Intro] == segments::Skip);
    cJSON *seg = cJSON_CreateObject();
    for (int t = 0; t < segments::TypeCount; t++)
        cJSON_AddStringToObject(seg, segments::key_of(t), segments::action_key(l.segment[t]));
    cJSON_AddItemToObject(j, "segments", seg);
    cJSON_AddNumberToObject(j, "stillWatching", l.still_watching);
    cJSON_AddNumberToObject(j, "language", l.language);
    cJSON_AddBoolToObject(j, "refresh120", l.refresh_120);
    cJSON_AddNumberToObject(j, "upscale", l.upscale);
    cJSON_AddNumberToObject(j, "audioDelayMs", l.audio_delay_ms);
    cJSON_AddBoolToObject(j, "nightMode", l.night_mode);
    cJSON_AddBoolToObject(j, "hdmiBitstream", l.hdmi_bitstream);
    cJSON_AddBoolToObject(j, "themeMusic", l.theme_music);
    cJSON_AddBoolToObject(j, "checkUpdates", l.check_updates);
    cJSON *st = cJSON_CreateObject();
    cJSON_AddNumberToObject(st, "size", l.sub_size);
    cJSON_AddNumberToObject(st, "offset", l.sub_offset);
    cJSON_AddNumberToObject(st, "background", l.sub_background);
    cJSON_AddBoolToObject(st, "outline", l.sub_outline);
    cJSON_AddItemToObject(j, "subtitles", st);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!text)
        return;
    /* Beside and whole, then renamed over: a crash or a full disk mid-write keeps the
     * old settings instead of truncating them to defaults. (No fsync: this runs on
     * the render thread at every change, and a sync can take tens of ms.) */
    const std::string tmp = std::string(kFile) + ".tmp";
    const size_t n = std::strlen(text);
    bool ok = false;
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        ok = std::fwrite(text, 1, n, f) == n && std::fflush(f) == 0;
        ok = std::fclose(f) == 0 && ok;
    }
    if (!ok || std::rename(tmp.c_str(), kFile) != 0) {
        std::remove(tmp.c_str());
        evo_bt("settings: cannot write %s", kFile);
    }
    std::free(text);
}

void use_server(const std::string &key)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_server = key;
    s_all.server = jf::UserPrefs();   /* the last account's are not this one's */
    s_prefs_of = nullptr;
    auto at = s_quality.find(key);
    s_all.local.max_mbps = at != s_quality.end() ? at->second : s_quality_default;
    s_all.local.max_mbps_for = key;
}

void load_server(jf::Client &c, const std::function<bool()> &current)
{
    jf::UserPrefs p;
    if (!c.get_prefs(&p))
        return;
    std::lock_guard<std::mutex> g(s_lock);
    if (!current())
        return;   /* the account changed while it was read */
    s_all.server = p;
    s_prefs_of = &c;
}

void set_server(jf::Client &c, const jf::UserPrefs &p)
{
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_all.server = p;
        if (s_prefs_of != &c) {
            evo_bt("settings: the account's preferences were never read; kept on the console only");
            return;
        }
    }
    jf::Client *cp = &c;
    const unsigned gen = ++s_save_gen;
    if (!jelly5::spawn([cp, p, gen] {
            std::lock_guard<std::mutex> g(s_save_lock);
            if (gen != s_save_gen)
                return;   /* a newer change posts instead */
            if (!cp->set_prefs(p))
                evo_bt("settings: saving to the server failed: %s", cp->last_error().c_str());
        }))
        evo_bt("settings: saving to the server failed: no thread");
}

} // namespace settings
