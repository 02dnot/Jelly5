/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_ws.h"

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace jf {
namespace {

bool split_url(const std::string &url, bool *tls, std::string *host, std::string *port, std::string *path)
{
    size_t p = url.find("://");
    if (p == std::string::npos)
        return false;
    const std::string scheme = url.substr(0, p);
    *tls = scheme == "https" || scheme == "wss";
    p += 3;
    const size_t slash = url.find('/', p);
    std::string hostport = url.substr(p, slash == std::string::npos ? std::string::npos : slash - p);
    *path = slash == std::string::npos ? "/" : url.substr(slash);
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos && hostport.find(']') == std::string::npos) {
        *host = hostport.substr(0, colon);
        *port = hostport.substr(colon + 1);
    } else {
        *host = hostport;
        *port = *tls ? "443" : "80";
    }
    return !host->empty();
}

std::string base64(const unsigned char *d, size_t n)
{
    static const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < n; i += 3) {
        const uint32_t v = (uint32_t)d[i] << 16 | (i + 1 < n ? (uint32_t)d[i + 1] << 8 : 0) | (i + 2 < n ? d[i + 2] : 0);
        out += t[(v >> 18) & 63];
        out += t[(v >> 12) & 63];
        out += i + 1 < n ? t[(v >> 6) & 63] : '=';
        out += i + 2 < n ? t[v & 63] : '=';
    }
    return out;
}

/* A connect that gives up after timeout_ms, then a blocking socket whose reads
 * and writes give up after 10 s: poll() can say readable while SSL_read still
 * waits for the rest of a TLS record, and a server that stops there must not
 * hold the remote thread past an account switch. A timed-out read or write
 * fails, and the caller reconnects. */
bool connect_with_timeout(int fd, const sockaddr *addr, socklen_t len, int timeout_ms)
{
    /* Non-blocking connect, then poll and SO_ERROR for the outcome. errno is not
     * consulted: the app's libc doesn't see the kernel's EINPROGRESS (a connect in
     * progress read as a failure, on every attempt, on the console). Without
     * fcntl, a plain blocking connect; the timeouts below still bound the reads. */
    const int flags = fcntl(fd, F_GETFL, 0);
    const bool nonblock = flags >= 0 && fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
    if (connect(fd, addr, len) != 0) {
        if (!nonblock)
            return false;
        pollfd p{fd, POLLOUT, 0};
        int err = 0;
        socklen_t elen = sizeof err;
        if (poll(&p, 1, timeout_ms) <= 0 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0)
            return false;
    }
    if (nonblock && fcntl(fd, F_SETFL, flags) < 0)
        return false;
    timeval tv{10, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return true;
}

unsigned rnd()
{
    static unsigned s = (unsigned)time(nullptr) ^ 0x5a17u;
    s = s * 1103515245u + 12345u;
    return s >> 8;
}

} // namespace

bool WebSocket::open(const std::string &url, const std::vector<std::string> &headers, std::string *error)
{
    close();
    bool tls;
    std::string host, port, path;
    if (!split_url(url, &tls, &host, &port, &path)) {
        *error = "bad url";
        return false;
    }
    addrinfo hints;
    std::memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port.c_str(), &hints, &res) != 0 || !res) {
        *error = "cannot resolve " + host;
        return false;
    }
    m_fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (m_fd < 0 || !connect_with_timeout(m_fd, res->ai_addr, res->ai_addrlen, 8000)) {
        freeaddrinfo(res);
        *error = "cannot connect to " + host + ":" + port;
        close();
        return false;
    }
    freeaddrinfo(res);
    const int one = 1;
    setsockopt(m_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);

    if (tls) {
        m_ctx = SSL_CTX_new(TLS_client_method());
        m_ssl = m_ctx ? SSL_new(m_ctx) : nullptr;
        if (!m_ssl) {
            *error = "tls setup failed";
            close();
            return false;
        }
        SSL_set_fd(m_ssl, m_fd);
        SSL_set_tlsext_host_name(m_ssl, host.c_str());
        if (SSL_connect(m_ssl) != 1) {
            *error = "tls handshake failed";
            close();
            return false;
        }
    }

    unsigned char nonce[16];
    for (unsigned char &b : nonce)
        b = (unsigned char)rnd();
    std::string req = "GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + port +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                      "Sec-WebSocket-Key: " + base64(nonce, sizeof nonce) + "\r\n";
    for (const std::string &h : headers)
        req += h + "\r\n";
    req += "\r\n";
    if (!raw_write(req.data(), req.size())) {
        *error = "handshake write failed";
        close();
        return false;
    }
    /* The response head, up to the blank line; anything after it is frames. */
    std::string head;
    char buf[1024];
    while (head.find("\r\n\r\n") == std::string::npos && head.size() < 16384) {
        if (wait_readable(8000) <= 0) {
            *error = "no handshake response";
            close();
            return false;
        }
        const int n = raw_read(buf, sizeof buf);
        if (n <= 0) {
            *error = "handshake read failed";
            close();
            return false;
        }
        head.append(buf, (size_t)n);
    }
    const size_t end = head.find("\r\n\r\n");
    const std::string status = head.substr(0, head.find("\r\n"));
    if (status.find(" 101") == std::string::npos) {
        *error = status;
        close();
        return false;
    }
    m_pending = head.substr(end + 4);
    return true;
}

void WebSocket::close()
{
    if (m_ssl) {
        SSL_free(m_ssl);
        m_ssl = nullptr;
    }
    if (m_ctx) {
        SSL_CTX_free(m_ctx);
        m_ctx = nullptr;
    }
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
    }
    m_pending.clear();
}

int WebSocket::wait_readable(int timeout_ms)
{
    if (!m_pending.empty() || (m_ssl && SSL_pending(m_ssl) > 0))
        return 1;
    pollfd p{m_fd, POLLIN, 0};
    const int r = poll(&p, 1, timeout_ms);
    if (r > 0 && (p.revents & (POLLERR | POLLHUP | POLLNVAL)) && !(p.revents & POLLIN))
        return -1;
    return r;
}

int WebSocket::raw_read(void *buf, size_t n)
{
    if (!m_pending.empty()) {
        const size_t k = std::min(n, m_pending.size());
        std::memcpy(buf, m_pending.data(), k);
        m_pending.erase(0, k);
        return (int)k;
    }
    if (m_ssl)
        return SSL_read(m_ssl, buf, (int)n);
    return (int)::recv(m_fd, buf, n, 0);
}

bool WebSocket::raw_write(const void *buf, size_t n)
{
    const char *p = (const char *)buf;
    while (n > 0) {
        const int w = m_ssl ? SSL_write(m_ssl, p, (int)n) : (int)::send(m_fd, p, n, 0);
        if (w <= 0)
            return false;
        p += w;
        n -= (size_t)w;
    }
    return true;
}

bool WebSocket::read_exact(void *buf, size_t n)
{
    char *p = (char *)buf;
    while (n > 0) {
        if (wait_readable(10000) <= 0)
            return false;
        const int r = raw_read(p, n);
        if (r <= 0)
            return false;
        p += r;
        n -= (size_t)r;
    }
    return true;
}

/* Client frames are always masked (RFC 6455 5.3). */
bool WebSocket::send_frame(int opcode, const std::string &payload)
{
    if (m_fd < 0)
        return false;
    std::string f;
    f += (char)(0x80 | opcode);
    const size_t n = payload.size();
    if (n < 126) {
        f += (char)(0x80 | n);
    } else if (n < 65536) {
        f += (char)(0x80 | 126);
        f += (char)(n >> 8);
        f += (char)(n & 0xff);
    } else {
        f += (char)(0x80 | 127);
        for (int i = 7; i >= 0; i--)
            f += (char)((uint64_t)n >> (8 * i));
    }
    unsigned char mask[4];
    for (unsigned char &b : mask)
        b = (unsigned char)rnd();
    f.append((const char *)mask, 4);
    for (size_t i = 0; i < n; i++)
        f += (char)(payload[i] ^ mask[i & 3]);
    return raw_write(f.data(), f.size());
}

bool WebSocket::send_text(const std::string &text) { return send_frame(0x1, text); }

int WebSocket::recv(std::string *out, int timeout_ms)
{
    std::string message;
    for (;;) {
        if (m_fd < 0)
            return -1;
        const int w = wait_readable(message.empty() ? timeout_ms : 10000);
        if (w == 0)
            return 0;
        if (w < 0)
            return -1;
        unsigned char h[2];
        if (!read_exact(h, 2))
            return -1;
        const bool fin = h[0] & 0x80;
        const int opcode = h[0] & 0x0f;
        uint64_t len = h[1] & 0x7f;
        if (len == 126) {
            unsigned char e[2];
            if (!read_exact(e, 2))
                return -1;
            len = (uint64_t)e[0] << 8 | e[1];
        } else if (len == 127) {
            unsigned char e[8];
            if (!read_exact(e, 8))
                return -1;
            len = 0;
            for (unsigned char b : e)
                len = len << 8 | b;
        }
        if (len > (8u << 20) || message.size() + len > (8u << 20))
            return -1;   /* nothing Jellyfin sends is this large, in one frame or in pieces */
        if (opcode >= 0x8 && (len > 125 || !fin))
            return -1;   /* control frames are short and never fragmented (RFC 6455 5.5) */
        unsigned char mask[4] = {0, 0, 0, 0};
        if ((h[1] & 0x80) && !read_exact(mask, 4))
            return -1;
        std::string payload((size_t)len, '\0');
        if (len && !read_exact(&payload[0], (size_t)len))
            return -1;
        if (h[1] & 0x80)
            for (size_t i = 0; i < payload.size(); i++)
                payload[i] = (char)(payload[i] ^ mask[i & 3]);
        switch (opcode) {
        case 0x8:   /* close */
            send_frame(0x8, std::string());
            close();
            return -1;
        case 0x9:   /* ping */
            send_frame(0xA, payload);
            continue;
        case 0xA:   /* pong */
            continue;
        default:    /* text, binary or a continuation of either */
            message += payload;
            if (fin) {
                *out = std::move(message);
                return 1;
            }
        }
    }
}

} // namespace jf
