/*
 * Jelly5 — jf::http_request on libcurl, for host-side tests.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_http.h"

#include <curl/curl.h>

namespace jf {

static size_t on_body(char *p, size_t size, size_t n, void *ud)
{
    static_cast<std::string *>(ud)->append(p, size * n);
    return size * n;
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
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &res.body);
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
    return res;
}

} // namespace jf
