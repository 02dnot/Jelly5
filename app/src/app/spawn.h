/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Threads that cannot abort the app. std::thread throws when the system refuses
 * a thread (too many, no memory), and an exception nobody catches aborts; a
 * vector of joinable threads unwound by it aborts too.
 */
#pragma once

#include <functional>
#include <system_error>
#include <thread>
#include <vector>

namespace jelly5 {

/* Runs f on a detached thread. false when the system refused one: nothing ran,
 * so the caller undoes what it set up for it (a "loading" flag, say). */
inline bool spawn(std::function<void()> f)
{
    try {
        std::thread(std::move(f)).detach();
        return true;
    } catch (const std::system_error &) {
        return false;
    }
}

/* Runs the jobs side by side and returns when all are done. A job whose thread
 * cannot be started runs on the caller instead; every started one is joined. */
inline void run_all(const std::vector<std::function<void()>> &jobs)
{
    std::vector<std::thread> running;
    running.reserve(jobs.size());
    for (const auto &job : jobs) {
        try {
            running.emplace_back(job);
        } catch (const std::system_error &) {
            job();
        }
    }
    for (std::thread &t : running)
        t.join();
}

} // namespace jelly5
