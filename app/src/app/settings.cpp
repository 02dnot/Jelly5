/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/settings.h"
#include "app/i18n.h"

#include <algorithm>

#include "evo_boot_trace.h"

#include <cstdio>
#include <cstdlib>
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
    s_quality_default = std::max(0, (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "maxMbps")));
    s_quality.clear();
    const cJSON *q;
    cJSON_ArrayForEach(q, cJSON_GetObjectItemCaseSensitive(j, "maxMbpsByServer"))
        if (q->string && cJSON_IsNumber(q))
            s_quality[q->string] = std::max(0, (int)q->valuedouble);
    auto at = s_quality.find(s_server);
    s_all.local.max_mbps = at != s_quality.end() ? at->second : s_quality_default;
    s_all.local.auto_skip_intro = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(j, "autoSkipIntro"));
    s_all.local.language = (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "language"));
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "nightMode"))
        s_all.local.night_mode = cJSON_IsTrue(v);
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "themeMusic"))
        s_all.local.theme_music = cJSON_IsTrue(v);
    if (const cJSON *v = cJSON_GetObjectItemCaseSensitive(j, "checkUpdates"))
        s_all.local.check_updates = cJSON_IsTrue(v);
    s_all.local.audio_delay_ms =
        std::max(-500, std::min(500, (int)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(j, "audioDelayMs"))));
    if (const cJSON *hz = cJSON_GetObjectItemCaseSensitive(j, "refresh120"))
        s_all.local.refresh_120 = cJSON_IsTrue(hz);
    if (const cJSON *st = cJSON_GetObjectItemCaseSensitive(j, "subtitles")) {
        Local &l = s_all.local;
        const cJSON *v;
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(st, "size")))
            l.sub_size = std::max(50, std::min(200, (int)v->valuedouble));
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(st, "offset")))
            l.sub_offset = std::max(0.f, std::min(40.f, (float)v->valuedouble));
        if (cJSON_IsNumber(v = cJSON_GetObjectItemCaseSensitive(st, "background")))
            l.sub_background = std::max(0.f, std::min(1.f, (float)v->valuedouble));
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
        s_all.local = l;
        if (!s_server.empty())
            s_quality[s_server] = l.max_mbps;
        else
            s_quality_default = l.max_mbps;
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
    cJSON_AddBoolToObject(j, "autoSkipIntro", l.auto_skip_intro);
    cJSON_AddNumberToObject(j, "language", l.language);
    cJSON_AddBoolToObject(j, "refresh120", l.refresh_120);
    cJSON_AddNumberToObject(j, "audioDelayMs", l.audio_delay_ms);
    cJSON_AddBoolToObject(j, "nightMode", l.night_mode);
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
    if (FILE *f = std::fopen(kFile, "wb")) {
        std::fputs(text, f);
        std::fclose(f);
    }
    std::free(text);
}

void use_server(const std::string &key)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_server = key;
    auto at = s_quality.find(key);
    s_all.local.max_mbps = at != s_quality.end() ? at->second : s_quality_default;
}

void load_server(jf::Client &c)
{
    jf::UserPrefs p;
    if (!c.get_prefs(&p))
        return;
    std::lock_guard<std::mutex> g(s_lock);
    s_all.server = p;
}

void set_server(jf::Client &c, const jf::UserPrefs &p)
{
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_all.server = p;
    }
    jf::Client *cp = &c;
    std::thread([cp, p] {
        if (!cp->set_prefs(p))
            evo_bt("settings: saving to the server failed: %s", cp->last_error().c_str());
    }).detach();
}

} // namespace settings
