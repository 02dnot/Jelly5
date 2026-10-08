/*
 * Nuvio PS5
 * Copyright (C) 2026 Husam Osman
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once
/*
 * The contract between the Nuvio Player and its interface: the status the
 * player feeds the interface every frame, and the commands (pause, seek, pick
 * a track, ...) the interface answers with. Jelly5's interface is
 * ui::PlayerUi (src/ui/player_ui.*); Nuvio's own drawing of it is gone.
 */
#include "nuvio_input.h"
#include "nuvio_session.h"
#include "ui_canvas.h"

#include <string>
#include <vector>

struct NuvioAudioTrack {
    int stream = -1;
    std::string lang;        /* "eng" */
    std::string title;       /* stream title, e.g. "Commentary" */
    std::string codec;       /* "TrueHD", "E-AC-3", ... */
    std::string channels;    /* "7.1", "5.1", "Stereo" */
    bool is_default = false;
};

struct NuvioStatus {
    double now = 0;               /* monotonic seconds */
    bool started = false;         /* the first frame is on screen */
    float open_progress = 0;      /* 0..1 before the first frame */
    const char *open_stage = "";  /* what the open is doing, for the loading screen */
    bool buffering = false;       /* stalled after start */
    bool paused = false;
    double position = 0, duration = 0, buffered = 0;
    std::vector<NuvioAudioTrack> audio;
    int audio_active = -1;        /* index into audio */
    int view_mode = 0;            /* 0 fit, 1 fill, 2 stretch */
    std::string quality_line;     /* "2160p · HDR10 · HEVC · TrueHD 7.1" */
    std::string error;            /* non-empty: the stream failed */
    bool switching = false;       /* reopening for an audio / source change */
    /* Jelly5: the L3 playback info panel, refreshed once a second while it is up.
     * Rows of label and value; a label starting with '#' is a section heading. */
    std::vector<std::pair<std::string, std::string>> stats;
};

enum class OsdCmd {
    TogglePause,
    SeekTo,          /* value = seconds */
    Stop,            /* back to Nuvio */
    SelectAudio,     /* index into status.audio */
    SelectSubtitle,  /* index = track id, -1 off */
    SubtitleDelay,   /* value = ms */
    SubtitleStyle,   /* style changed (read nuvio_subs_get_style) */
    SwitchSource,    /* index into request.sources */
    PlayEpisode,     /* season, episode */
    PlayNext,
    SetViewMode,     /* index = mode */
};

struct OsdCommand {
    OsdCmd cmd;
    double value = 0;
    int index = -1;
    int season = 0, episode = 0;
    std::string video_id;   /* Jelly5: PlayEpisode's item (numbers can be missing or repeat) */
};

/* "English" for "eng"/"en"/"en-US"; the code itself when unknown. */
std::string nuvio_language_name(const std::string &code);
/* ISO 639-1 form of a 2- or 3-letter code ("eng" -> "en"), lower case. */
std::string nuvio_language_key(const std::string &code);
