/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "app/remote.h"

#include "app/syncplay.h"
#include "jf/jf_ws.h"

#include "evo_boot_trace.h"

#include <atomic>
#include <ctime>
#include <deque>
#include <mutex>
#include <thread>
#include <unistd.h>

extern "C" {
#include "cJSON.h"
}

namespace remote {
namespace {

std::mutex s_lock;
std::deque<Command> s_queue;
std::atomic<unsigned> s_generation{0};

std::string str(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : std::string();
}

int64_t num(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (int64_t)v->valuedouble : 0;
}

void push(const Command &c)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_queue.size() < 32)
        s_queue.push_back(c);
}

/* One message from the server: Play, Playstate or GeneralCommand. */
void handle(const std::string &text)
{
    cJSON *j = cJSON_Parse(text.c_str());
    if (!j)
        return;
    const std::string type = str(j, "MessageType");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(j, "Data");
    Command c;
    bool have = false;
    if (type == "Play") {
        c.kind = Command::Play;
        const cJSON *id;
        cJSON_ArrayForEach(id, cJSON_GetObjectItemCaseSensitive(d, "ItemIds"))
            if (cJSON_IsString(id))
                c.item_ids.push_back(id->valuestring);
            else if (cJSON_IsNumber(id))   /* Emby's ids are numbers ("ItemIds":[11]) */
                c.item_ids.push_back(std::to_string((long long)id->valuedouble));
        c.start_index = (int)num(d, "StartIndex");
        c.start_ticks = num(d, "StartPositionTicks");
        c.play_command = str(d, "PlayCommand");
        have = !c.item_ids.empty();
    } else if (type == "Playstate") {
        const std::string cmd = str(d, "Command");
        have = true;
        if (cmd == "Pause") c.kind = Command::Pause;
        else if (cmd == "Unpause") c.kind = Command::Unpause;
        else if (cmd == "PlayPause") c.kind = Command::PlayPause;
        else if (cmd == "Stop") c.kind = Command::Stop;
        else if (cmd == "Seek") c.kind = Command::Seek;
        else if (cmd == "NextTrack") c.kind = Command::Next;
        else if (cmd == "PreviousTrack") c.kind = Command::Previous;
        else if (cmd == "Rewind") c.kind = Command::Rewind;
        else if (cmd == "FastForward") c.kind = Command::FastForward;
        else have = false;
        c.seek_ticks = num(d, "SeekPositionTicks");
    } else if (type == "SyncPlayGroupUpdate") {
        syncplay::on_group_update(d);
    } else if (type == "SyncPlayCommand") {
        syncplay::on_command(d);
    } else if (type == "GeneralCommand" && str(d, "Name") == "DisplayMessage") {
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(d, "Arguments");
        c.kind = Command::Message;
        c.header = str(args, "Header");
        c.text = str(args, "Text");
        have = !c.header.empty() || !c.text.empty();
    }
    if (have) {
        evo_bt("remote: %s %s", type.c_str(), c.play_command.c_str());
        push(c);
    }
    cJSON_Delete(j);
}

void run(jf::Client *client, std::function<bool()> alive, unsigned gen)
{
    auto current = [&] { return gen == s_generation && alive(); };
    int backoff = 2;
    while (current()) {
        jf::WebSocket ws;
        std::string err;
        if (!ws.open(client->socket_url(), {client->auth_header()}, &err)) {
            evo_bt("remote: socket failed: %s", err.c_str());
            for (int i = 0; i < backoff * 10 && current(); i++)
                usleep(100 * 1000);
            backoff = backoff < 60 ? backoff * 2 : 60;
            continue;
        }
        backoff = 2;
        client->post_capabilities();
        evo_bt("remote: listening");
        int keepalive = 30;
        time_t last_sent = time(nullptr);
        while (current()) {
            std::string msg;
            const int r = ws.recv(&msg, 1000);
            if (r < 0)
                break;
            if (r > 0) {
                if (msg.find("\"ForceKeepAlive\"") != std::string::npos) {
                    cJSON *j = cJSON_Parse(msg.c_str());
                    const int secs = (int)num(j, "Data");
                    cJSON_Delete(j);
                    if (secs > 2)
                        keepalive = secs / 2;
                } else {
                    handle(msg);
                }
            }
            if (time(nullptr) - last_sent >= keepalive) {
                if (!ws.send_text("{\"MessageType\":\"KeepAlive\"}"))
                    break;
                last_sent = time(nullptr);
            }
        }
        evo_bt("remote: socket closed");
    }
}

} // namespace

void start(jf::Client *client, std::function<bool()> alive)
{
    const unsigned gen = ++s_generation;
    {
        std::lock_guard<std::mutex> g(s_lock);
        s_queue.clear();
    }
    std::thread([client, alive, gen] { run(client, alive, gen); }).detach();
}

void send(const Command &c) { push(c); }

bool take(Command *out)
{
    std::lock_guard<std::mutex> g(s_lock);
    if (s_queue.empty())
        return false;
    *out = std::move(s_queue.front());
    s_queue.pop_front();
    return true;
}

void put_back(const Command &c)
{
    std::lock_guard<std::mutex> g(s_lock);
    s_queue.push_front(c);
}

} // namespace remote
