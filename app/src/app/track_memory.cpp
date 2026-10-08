/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/track_memory.h"
#include "nuvio_osd.h"   /* nuvio_language_key */

#include "evo_boot_trace.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <sys/stat.h>

extern "C" {
#include "cJSON.h"
}

#ifndef JELLY5_TRACKS_FILE
#define JELLY5_TRACKS_FILE "/download0/jelly5/tracks.json"
#endif

namespace track_memory {
namespace {

constexpr const char *kFile = JELLY5_TRACKS_FILE;
constexpr size_t kMaxSeries = 200;

struct Entry {
    std::string key;   /* account + "/" + series */
    Choice choice;
};

std::mutex s_lock;
bool s_loaded = false;
std::vector<Entry> s_entries;   /* the most recently changed first */

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)std::tolower((unsigned char)c);
    return s;
}

bool has(const std::string &text, const char *word) { return text.find(word) != std::string::npos; }

/* One key per language: Norwegian comes as nor, nob, nno, no, nb and nn. */
std::string lang_key(const std::string &code)
{
    const std::string k = nuvio_language_key(code);
    if (k == "nb" || k == "nn" || k == "nno" || k == "no")
        return "no";
    if (k == "und" || k == "unknown" || k == "zxx")
        return std::string();
    return k;
}

cJSON *track_json(const Track &t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "lang", t.lang.c_str());
    if (t.forced)
        cJSON_AddTrueToObject(o, "forced");
    if (t.sdh)
        cJSON_AddTrueToObject(o, "sdh");
    if (t.commentary)
        cJSON_AddTrueToObject(o, "commentary");
    if (t.description)
        cJSON_AddTrueToObject(o, "description");
    if (t.image)
        cJSON_AddTrueToObject(o, "image");
    if (t.channels > 0)
        cJSON_AddNumberToObject(o, "channels", t.channels);
    return o;
}

Track track_of(const cJSON *o)
{
    Track t;
    const cJSON *l = cJSON_GetObjectItemCaseSensitive(o, "lang");
    if (cJSON_IsString(l) && l->valuestring)
        t.lang = l->valuestring;
    t.forced = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "forced"));
    t.sdh = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "sdh"));
    t.commentary = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "commentary"));
    t.description = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "description"));
    t.image = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(o, "image"));
    const cJSON *ch = cJSON_GetObjectItemCaseSensitive(o, "channels");
    if (cJSON_IsNumber(ch) && ch->valuedouble > 0 && ch->valuedouble < 64)
        t.channels = (int)ch->valuedouble;
    return t;
}

void load_locked()
{
    if (s_loaded)
        return;
    s_loaded = true;
    std::string body;
    if (FILE *f = std::fopen(kFile, "rb")) {
        char buf[4096];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
            body.append(buf, n);
        std::fclose(f);
    }
    cJSON *j = cJSON_Parse(body.c_str());
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetObjectItemCaseSensitive(j, "series")) {
        const cJSON *k = cJSON_GetObjectItemCaseSensitive(e, "key");
        if (!cJSON_IsString(k) || !k->valuestring || s_entries.size() >= kMaxSeries)
            continue;
        Entry en;
        en.key = k->valuestring;
        if (const cJSON *a = cJSON_GetObjectItemCaseSensitive(e, "audio")) {
            en.choice.audio_set = cJSON_IsObject(a);
            if (en.choice.audio_set)
                en.choice.audio = track_of(a);
        }
        if (const cJSON *s = cJSON_GetObjectItemCaseSensitive(e, "subtitle")) {
            if (cJSON_IsString(s) && s->valuestring && std::strcmp(s->valuestring, "off") == 0) {
                en.choice.subtitle_set = en.choice.subtitle_off = true;
            } else if (cJSON_IsObject(s)) {
                en.choice.subtitle_set = true;
                en.choice.subtitle = track_of(s);
            }
        }
        if (en.choice.audio_set || en.choice.subtitle_set)
            s_entries.push_back(en);
    }
    cJSON_Delete(j);
}

/* Beside and whole, then renamed over (as settings.json): a crash mid-write keeps the old file. */
void save_locked()
{
    cJSON *j = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (const Entry &en : s_entries) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "key", en.key.c_str());
        if (en.choice.audio_set)
            cJSON_AddItemToObject(e, "audio", track_json(en.choice.audio));
        if (en.choice.subtitle_off)
            cJSON_AddStringToObject(e, "subtitle", "off");
        else if (en.choice.subtitle_set)
            cJSON_AddItemToObject(e, "subtitle", track_json(en.choice.subtitle));
        cJSON_AddItemToArray(arr, e);
    }
    cJSON_AddItemToObject(j, "series", arr);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (!text)
        return;
    std::string dir = kFile;
    dir.erase(dir.rfind('/'));
    mkdir(dir.c_str(), 0777);
    const std::string tmp = std::string(kFile) + ".tmp";
    const size_t n = std::strlen(text);
    bool ok = false;
    if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
        ok = std::fwrite(text, 1, n, f) == n && std::fflush(f) == 0;
        ok = std::fclose(f) == 0 && ok;
    }
    if (!ok || std::rename(tmp.c_str(), kFile) != 0) {
        std::remove(tmp.c_str());
        evo_bt("tracks: cannot write %s", kFile);
    }
    std::free(text);
}

/* The series' entry, moved to the front (created when missing; the oldest beyond the bound go). */
Entry &touch_locked(const std::string &key)
{
    load_locked();
    auto it = std::find_if(s_entries.begin(), s_entries.end(), [&](const Entry &e) { return e.key == key; });
    Entry en;
    if (it != s_entries.end()) {
        en = *it;
        s_entries.erase(it);
    } else {
        en.key = key;
    }
    s_entries.insert(s_entries.begin(), en);
    if (s_entries.size() > kMaxSeries)
        s_entries.resize(kMaxSeries);
    return s_entries.front();
}

} // namespace

Track describe(const jf::MediaStream &m)
{
    Track t;
    t.lang = lang_key(m.language);
    /* The flags the server read from the file, and what a title says (Jellyfin
     * and Emby have no commentary flag; older files name SDH and forced only there). */
    const std::string title = lower(m.title);
    t.forced = m.is_forced || has(title, "forced");
    t.sdh = m.is_hearing_impaired || has(title, "sdh") || has(title, "hearing impaired");
    t.commentary = has(title, "commentary") || has(title, "kommentar") || has(title, "commentaire");
    if (m.type == "Audio") {
        t.description = has(title, "description") || has(title, "descriptive") || has(title, "synstolk");
        t.channels = m.channels;
    } else {
        /* Jellyfin and Emby say IsTextSubtitleStream; the picture formats by name, for a server that does not. */
        const std::string c = lower(m.codec);
        t.image = !m.is_text && (has(c, "pgs") || has(c, "dvd") || has(c, "dvb") || has(c, "vobsub") ||
                                 has(c, "xsub") || c == "sup");
    }
    return t;
}

int match(const std::vector<jf::MediaStream> &streams, const char *type, const Track &want)
{
    int best = -1, best_score = -1;
    for (const jf::MediaStream &m : streams) {
        if (m.type != type || m.index < 0)
            continue;
        if (m.is_external && m.type == "Audio")
            continue;   /* the player opens the file itself only: a separate audio file is not in it */
        const Track t = describe(m);
        if (t.lang != want.lang || t.forced != want.forced || t.commentary != want.commentary ||
            t.description != want.description)
            continue;
        int score = 0;
        if (t.sdh == want.sdh)
            score += 8;
        if (t.image == want.image)
            score += 4;
        if (m.is_default)
            score += 2;
        if (want.channels > 0 && t.channels == want.channels)
            score += 1;
        score = score * 16 + std::min(t.channels, 15);   /* then the most channels */
        if (score > best_score) {   /* (equal: the first) */
            best_score = score;
            best = m.index;
        }
    }
    return best;
}

bool get(const std::string &account, const std::string &series, Choice *out)
{
    if (account.empty() || series.empty())
        return false;
    std::lock_guard<std::mutex> g(s_lock);
    load_locked();
    const std::string key = account + "/" + series;
    for (const Entry &e : s_entries)
        if (e.key == key) {
            *out = e.choice;
            return true;
        }
    return false;
}

void set_audio(const std::string &account, const std::string &series, const Track &t)
{
    if (account.empty() || series.empty())
        return;
    std::lock_guard<std::mutex> g(s_lock);
    Entry &e = touch_locked(account + "/" + series);
    e.choice.audio_set = true;
    e.choice.audio = t;
    save_locked();
}

void set_subtitle(const std::string &account, const std::string &series, bool off, const Track &t)
{
    if (account.empty() || series.empty())
        return;
    std::lock_guard<std::mutex> g(s_lock);
    Entry &e = touch_locked(account + "/" + series);
    e.choice.subtitle_set = true;
    e.choice.subtitle_off = off;
    e.choice.subtitle = off ? Track() : t;
    save_locked();
}

} // namespace track_memory
