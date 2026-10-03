/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * jf::http_request on FFmpeg's HTTP protocol (avio). FFmpeg reports 4xx/5xx
 * as open errors, which are mapped back to status codes here.
 */
#include "jf_http.h"

#include <cstdio>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
}

namespace jf {
namespace {

int status_of_error(int rc)
{
    switch (rc) {
    case AVERROR_HTTP_BAD_REQUEST:  return 400;
    case AVERROR_HTTP_UNAUTHORIZED: return 401;
    case AVERROR_HTTP_FORBIDDEN:    return 403;
    case AVERROR_HTTP_NOT_FOUND:    return 404;
    case AVERROR_HTTP_OTHER_4XX:    return 499;
    case AVERROR_HTTP_SERVER_ERROR: return 500;
    default:                        return 0;
    }
}

/* FFmpeg's binary post_data option takes hex when set through a dictionary. */
std::string hex_of(const std::string &s)
{
    static const char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s) {
        out += digits[c >> 4];
        out += digits[c & 15];
    }
    return out;
}

} // namespace

HttpResponse http_request(const std::string &method, const std::string &url,
                          const std::vector<std::string> &headers, const std::string &body,
                          int timeout_s)
{
    HttpResponse res;
    AVDictionary *opts = nullptr;
    av_dict_set_int(&opts, "timeout", (int64_t)timeout_s * 1000000, 0);
    av_dict_set_int(&opts, "rw_timeout", (int64_t)timeout_s * 1000000, 0);
    av_dict_set(&opts, "method", method.c_str(), 0);
    av_dict_set(&opts, "reconnect", "0", 0);
    std::string head;
    for (const auto &h : headers)
        head += h + "\r\n";
    if (!body.empty()) {
        head += "Content-Type: application/json\r\n";
        av_dict_set(&opts, "post_data", hex_of(body).c_str(), 0);
    }
    if (!head.empty())
        av_dict_set(&opts, "headers", head.c_str(), 0);

    AVIOContext *io = nullptr;
    const int rc = avio_open2(&io, url.c_str(), AVIO_FLAG_READ, nullptr, &opts);
    av_dict_free(&opts);
    if (rc < 0) {
        res.status = status_of_error(rc);
        char err[128];
        av_strerror(rc, err, sizeof err);
        res.error = err;
        return res;
    }
    unsigned char buf[16384];
    int n;
    while ((n = avio_read(io, buf, sizeof buf)) > 0)
        res.body.append((const char *)buf, (size_t)n);
    avio_closep(&io);
    res.status = 200;   /* avio exposes no 2xx detail; 204 arrives as an empty 200 */
    return res;
}

} // namespace jf
