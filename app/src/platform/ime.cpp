/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Based on ProsperoTV's iptv_ime.c (BlackBearReloaded, GPL-3.0-or-later):
 * the parameter layout, the UTF-8/UTF-16 conversion and the status protocol.
 */
#include "ime.h"

#include "evo_boot_trace.h"

#include <cstdint>
#include <cstring>
#include <time.h>

namespace {

struct ImeParam {
    int32_t user_id;
    int32_t type;
    uint64_t supported_languages;
    int32_t enter_label;
    int32_t input_method;
    void *filter;
    uint32_t option;
    uint32_t max_text_length;
    uint16_t *input_text_buffer;
    float pos_x, pos_y;
    int32_t horizontal_alignment, vertical_alignment;
    const uint16_t *placeholder;
    const uint16_t *title;
    int8_t reserved[16];
};
struct ImeResult {
    int32_t outcome;
    int8_t reserved[12];
};
static_assert(sizeof(ImeParam) == 96, "unexpected PS5 IME parameter layout");
static_assert(sizeof(ImeResult) == 16, "unexpected PS5 IME result layout");

constexpr uint16_t kSysmoduleImeDialog = 0x0096;
constexpr uint32_t kCommonDialogAlreadyInitialised = 0x80B80002u;
constexpr int32_t kTypeDefault = 0, kTypeBasicLatin = 1;
constexpr int32_t kEnterDefault = 0, kEnterSearch = 2, kEnterGo = 3;
constexpr uint32_t kOptionPassword = 0x4;
constexpr unsigned kMaxChars = 256;

extern "C" {
int sceCommonDialogInitialize(void);
int sceImeDialogAbort(void);
int sceImeDialogGetResult(ImeResult *result);
int sceImeDialogGetStatus(void);
int sceImeDialogInit(const ImeParam *param, const void *extended);
int sceImeDialogTerm(void);
int sceSysmoduleLoadModule(uint16_t module_id);
int sceUserServiceGetForegroundUser(int32_t *user_id);
}

bool s_loaded = false, s_active = false;
double s_started = 0;
uint16_t s_text[kMaxChars + 1];
uint16_t s_title[48];
std::function<void(const std::string &)> s_done;

double now_s()
{
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

void to_utf16(const std::string &in, uint16_t *out, size_t cap)
{
    size_t w = 0;
    const unsigned char *t = (const unsigned char *)in.c_str();
    while (*t && w + 1 < cap) {
        uint32_t cp = *t;
        unsigned n = 1;
        if ((t[0] & 0xe0) == 0xc0) cp = t[0] & 0x1f, n = 2;
        else if ((t[0] & 0xf0) == 0xe0) cp = t[0] & 0x0f, n = 3;
        else if ((t[0] & 0xf8) == 0xf0) cp = t[0] & 0x07, n = 4;
        bool ok = !(n == 1 && (t[0] & 0x80));
        for (unsigned i = 1; i < n && ok; i++) {
            if ((t[i] & 0xc0) != 0x80) ok = false;
            else cp = (cp << 6) | (t[i] & 0x3f);
        }
        if (!ok) cp = 0xfffd, n = 1;
        t += n;
        if (cp >= 0x10000) {
            if (w + 2 >= cap) break;
            cp -= 0x10000;
            out[w++] = (uint16_t)(0xd800 | (cp >> 10));
            out[w++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
        } else {
            out[w++] = (uint16_t)cp;
        }
    }
    out[w] = 0;
}

std::string to_utf8(const uint16_t *in)
{
    std::string out;
    for (size_t i = 0; in[i]; i++) {
        uint32_t cp = in[i];
        if (cp >= 0xd800 && cp <= 0xdbff && in[i + 1] >= 0xdc00 && in[i + 1] <= 0xdfff)
            cp = 0x10000 + ((cp - 0xd800) << 10) + (in[++i] - 0xdc00);
        else if (cp >= 0xd800 && cp <= 0xdfff)
            cp = 0xfffd;
        if (cp < 0x80) {
            out += (char)cp;
        } else if (cp < 0x800) {
            out += (char)(0xc0 | (cp >> 6));
            out += (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            out += (char)(0xe0 | (cp >> 12));
            out += (char)(0x80 | ((cp >> 6) & 0x3f));
            out += (char)(0x80 | (cp & 0x3f));
        } else {
            out += (char)(0xf0 | (cp >> 18));
            out += (char)(0x80 | ((cp >> 12) & 0x3f));
            out += (char)(0x80 | ((cp >> 6) & 0x3f));
            out += (char)(0x80 | (cp & 0x3f));
        }
    }
    return out;
}

void wipe()
{
    volatile uint16_t *p = s_text;
    for (size_t i = 0; i < sizeof s_text / sizeof s_text[0]; i++)
        p[i] = 0;
}

} // namespace

namespace ime {

bool init()
{
    if (s_loaded)
        return true;
    const int cd = sceCommonDialogInitialize();
    if (cd < 0 && (uint32_t)cd != kCommonDialogAlreadyInitialised) {
        evo_bt("ime: common dialog init 0x%08x", (unsigned)cd);
        return false;
    }
    const int rc = sceSysmoduleLoadModule(kSysmoduleImeDialog);
    if (rc < 0) {
        evo_bt("ime: load module 0x%08x", (unsigned)rc);
        return false;
    }
    s_loaded = true;
    return true;
}

bool active() { return s_active; }

void request(Kind kind, const std::string &title, const std::string &initial,
             std::function<void(const std::string &)> done)
{
    if (s_active || !init())
        return;
    int32_t user = -1;
    if (sceUserServiceGetForegroundUser(&user) < 0)
        return;
    to_utf16(kind == Kind::Password ? std::string() : initial, s_text, kMaxChars + 1);
    to_utf16(title, s_title, sizeof s_title / sizeof s_title[0]);
    static const uint16_t empty[1] = {0};
    ImeParam p;
    std::memset(&p, 0, sizeof p);
    p.user_id = user;
    p.type = kind == Kind::Url || kind == Kind::Password ? kTypeBasicLatin : kTypeDefault;
    p.enter_label = kind == Kind::Search ? kEnterSearch : kind == Kind::Url ? kEnterGo : kEnterDefault;
    p.option = kind == Kind::Password ? kOptionPassword : 0;
    p.max_text_length = kMaxChars;
    p.input_text_buffer = s_text;
    p.horizontal_alignment = 1;
    p.vertical_alignment = 1;
    p.placeholder = empty;
    p.title = s_title;
    const int rc = sceImeDialogInit(&p, nullptr);
    s_active = rc == 0;
    if (!s_active) {
        evo_bt("ime: dialog init 0x%08x", (unsigned)rc);
        return;
    }
    s_done = std::move(done);
    s_started = now_s();
}

void poll()
{
    if (!s_active)
        return;
    const int status = sceImeDialogGetStatus();
    if (status == 1 || (status == 0 && now_s() - s_started < 1.0))
        return;   /* running, or not up yet */
    if (status == 2) {
        ImeResult r;
        std::memset(&r, 0, sizeof r);
        if (sceImeDialogGetResult(&r) >= 0 && r.outcome == 0 && s_done) {
            const std::string text = to_utf8(s_text);
            auto done = std::move(s_done);
            s_done = nullptr;
            done(text);
        }
        sceImeDialogTerm();
    }
    wipe();
    s_done = nullptr;
    s_active = false;
}

void cancel()
{
    s_done = nullptr;   /* its owner may be going away: never call it */
    if (s_active)
        sceImeDialogAbort();
}

} // namespace ime
