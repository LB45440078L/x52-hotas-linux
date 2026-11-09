// SPDX-License-Identifier: GPL-2.0-or-later
#include "x52/mfd.hpp"

#include <algorithm>
#include <cstring>

namespace x52 {
namespace {

// Decodes one UTF-8 sequence; returns the code point and advances `i`.
uint32_t next_codepoint(std::string_view s, std::size_t& i) noexcept {
    unsigned char c = static_cast<unsigned char>(s[i]);
    if (c < 0x80) { ++i; return c; }

    std::size_t extra = 0;
    uint32_t cp = 0;
    if ((c & 0xe0) == 0xc0) { extra = 1; cp = c & 0x1f; }
    else if ((c & 0xf0) == 0xe0) { extra = 2; cp = c & 0x0f; }
    else if ((c & 0xf8) == 0xf0) { extra = 3; cp = c & 0x07; }
    else { ++i; return 0xfffd; }

    if (i + extra >= s.size()) { i = s.size(); return 0xfffd; }
    for (std::size_t k = 1; k <= extra; ++k) {
        unsigned char cc = static_cast<unsigned char>(s[i + k]);
        if ((cc & 0xc0) != 0x80) { ++i; return 0xfffd; }
        cp = (cp << 6) | (cc & 0x3f);
    }
    i += extra + 1;
    return cp;
}

}  // namespace

char mfd_fold_codepoint(uint32_t cp) noexcept {
    if (cp < 0x80) {
        char c = static_cast<char>(cp);
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        if (cp >= 0x20 && cp <= 0x5f) return c;
        // Characters outside the display's cell set: fold what has an
        // obvious ASCII stand-in, otherwise show a placeholder.
        switch (cp) {
            case '\t': return ' ';
            case 0x7f: return ' ';
            default: break;
        }
        if (cp >= 0x60 && cp <= 0x7e) {
            // Lowercase block already handled; remaining punctuation renders.
            return c;
        }
        return '?';
    }

    // Latin-1 supplement and common typographic characters.
    switch (cp) {
        case 0x00a0: return ' ';
        case 0x00b0: return 'O';   // degree
        case 0x00b1: return '+';   // plus-minus
        case 0x00b7: return '.';
        case 0x00d7: return 'x';
        case 0x00f7: return '/';
        case 0x2013:
        case 0x2014: return '-';
        case 0x2018:
        case 0x2019: return '\'';
        case 0x201c:
        case 0x201d: return '"';
        case 0x2026: return '.';
        case 0x20ac: return 'E';
        default: break;
    }

    // Strip accents from the Latin-1 range: à..å -> A, ç -> C, è..ë -> E, ...
    if ((cp >= 0x00c0 && cp <= 0x00c5) || (cp >= 0x00e0 && cp <= 0x00e5)) return 'A';
    if (cp == 0x00c7 || cp == 0x00e7) return 'C';
    if ((cp >= 0x00c8 && cp <= 0x00cb) || (cp >= 0x00e8 && cp <= 0x00eb)) return 'E';
    if ((cp >= 0x00cc && cp <= 0x00cf) || (cp >= 0x00ec && cp <= 0x00ef)) return 'I';
    if (cp == 0x00d1 || cp == 0x00f1) return 'N';
    if ((cp >= 0x00d2 && cp <= 0x00d6) || (cp >= 0x00f2 && cp <= 0x00f6)) return 'O';
    if (cp == 0x00d9 || cp == 0x00da || cp == 0x00db || cp == 0x00dc ||
        cp == 0x00f9 || cp == 0x00fa || cp == 0x00fb || cp == 0x00fc) return 'U';
    if (cp == 0x00dd || cp == 0x00fd || cp == 0x00ff) return 'Y';
    if (cp == 0x00df) return 'S';

    return '?';
}

std::string mfd_sanitize(std::string_view utf8, std::size_t width) {
    std::string out;
    out.reserve(width);
    std::size_t i = 0;
    while (i < utf8.size() && out.size() < width) {
        out.push_back(mfd_fold_codepoint(next_codepoint(utf8, i)));
    }
    while (out.size() < width) out.push_back(' ');
    return out;
}

void Mfd::clear_local() {
    for (auto& l : lines_) l.fill(' ');
    dirty_.fill(true);
}

bool Mfd::set_line(std::size_t line, std::string_view text) {
    if (line >= kMfdLineCount) return false;
    std::string s = mfd_sanitize(text, kMfdLineWidth);
    bool changed = false;
    for (std::size_t i = 0; i < kMfdLineWidth; ++i) {
        if (lines_[line][i] != s[i]) { changed = true; break; }
    }
    if (!changed) return false;
    std::copy(s.begin(), s.end(), lines_[line].begin());
    dirty_[line] = true;
    return true;
}

bool Mfd::set_lines(std::string_view l1, std::string_view l2, std::string_view l3) {
    bool a = set_line(0, l1);
    bool b = set_line(1, l2);
    bool c = set_line(2, l3);
    return a || b || c;
}

const std::array<char, kMfdLineWidth>& Mfd::line(std::size_t line) const {
    static const std::array<char, kMfdLineWidth> empty{};
    return line < kMfdLineCount ? lines_[line] : empty;
}

std::string Mfd::line_string(std::size_t line) const {
    if (line >= kMfdLineCount) return {};
    std::string s(lines_[line].begin(), lines_[line].end());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void Mfd::set_brightness(uint8_t value) {
    if (brightness_ == value) return;
    brightness_ = value;
    brightness_dirty_ = true;
}

void Mfd::mark_all_dirty() {
    dirty_.fill(true);
    brightness_dirty_ = true;
}

void Mfd::mark_all_clean() {
    dirty_.fill(false);
    brightness_dirty_ = false;
}

void Mfd::encode_pair(std::size_t line, std::size_t index, uint16_t& out) const noexcept {
    uint8_t lo = static_cast<uint8_t>(lines_[line][index]);
    uint8_t hi = static_cast<uint8_t>(lines_[line][index + 1]);
    out = static_cast<uint16_t>(lo | (hi << 8));
}

DeviceError Mfd::flush(Transport& dev) {
    if (!dev.is_open()) return DeviceError::Disconnected;

    static constexpr uint16_t kBase[kMfdLineCount] = {CmdMfdLine1, CmdMfdLine2, CmdMfdLine3};

    if (brightness_dirty_) {
        DeviceError e = dev.send(CmdMfdBrightness, brightness_);
        if (e != DeviceError::None) return e;
        brightness_dirty_ = false;
    }

    for (std::size_t l = 0; l < kMfdLineCount; ++l) {
        if (!dirty_[l]) continue;

        DeviceError e = dev.send(static_cast<uint16_t>(kBase[l] | CmdMfdClearMask), 0);
        if (e != DeviceError::None) return e;

        for (std::size_t i = 0; i < kMfdLineWidth; i += 2) {
            uint16_t value = 0;
            encode_pair(l, i, value);
            e = dev.send(static_cast<uint16_t>(kBase[l] | 0x00), value);
            if (e != DeviceError::None) return e;
        }
        dirty_[l] = false;
    }
    return DeviceError::None;
}

}  // namespace x52
