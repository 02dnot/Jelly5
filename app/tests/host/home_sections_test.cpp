/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Client::home_sections() against canned DisplayPreferences answers, no network:
 * an unset slot takes jellyfin-web's default for its place, none set is the
 * default order (empty), "folders" is the first default, and a body that is not
 * JSON is no answer (ok = false).
 */
#include "jf_client.h"
#include "jf_http.h"

#include <cstdio>
#include <string>
#include <vector>

static std::string s_body;

namespace jf {
HttpResponse http_request(const std::string &, const std::string &url, const std::vector<std::string> &,
                          const std::string &, int)
{
    HttpResponse r;
    r.status = 200;
    r.body = url.find("/DisplayPreferences/") != std::string::npos ? s_body
                                                                   : R"({"Name":"u","Configuration":{},"Policy":{}})";
    return r;
}
} // namespace jf

static int s_failed = 0;

static void check(const char *name, const std::string &body, const std::vector<std::string> &want, bool want_ok)
{
    s_body = body;
    jf::Client c("http://srv", "dev1", "PS5");
    c.set_session("TOKEN", "u1", "u");
    bool ok = !want_ok;
    const std::vector<std::string> got = c.home_sections(&ok);
    if (got == want && ok == want_ok) {
        std::printf("ok   %s\n", name);
        return;
    }
    s_failed++;
    std::printf("FAIL %s: ok=%d, got", name, ok);
    for (const std::string &s : got)
        std::printf(" %s", s.c_str());
    std::printf("\n");
}

int main()
{
    const std::vector<std::string> none;
    check("nothing set", R"({"CustomPrefs":{}})", none, true);
    check("no CustomPrefs", R"({})", none, true);
    check("only empty values", R"({"CustomPrefs":{"homesection0":""}})", none, true);
    check("first two set",
          R"({"CustomPrefs":{"homesection0":"resume","homesection1":"latestmedia"}})",
          {"resume", "latestmedia", "resumeaudio", "resumebook", "livetv", "nextup", "latestmedia", "none", "none",
           "none"},
          true);
    check("a gap at slot 1",
          R"({"CustomPrefs":{"homesection0":"latestmedia","homesection2":"nextup"}})",
          {"latestmedia", "resume", "nextup", "resumebook", "livetv", "nextup", "latestmedia", "none", "none", "none"},
          true);
    check("folders",
          R"({"CustomPrefs":{"homesection0":"folders","homesection1":"nextup"}})",
          {"smalllibrarytiles", "nextup", "resumeaudio", "resumebook", "livetv", "nextup", "latestmedia", "none",
           "none", "none"},
          true);
    check("not JSON", "<html>proxy</html>", none, false);
    return s_failed ? 1 : 0;
}
