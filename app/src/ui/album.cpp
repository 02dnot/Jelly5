/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ui/album.h"
#include "app/i18n.h"

#include "gfx/art.h"
#include "nuvio_input.h"

#include <algorithm>
#include <cstdio>
#include <thread>

namespace ui {
namespace {

constexpr float kCover = 520, kTop = 190;
constexpr float kListX = kPad + kCover + 90;
constexpr float kListTop = 600, kRowH = 66;

std::string duration(int64_t ticks)
{
    const int s = (int)(ticks / jf::kTicksPerSecond);
    char b[16];
    std::snprintf(b, sizeof b, "%d:%02d", s / 60, s % 60);
    return b;
}

std::string cover_url(jf::Client &c, const jf::Item &it)
{
    return c.image_url(it.id, "Primary", it.primary_tag, 800);
}

} // namespace

Album::Album(jf::Client &client, const jf::Item &album)
    : m_client(client), m_album(album), m_playlist(album.type == "Playlist")
{
    m_data->album = album;
}

std::vector<Album::Button> Album::buttons() const
{
    std::vector<Button> b{PlayAll, Shuffle};
    if (!m_playlist) {
        b.push_back(Mix);
        if (!m_album.album_artist_id.empty())
            b.push_back(Artist);
    }
    return b;
}

/* An album plays through itself (the player builds the queue); a playlist is
 * handed over as the queue. */
Action Album::play_from(size_t i, bool shuffled) const
{
    Action a;
    if (m_tracks.empty())
        return a;
    i = std::min(i, m_tracks.size() - 1);
    a.kind = Action::Play;
    a.item = m_tracks[i];
    if (m_playlist) {
        a.queue = m_tracks;
        a.queue_start = i;
        if (shuffled) {
            for (size_t k = a.queue.size(); k > 1; k--)
                std::swap(a.queue[k - 1], a.queue[(size_t)std::rand() % k]);
            a.queue_start = 0;
            a.item = a.queue.front();
        }
        for (jf::Item &t : a.queue)
            t.position_ticks = 0;
    } else if (shuffled) {
        a.kind = Action::PlayShuffled;
    }
    a.item.position_ticks = 0;   /* songs start from the top */
    return a;
}

void Album::activate()
{
    m_enter.to(1.f);
    std::shared_ptr<Data> d = m_data;
    jf::Client *c = &m_client;
    const std::string id = m_album.id;
    const bool playlist = m_playlist;
    std::thread([d, c, id, playlist] {
        jf::Item album;
        bool got = false;
        std::vector<jf::Item> tracks;
        std::thread a([&] { got = c->item(id, &album); });
        if (playlist) {
            for (jf::Item &t : c->playlist_items(id))
                if (t.type == "Audio" || t.type == "Movie" || t.type == "Episode" || t.type == "Video" ||
                    t.type == "MusicVideo")
                    tracks.push_back(std::move(t));
        } else {
            for (jf::Item &t : c->children(id, "ParentIndexNumber,IndexNumber,SortName", 1000))
                if (t.type == "Audio")
                    tracks.push_back(std::move(t));
        }
        a.join();
        std::lock_guard<std::mutex> g(d->lock);
        if (got)
            d->album = album;
        d->tracks = std::move(tracks);
        d->loaded = true;
    }).detach();
}

Action Album::input(uint32_t p)
{
    Action a;
    const int n = (int)m_tracks.size();
    if (p & NUVIO_BTN_CIRCLE) {
        if (m_in_tracks)
            m_in_tracks = false;
        else
            a.kind = Action::Back;
    } else if (p & NUVIO_BTN_DOWN) {
        if (!m_in_tracks && n > 0)
            m_in_tracks = true;
        else if (m_in_tracks)
            m_track = std::min(n - 1, m_track + 1);
    } else if (p & NUVIO_BTN_UP) {
        if (m_in_tracks && m_track > 0)
            m_track--;
        else
            m_in_tracks = false;
    } else if ((p & (NUVIO_BTN_LEFT | NUVIO_BTN_RIGHT)) && !m_in_tracks) {
        const int nb = (int)buttons().size();
        m_button = std::max(0, std::min(nb - 1, m_button + ((p & NUVIO_BTN_RIGHT) ? 1 : -1)));
    } else if ((p & NUVIO_BTN_CROSS) && n > 0) {
        if (m_in_tracks)
            return play_from((size_t)std::min(m_track, n - 1), false);
        const std::vector<Button> bs = buttons();
        switch (bs[std::min(m_button, (int)bs.size() - 1)]) {
        case PlayAll: return play_from(0, false);
        case Shuffle: return play_from((size_t)std::rand() % m_tracks.size(), true);   /* a random first track */
        case Mix:
            a.kind = Action::PlayMix;
            a.item = m_album;
            break;
        case Artist:
            a.kind = Action::Open;
            a.item.type = "MusicArtist";
            a.item.id = m_album.album_artist_id;
            a.item.name = m_album.album_artist;
            break;
        }
    }
    return a;
}

void Album::draw(double now, float dt)
{
    (void)now;
    m_animating = false;
    {
        std::lock_guard<std::mutex> g(m_data->lock);
        m_album = m_data->album;
        if (m_data->loaded && !m_loaded) {
            m_tracks = m_data->tracks;
            m_loaded = true;
        }
    }
    if (m_enter.step(dt, 9.f))
        m_animating = true;
    m_content.to(m_loaded ? 1.f : 0.f);
    if (m_content.step(dt, 8.f))
        m_animating = true;

    const std::string cover = cover_url(m_client, m_album);
    gfx::fill({0, 0, gfx::W, gfx::H}, kBg);
    if (const gfx::Texture *bh = art::blurhash(m_album.primary_blurhash))
        gfx::image({0, 0, gfx::W, gfx::H}, bh, 0.5f, 0, true);
    gfx::fill_hgradient({0, 0, gfx::W, gfx::H}, 0x9907070au, 0xe607070au);

    const gfx::Rect cr{kPad, kTop, kCover, kCover};
    gfx::shadow(cr, 20, 50, 0.7f, 22);
    art::draw(cr, cover, m_album.primary_blurhash, 800, 800, 20, 1.f, 0xff1c1c22u);

    /* Title, artist, year · tracks · minutes. */
    const float x = kListX, w = gfx::W - kPad - x;
    gfx::text(x, kTop + 70, m_album.name, {gfx::Bold, 60, w}, kText);
    const std::string by = m_playlist ? std::string(T("Spilleliste")) : m_album.album_artist;
    if (!by.empty())
        gfx::text(x, kTop + 124, by, {gfx::Medium, 32, w}, kText2);
    std::string meta;
    if (m_album.year)
        meta = std::to_string(m_album.year);
    if (!m_tracks.empty()) {
        int64_t total = 0;
        for (const jf::Item &t : m_tracks)
            total += t.runtime_ticks;
        const int min = (int)(total / jf::kTicksPerSecond / 60);
        char b[64];
        std::snprintf(b, sizeof b, "%zu %s \xC2\xB7 %d min", m_tracks.size(), m_playlist ? T("titler") : T("spor"), min);
        meta += (meta.empty() ? "" : " \xC2\xB7 ") + std::string(b);
    }
    gfx::text(x, kTop + 168, meta, {gfx::Medium, 24}, alpha(kText3, m_content.value));

    /* Spill av, Bland, Miks, the artist. */
    /* Glass buttons, the focus drop, then the labels. */
    const std::vector<Button> bs = buttons();
    if (m_in_tracks)
        m_btn_drop.hide();
    for (int pass = 0; pass < 2; pass++) {
        if (pass == 1)
            m_btn_drop.draw(dt, 1.f, &m_animating, 16);
        float bx = x;
        for (int i = 0; i < (int)bs.size(); i++) {
            const bool focus = !m_in_tracks && m_button == i;
            const gfx::TextStyle st{gfx::Bold, 26};
            const std::string label = bs[i] == PlayAll   ? T("Spill av")
                                      : bs[i] == Shuffle ? T("Bland")
                                      : bs[i] == Mix     ? T("Miks")
                                                         : m_album.album_artist + " \xE2\x80\xBA";
            const float bw = std::min(520.f, gfx::text_width(label, st)) + 80;
            const gfx::Rect r{bx, kTop + 230, bw, 76};
            bx += bw + 20;
            if (pass == 0) {
                glass_panel(r, 16, 1.f, false);
                if (focus)
                    m_btn_drop.to(r, (int)bs[i]);
                continue;
            }
            gfx::text(r.x + r.w / 2, r.y + r.h / 2 + 9, label, {focus ? gfx::Bold : gfx::SemiBold, 26, 520}, kText, 1);
        }
    }

    /* Tracks: the list scrolls so the focused one stays in view. */
    const int n = (int)m_tracks.size();
    const int visible = (int)((gfx::H - kListTop - 40) / kRowH);
    m_scroll.to((float)std::max(0, std::min(m_track - visible / 2, n - visible)) * kRowH);
    if (m_scroll.step(dt, 12.f))
        m_animating = true;
    gfx::push_opacity(m_content.value);
    gfx::push_scissor({0, kListTop - 8, gfx::W, gfx::H - kListTop + 8});
    const bool discs = n > 0 && m_tracks.back().parent_index > 1;
    if (m_in_tracks && m_track < n)
        m_track_drop.to({x - 20, kListTop + m_track * kRowH - m_scroll.value, w + 20, kRowH - 6}, m_track, 0,
                        -m_scroll.value);
    else
        m_track_drop.hide();
    m_track_drop.draw(dt, 1.f, &m_animating, 14);
    for (int i = 0; i < n; i++) {
        const float y = kListTop + i * kRowH - m_scroll.value;
        if (y < kListTop - kRowH || y > gfx::H)
            continue;
        const jf::Item &t = m_tracks[i];
        const bool focus = m_in_tracks && i == m_track;
        const uint32_t fg = kText, dim = focus ? kText2 : kText3;
        char num[16];
        if (discs)
            std::snprintf(num, sizeof num, "%d.%d", std::max(1, t.parent_index), std::max(0, t.index));
        else
            std::snprintf(num, sizeof num, "%d", t.index > 0 ? t.index : i + 1);
        gfx::text(x + 30, y + 40, num, {gfx::SemiBold, 22}, dim, 2);
        gfx::text(x + 60, y + 40, t.name, {focus ? gfx::Bold : gfx::Medium, 25, w - 200}, fg);
        gfx::text(x + w - 20, y + 40, duration(t.runtime_ticks), {gfx::Medium, 22}, dim, 2);
    }
    gfx::pop_scissor();
    gfx::pop_opacity();
    if (!m_loaded)
        m_animating = true;
    if (art::animating())
        m_animating = true;
}

} // namespace ui
