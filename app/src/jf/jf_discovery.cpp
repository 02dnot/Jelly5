/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "jf_discovery.h"

#include "cJSON.h"

#include <arpa/inet.h>
#include <cstring>
#include <ctime>
#include <string>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace jf {
namespace {

double now_ms()
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

std::string str(const cJSON *o, const char *key)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

/* The host of an address ("http://192.168.0.10:8096" -> "192.168.0.10"). */
std::string host_of(const std::string &address)
{
    size_t b = address.find("://");
    b = b == std::string::npos ? 0 : b + 3;
    if (b < address.size() && address[b] == '[')   /* IPv6: [::1]:8096 */
        return address.substr(b, address.find(']', b) - b + 1);
    const size_t e = address.find_first_of(":/?#", b);
    return address.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

/* A reply names its server's address. An IP there must be the one it came from:
 * otherwise anyone on the network could send the PS5 to any address. A name
 * (a server's PublishedServerUrl, say) cannot be checked here and is taken; the
 * login offers only those that answer at their address anyway. */
bool plausible(const std::string &address, const struct in_addr &from)
{
    const std::string host = host_of(address);
    if (host.empty())
        return false;
    struct in_addr a;
    if (inet_pton(AF_INET, host.c_str(), &a) == 1)
        return a.s_addr == from.s_addr;
    if (host[0] == '[')
        return false;   /* (the broadcast is IPv4: an IPv6 address did not answer it) */
    return true;
}

/* A busy network has a few servers, not hundreds: each one found is probed. */
constexpr size_t kMaxServers = 16;

} // namespace

std::vector<FoundServer> discover(int timeout_ms)
{
    std::vector<FoundServer> out;
    const int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0)
        return out;
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &on, sizeof on);
    struct sockaddr_in to;
    memset(&to, 0, sizeof to);
    to.sin_family = AF_INET;
    to.sin_port = htons(7359);
    to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
    /* Jellyfin and Emby answer only their own question, with the same reply. */
    static const char *const kAsk[] = {"who is JellyfinServer?", "who is EmbyServer?"};
    int sent = 0;
    for (const char *ask : kAsk)
        sent += sendto(fd, ask, strlen(ask), 0, (const struct sockaddr *)&to, sizeof to) >= 0;
    if (!sent) {
        close(fd);
        return out;
    }
    const double end = now_ms() + timeout_ms;
    for (;;) {
        const int left = (int)(end - now_ms());
        if (left <= 0)
            break;
        struct pollfd p = {fd, POLLIN, 0};
        if (poll(&p, 1, left) <= 0)
            break;
        char buf[2048];
        struct sockaddr_in from;
        socklen_t from_len = sizeof from;
        memset(&from, 0, sizeof from);
        const ssize_t n = recvfrom(fd, buf, sizeof buf - 1, 0, (struct sockaddr *)&from, &from_len);
        if (n <= 0 || from.sin_family != AF_INET)
            continue;
        buf[n] = 0;
        cJSON *j = cJSON_Parse(buf);
        if (!j)
            continue;
        FoundServer s{str(j, "Id"), str(j, "Name"), str(j, "Address")};
        cJSON_Delete(j);
        if (s.address.empty() || !plausible(s.address, from.sin_addr))
            continue;
        bool seen = false;
        for (const FoundServer &o : out)
            seen = seen || (!s.id.empty() ? o.id == s.id : o.address == s.address);
        if (!seen)
            out.push_back(s);
        if (out.size() >= kMaxServers)
            break;
    }
    close(fd);
    return out;
}

} // namespace jf
