/*
 * Jelly5 — jf::http_request on libcurl, for host-side tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * It answers the way the console's FFmpeg client does (jf_http_avio.cpp), so
 * what the tests exercise is what runs on the PS5: every 2xx arrives as 200,
 * and an error status comes without its body, folded as FFmpeg folds it
 * (400, 401, 403, 404, any other 4xx as 499, any 5xx as 500).
 */
#include "jf_http.h"

#include <curl/curl.h>
#include <strings.h>

namespace jf {

static size_t on_body(char *p, size_t size, size_t n, void *ud)
{
    static_cast<std::string *>(ud)->append(p, size * n);
    return size * n;
}

static size_t on_header(char *p, size_t size, size_t n, void *ud)
{
    static const char kSetCookie[] = "Set-Cookie:";
    const size_t len = size * n, k = sizeof kSetCookie - 1;
    if (len > k && strncasecmp(p, kSetCookie, k) == 0) {
        std::string v(p + k, len - k);
        while (!v.empty() && (v.back() == '\r' || v.back() == '\n' || v.back() == ' '))
            v.pop_back();
        while (!v.empty() && v.front() == ' ')
            v.erase(0, 1);
        if (!v.empty())
            static_cast<HttpResponse *>(ud)->cookies.push_back(v);
    }
    return len;
}

HttpResponse http_request(const std::string &method, const std::string &url,
                          const std::vector<std::string> &headers, const std::string &body,
                          int timeout_s)
{
    HttpResponse res;
    CURL *c = curl_easy_init();
    curl_slist *hl = nullptr;
    for (const auto &h : headers)
        hl = curl_slist_append(hl, h.c_str());
    if (!body.empty()) {
        hl = curl_slist_append(hl, "Content-Type: application/json");
        curl_easy_setopt(c, CURLOPT_POSTFIELDS, body.c_str());
    }
    curl_easy_setopt(c, CURLOPT_URL, url.c_str());
    curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method.c_str());
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, hl);
    curl_easy_setopt(c, CURLOPT_TIMEOUT, (long)timeout_s);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);   /* as FFmpeg does */
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &res.body);
    curl_easy_setopt(c, CURLOPT_HEADERFUNCTION, on_header);
    curl_easy_setopt(c, CURLOPT_HEADERDATA, &res);
    CURLcode rc = curl_easy_perform(c);
    if (rc == CURLE_OK) {
        long code = 0;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        res.status = (int)code;
    } else {
        res.error = curl_easy_strerror(rc);
    }
    curl_slist_free_all(hl);
    curl_easy_cleanup(c);
    if (res.status >= 200 && res.status < 300) {
        res.status = 200;
    } else if (res.status >= 400) {
        if (res.status >= 500)
            res.status = 500;
        else if (res.status != 400 && res.status != 401 && res.status != 403 && res.status != 404)
            res.status = 499;
        res.error = "HTTP error " + std::to_string(res.status);
        res.body.clear();
        res.cookies.clear();
    }
    return res;
}

} // namespace jf
