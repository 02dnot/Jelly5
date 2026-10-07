/*
 * Jelly5 — Jellyfin for PS5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The translation tables besides English (i18n.cpp), each keyed by the
 * Norwegian text: i18n_<language code>.cpp.
 */
#pragma once

#include <string>
#include <unordered_map>

namespace i18n {

const std::unordered_map<std::string, const char *> &spanish_table();
const std::unordered_map<std::string, const char *> &french_table();
const std::unordered_map<std::string, const char *> &german_table();
const std::unordered_map<std::string, const char *> &portuguese_table();
const std::unordered_map<std::string, const char *> &italian_table();
const std::unordered_map<std::string, const char *> &japanese_table();
const std::unordered_map<std::string, const char *> &dutch_table();
const std::unordered_map<std::string, const char *> &russian_table();
const std::unordered_map<std::string, const char *> &korean_table();
const std::unordered_map<std::string, const char *> &chinese_traditional_table();
const std::unordered_map<std::string, const char *> &chinese_simplified_table();
const std::unordered_map<std::string, const char *> &finnish_table();
const std::unordered_map<std::string, const char *> &swedish_table();
const std::unordered_map<std::string, const char *> &danish_table();
const std::unordered_map<std::string, const char *> &polish_table();
const std::unordered_map<std::string, const char *> &turkish_table();
const std::unordered_map<std::string, const char *> &arabic_table();
const std::unordered_map<std::string, const char *> &czech_table();
const std::unordered_map<std::string, const char *> &hungarian_table();
const std::unordered_map<std::string, const char *> &greek_table();
const std::unordered_map<std::string, const char *> &romanian_table();
const std::unordered_map<std::string, const char *> &thai_table();
const std::unordered_map<std::string, const char *> &vietnamese_table();
const std::unordered_map<std::string, const char *> &indonesian_table();
const std::unordered_map<std::string, const char *> &ukrainian_table();

} // namespace i18n
