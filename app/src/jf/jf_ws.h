/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A small WebSocket client (RFC 6455) for Jellyfin's /socket: text frames,
 * ping/pong and close, over a plain socket for http:// servers and OpenSSL
 * for https:// (no certificate check, as the player's own streams). One
 * thread drives it: recv() waits up to a timeout so the caller can send its
 * keep-alives in between.
 */
#pragma once

#include <string>
#include <vector>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace jf {

class WebSocket {
public:
    WebSocket() = default;
    ~WebSocket() { close(); }
    WebSocket(const WebSocket &) = delete;
    WebSocket &operator=(const WebSocket &) = delete;

    /* url: http(s)://host[:port]/path?query (the scheme picks TLS). headers:
     * "Name: value" lines sent with the upgrade request. */
    bool open(const std::string &url, const std::vector<std::string> &headers, std::string *error);
    bool send_text(const std::string &text);
    /* 1: a text message in *out; 0: nothing within timeout_ms; -1: closed or failed. */
    int recv(std::string *out, int timeout_ms);
    void close();
    bool is_open() const { return m_fd >= 0; }

private:
    bool send_frame(int opcode, const std::string &payload);
    bool read_exact(void *buf, size_t n);
    int raw_read(void *buf, size_t n);
    bool raw_write(const void *buf, size_t n);
    int wait_readable(int timeout_ms);

    int m_fd = -1;
    SSL_CTX *m_ctx = nullptr;
    SSL *m_ssl = nullptr;
    std::string m_pending;              /* bytes read past the handshake */
};

} // namespace jf
