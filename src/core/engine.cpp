// SPDX-License-Identifier: BSD-3-Clause
#include "x52/engine.hpp"

#include <linux/input.h>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <sstream>

#include "x52/mfd.hpp"
#include "x52/uinput.hpp"

namespace x52 {
namespace {

std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    out.push_back(cur);
    return out;
}

// Maps a printable ASCII character to a key code and whether shift is needed.
bool char_to_key(char c, uint16_t& code, bool& need_shift) {
    need_shift = false;
    if (c >= 'a' && c <= 'z') {
        static const uint16_t lower[26] = {KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
                                           KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
                                           KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
                                           KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z};
        code = lower[c - 'a'];
        return true;
    }
    if (c >= 'A' && c <= 'Z') {
        if (!char_to_key(static_cast<char>(c - 'A' + 'a'), code, need_shift)) return false;
        need_shift = true;
        return true;
    }
    if (c >= '1' && c <= '9') { code = static_cast<uint16_t>(KEY_1 + (c - '1')); return true; }
    if (c == '0') { code = KEY_0; return true; }
    switch (c) {
        case ' ': code = KEY_SPACE; return true;
        case '\n': code = KEY_ENTER; return true;
        case '\t': code = KEY_TAB; return true;
        case '-': code = KEY_MINUS; return true;
        case '=': code = KEY_EQUAL; return true;
        case '[': code = KEY_LEFTBRACE; return true;
        case ']': code = KEY_RIGHTBRACE; return true;
        case ';': code = KEY_SEMICOLON; return true;
        case '\'': code = KEY_APOSTROPHE; return true;
        case '`': code = KEY_GRAVE; return true;
        case '\\': code = KEY_BACKSLASH; return true;
        case ',': code = KEY_COMMA; return true;
        case '.': code = KEY_DOT; return true;
        case '/': code = KEY_SLASH; return true;
        default: break;
    }
    // Shifted symbols.
    switch (c) {
        case '!': code = KEY_1; break;
        case '@': code = KEY_2; break;
        case '#': code = KEY_3; break;
        case '$': code = KEY_4; break;
        case '%': code = KEY_5; break;
        case '^': code = KEY_6; break;
        case '&': code = KEY_7; break;
        case '*': code = KEY_8; break;
        case '(': code = KEY_9; break;
        case ')': code = KEY_0; break;
        case '_': code = KEY_MINUS; break;
        case '+': code = KEY_EQUAL; break;
        case '{': code = KEY_LEFTBRACE; break;
        case '}': code = KEY_RIGHTBRACE; break;
        case ':': code = KEY_SEMICOLON; break;
        case '"': code = KEY_APOSTROPHE; break;
        case '~': code = KEY_GRAVE; break;
        case '|': code = KEY_BACKSLASH; break;
        case '<': code = KEY_COMMA; break;
        case '>': code = KEY_DOT; break;
        case '?': code = KEY_SLASH; break;
        default: return false;
    }
    need_shift = true;
    return true;
}

}  // namespace

std::string format_stopwatch(uint64_t us) {
    uint64_t cs = us / 10000;  // hundredths
    uint64_t minutes = cs / 6000;
    uint64_t seconds = (cs / 100) % 60;
    uint64_t hundredths = cs % 100;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu.%02llu",
                  static_cast<unsigned long long>(minutes),
                  static_cast<unsigned long long>(seconds),
                  static_cast<unsigned long long>(hundredths));
    return buf;
}

bool Macro::parse(const std::vector<std::string>& steps, std::vector<MacroStep>& out,
                  std::string& error) {
    out.clear();
    for (const auto& raw : steps) {
        std::string step = trim(raw);
        if (step.empty()) continue;

        auto colon = step.find(':');
        std::string prefix = colon == std::string::npos ? std::string() : step.substr(0, colon);
        std::string arg = colon == std::string::npos ? std::string() : step.substr(colon + 1);
        std::string plower = prefix;
        std::transform(plower.begin(), plower.end(), plower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

        if (plower == "delay" || plower == "wait") {
            MacroStep s;
            s.kind = MacroStep::Kind::Delay;
            s.delay_ms = std::atoi(arg.c_str());
            if (s.delay_ms < 0) s.delay_ms = 0;
            out.push_back(std::move(s));
            continue;
        }
        if (plower == "text") {
            MacroStep s;
            s.kind = MacroStep::Kind::Text;
            s.text = arg;
            out.push_back(std::move(s));
            continue;
        }
        if (plower == "down" || plower == "up" || plower == "key" || plower == "tap") {
            std::string name = trim(arg);
            uint16_t code = keycode_from_name(name);
            if (code == 0) {
                error = "unknown key '" + name + "' in macro step '" + step + "'";
                return false;
            }
            MacroStep s;
            s.kind = plower == "down" ? MacroStep::Kind::Down
                     : plower == "up" ? MacroStep::Kind::Up
                                      : MacroStep::Kind::Tap;
            s.keys.push_back(code);
            out.push_back(std::move(s));
            continue;
        }

        // "ctrl+shift+t" chord
        auto parts = split(step, '+');
        MacroStep s;
        s.kind = MacroStep::Kind::Tap;
        for (const auto& part : parts) {
            std::string name = trim(part);
            if (name.empty()) continue;
            uint16_t code = keycode_from_name(name);
            if (code == 0) {
                error = "unknown key '" + name + "' in macro step '" + step + "'";
                return false;
            }
            s.keys.push_back(code);
        }
        if (s.keys.empty()) {
            error = "empty macro step";
            return false;
        }
        out.push_back(std::move(s));
    }
    return true;
}

void Macro::schedule(const std::vector<MacroStep>& steps, uint64_t now_us,
                     std::vector<std::pair<uint64_t, std::pair<uint16_t, bool>>>& out) {
    uint64_t t = now_us;
    for (const auto& step : steps) {
        switch (step.kind) {
            case MacroStep::Kind::Delay:
                t += static_cast<uint64_t>(step.delay_ms) * 1000;
                break;

            case MacroStep::Kind::Text: {
                for (char c : step.text) {
                    uint16_t code = 0;
                    bool need_shift = false;
                    if (!char_to_key(c, code, need_shift)) {
                        t += 20000;
                        continue;
                    }
                    if (need_shift) out.emplace_back(t, std::make_pair(KEY_LEFTSHIFT, true));
                    out.emplace_back(t + 2000, std::make_pair(code, true));
                    out.emplace_back(t + 12000, std::make_pair(code, false));
                    if (need_shift) out.emplace_back(t + 14000, std::make_pair(KEY_LEFTSHIFT, false));
                    t += 22000;
                }
                break;
            }

            case MacroStep::Kind::Down:
                for (uint16_t code : step.keys) out.emplace_back(t, std::make_pair(code, true));
                break;

            case MacroStep::Kind::Up:
                for (uint16_t code : step.keys) out.emplace_back(t, std::make_pair(code, false));
                break;

            case MacroStep::Kind::Tap: {
                for (uint16_t code : step.keys) out.emplace_back(t, std::make_pair(code, true));
                for (auto it = step.keys.rbegin(); it != step.keys.rend(); ++it) {
                    out.emplace_back(t + 15000, std::make_pair(*it, false));
                }
                t += 20000;
                break;
            }
        }
    }
}

Engine::Engine(OutputSink* sink) : sink_(sink) { last_axis_.fill(-1); }

void Engine::set_profile(const Profile& p) {
    // Release anything a running macro may have left down.
    if (sink_) {
        for (uint16_t code : held_keys_) sink_->key(code, false);
        if (!held_keys_.empty()) sink_->sync();
    }
    held_keys_.clear();
    scheduled_.clear();

    profile_ = p;
    page_ = p.settings.mfd_page;
    last_axis_.fill(-1);  // force a full axis refresh
    last_hat_ = -1;
    last_thumb_x_ = -1;
    last_thumb_y_ = -1;
    mode_ = std::clamp(mode_, 1, kModeCount);  // keep the current mode
    shift_ = false;
    last_buttons_.fill(false);
    if (on_page_changed) on_page_changed(page_);
    if (on_mode_changed) on_mode_changed(mode_);
}

void Engine::set_mode(int mode) {
    mode = std::clamp(mode, 1, kModeCount);
    if (mode == mode_) return;
    mode_ = mode;
    if (on_mode_changed) on_mode_changed(mode_);
}

void Engine::set_shift(bool on) {
    if (shift_ == on) return;
    shift_ = on;
    if (on_shift_changed) on_shift_changed(shift_);
}

void Engine::set_page(MfdPage page) {
    if (page_ == page) return;
    page_ = page;
    if (on_page_changed) on_page_changed(page_);
}

void Engine::next_page() {
    int p = (static_cast<int>(page_) + 1) % 6;
    set_page(static_cast<MfdPage>(p));
}

void Engine::set_custom_text(const std::array<std::string, kMfdLineCount>& text) {
    profile_.settings.custom_text = text;
}

bool Engine::current_leds(std::array<LedColor, static_cast<std::size_t>(Led::Count)>& out) const {
    const ModeLayer& l = profile_.layer(mode_);
    if (!l.has_leds) return false;
    out = l.leds;
    return true;
}

void Engine::stopwatch_start(uint64_t now_us) {
    if (sw_running_) return;
    sw_running_ = true;
    sw_started_us_ = now_us;
}

void Engine::stopwatch_stop(uint64_t now_us) {
    if (!sw_running_) return;
    sw_accum_us_ += now_us - sw_started_us_;
    sw_running_ = false;
}

void Engine::stopwatch_reset() {
    sw_running_ = false;
    sw_accum_us_ = 0;
    sw_started_us_ = 0;
}

uint64_t Engine::stopwatch_elapsed_us(uint64_t now_us) const {
    return sw_accum_us_ + (sw_running_ ? now_us - sw_started_us_ : 0);
}

void Engine::start_macro(const Action& a) {
    std::vector<MacroStep> steps;
    std::string error;
    if (!Macro::parse(a.macro, steps, error)) return;
    uint64_t base = now_us_;
    if (!scheduled_.empty() && scheduled_.back().first > base) base = scheduled_.back().first;
    Macro::schedule(steps, base, scheduled_);
    std::sort(scheduled_.begin(), scheduled_.end(),
              [](const auto& x, const auto& y) { return x.first < y.first; });
}

void Engine::dispatch(const Action& a, bool pressed, Button /*button*/) {
    if (!sink_) return;
    switch (a.type) {
        case ActionType::None:
            break;
        case ActionType::Passthrough:
            break;  // handled by the caller
        case ActionType::Key: {
            uint16_t code = keycode_from_name(a.key);
            if (code) {
                sink_->key(code, pressed);
                sink_->sync();
            }
            break;
        }
        case ActionType::Mouse:
            sink_->mouse_button(static_cast<uint16_t>(BTN_LEFT + static_cast<int>(a.mouse)), pressed);
            sink_->sync();
            break;
        case ActionType::Wheel:
            if (pressed) {
                sink_->wheel(a.wheel_delta);
                sink_->sync();
            }
            break;
        case ActionType::Macro:
            if (pressed) start_macro(a);
            break;
        case ActionType::Mode:
            if (pressed) set_mode(a.mode);
            break;
        case ActionType::MfdPage:
            if (pressed) set_page(a.page);
            break;
        case ActionType::Exec:
            if (pressed) sink_->exec(a.command);
            break;
        case ActionType::Button:
            if (a.button >= 0 && a.button < static_cast<int>(kButtonCount)) {
                sink_->button(a.button, pressed);
            }
            break;
    }
}

void Engine::process(const InputState& cur, const InputState& /*prev*/) {
    // --- shift sources -----------------------------------------------------
    bool shift_src = false;
    if (profile_.settings.shift_on_pinky && cur.button(Button::Shift)) shift_src = true;
    if (profile_.settings.clutch_as_shift && cur.button(Button::Clutch)) shift_src = true;
    set_shift(shift_src);

    // --- mode buttons ------------------------------------------------------
    for (int m = 0; m < kModeCount; ++m) {
        auto b = static_cast<Button>(static_cast<int>(Button::Mode1) + m);
        bool now = cur.button(b);
        bool before = last_buttons_[static_cast<std::size_t>(b)];
        if (now && !before) set_mode(m + 1);
    }

    // --- hardwired MFD controls (unless the profile binds them) ------------
    auto bound = [&](Button b) {
        const ModeLayer& l = profile_.layer(mode_);
        const Action& a = shift_ ? l.shift_buttons[static_cast<std::size_t>(b)]
                                 : l.buttons[static_cast<std::size_t>(b)];
        return a.type != ActionType::Passthrough;
    };

    auto edge = [&](Button b) -> int {
        bool now = cur.button(b);
        bool before = last_buttons_[static_cast<std::size_t>(b)];
        if (now == before) return 0;
        return now ? 1 : -1;
    };

    if (!bound(Button::MfdSelect)) {
        if (edge(Button::MfdSelect) == 1) next_page();
    }
    if (!bound(Button::Function)) {
        int e = edge(Button::Function);
        if (e == 1) {
            set_page(page_ == MfdPage::Clock ? MfdPage::Stopwatch : MfdPage::Clock);
        }
    }
    if (!bound(Button::StartStop)) {
        if (edge(Button::StartStop) == 1) {
            if (sw_running_) {
                stopwatch_stop(now_us_);
            } else {
                stopwatch_start(now_us_);
            }
        }
    }
    if (!bound(Button::Reset)) {
        if (edge(Button::Reset) == 1) stopwatch_reset();
    }

    // --- everything else ---------------------------------------------------
    const ModeLayer& layer = profile_.layer(mode_);
    for (std::size_t i = 0; i < kButtonCount; ++i) {
        auto b = static_cast<Button>(i);
        if (b == Button::Mode1 || b == Button::Mode2 || b == Button::Mode3) {
            last_buttons_[i] = cur.buttons[i];
            continue;
        }
        if (b == Button::Shift) {
            last_buttons_[i] = cur.buttons[i];
            continue;  // consumed as the shift modifier
        }
        if (b == Button::Clutch && profile_.settings.clutch_as_shift) {
            last_buttons_[i] = cur.buttons[i];
            continue;
        }

        bool now = cur.buttons[i];
        bool before = last_buttons_[i];
        last_buttons_[i] = now;
        if (now == before) continue;

        Action action = shift_ ? layer.shift_buttons[i] : layer.buttons[i];
        if (shift_ && action.type == ActionType::Passthrough) {
            // Unset shift bindings fall through to the base layer, so users
            // only describe what actually differs.
            action = layer.buttons[i];
        }

        if (action.type == ActionType::Passthrough) {
            if (sink_) sink_->button(static_cast<int>(i), now);
        } else {
            dispatch(action, now, b);
        }
    }

    apply_axis(cur);
    apply_hat(cur);
    apply_thumb(cur);
}

void Engine::apply_axis(const InputState& st) {
    if (!sink_) return;
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        auto a = static_cast<Axis>(i);
        int raw = st.axis[i];
        int max = (a == Axis::X || a == Axis::Y || a == Axis::Twist) ? 1023 : 255;
        const AxisConfig& cfg = profile_.axes[i];
        int value = raw;
        if (cfg.center != 0.0 || cfg.deadzone > 0.0 || cfg.saturation < 1.0 || cfg.invert ||
            cfg.curve != 1.0) {
            double norm = static_cast<double>(raw) / static_cast<double>(max);
            double shaped = shape_axis(norm, cfg);
            value = static_cast<int>(shaped * max + 0.5);
        }
        // Only push real changes: uinput traffic is the hot path.
        if (value == last_axis_[i]) continue;
        last_axis_[i] = value;
        sink_->axis(static_cast<int>(i), value);
    }
}

void Engine::apply_hat(const InputState& st) {
    if (!sink_) return;
    if (st.hat == last_hat_) return;
    last_hat_ = st.hat;
    sink_->hat(st.hat);
}

// The 4-bit ministick has no evdev axis on the physical device, so it is
// forwarded as a second hat: centre 6..10, else -1/0/1 per axis.
void Engine::apply_thumb(const InputState& st) {
    if (!sink_) return;
    if (st.thumb_x == last_thumb_x_ && st.thumb_y == last_thumb_y_) return;
    last_thumb_x_ = st.thumb_x;
    last_thumb_y_ = st.thumb_y;

    auto axis = [](int v) { return v <= 6 ? -1 : (v >= 10 ? 1 : 0); };
    sink_->thumb(axis(st.thumb_x), axis(st.thumb_y));
}

void Engine::tick(uint64_t now_us) {
    if (scheduled_.empty()) return;

    std::vector<std::pair<uint64_t, std::pair<uint16_t, bool>>> due;
    auto it = scheduled_.begin();
    while (it != scheduled_.end() && it->first <= now_us) {
        due.push_back(*it);
        ++it;
    }
    scheduled_.erase(scheduled_.begin(), it);

    if (!sink_) return;
    for (const auto& ev : due) {
        sink_->key(ev.second.first, ev.second.second);
        if (ev.second.second) {
            held_keys_.push_back(ev.second.first);
        } else {
            held_keys_.erase(std::remove(held_keys_.begin(), held_keys_.end(), ev.second.first),
                             held_keys_.end());
        }
    }
    if (!due.empty()) sink_->sync();
}

std::array<std::string, kMfdLineCount> Engine::render_mfd(uint64_t now_us) const {
    std::array<std::string, kMfdLineCount> lines{{"", "", ""}};
    const ModeLayer& layer = profile_.layer(mode_);
    char buf[32];

    auto mode_tag = [&]() {
        std::string name = layer.name.empty() ? "MODE" : layer.name;
        return "M" + std::to_string(mode_) + " " + name;
    };

    switch (page_) {
        case MfdPage::Clock: {
            std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_r(&now, &tm);
            lines[0] = profile_.settings.mfd_clock_label;
            std::snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d",
                          profile_.settings.clock_24h ? "TIME" : "TIME",
                          tm.tm_hour, tm.tm_min, tm.tm_sec);
            lines[1] = buf;
            std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1,
                          tm.tm_mday);
            lines[2] = std::string("DATE ") + buf;
            break;
        }
        case MfdPage::Stopwatch: {
            lines[0] = "STOPWATCH";
            lines[1] = format_stopwatch(stopwatch_elapsed_us(now_us));
            lines[2] = sw_running_ ? "RUNNING" : "STOPPED";
            break;
        }
        case MfdPage::Status: {
            if (layer.has_mfd) {
                lines[0] = layer.mfd_lines[0];
                lines[1] = layer.mfd_lines[1];
                lines[2] = layer.mfd_lines[2].empty() ? mode_tag() : layer.mfd_lines[2];
            } else {
                lines[0] = profile_.name;
                lines[1] = mode_tag();
                lines[2] = shift_ ? "SHIFT LAYER" : "SHIFT OFF";
            }
            break;
        }
        case MfdPage::Custom: {
            lines = profile_.settings.custom_text;
            break;
        }
        case MfdPage::Telemetry: {
            // The daemon replaces these with the live template rendering.
            lines[0] = "GAME";
            lines[1] = "NO TELEMETRY";
            lines[2] = "";
            break;
        }
        case MfdPage::Profile: {
            lines[0] = profile_.name;
            lines[1] = mode_tag();
            lines[2] = profile_.description.empty() ? "X52 PRO LINUX" : profile_.description;
            break;
        }
    }

    for (auto& l : lines) l = mfd_sanitize(l, kMfdLineWidth);
    for (auto& l : lines) {
        while (!l.empty() && l.back() == ' ') l.pop_back();
    }
    return lines;
}

}  // namespace x52
