// SPDX-License-Identifier: BSD-3-Clause
//
// Mode / profile engine.  Pure logic: it consumes input states, resolves the
// action bound to each button for the active mode and shift layer, and pushes
// the result to an OutputSink (uinput in production, a recorder in tests).
//
#ifndef X52_ENGINE_HPP
#define X52_ENGINE_HPP

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "x52/input.hpp"
#include "x52/profile.hpp"

namespace x52 {

class OutputSink {
public:
    virtual ~OutputSink() = default;
    virtual void key(uint16_t code, bool pressed) = 0;
    virtual void mouse_button(uint16_t button, bool pressed) = 0;
    virtual void wheel(int delta) = 0;
    virtual void axis(int index, int value) = 0;
    virtual void button(int index, bool pressed) = 0;
    virtual void hat(int direction) = 0;
    virtual void sync() = 0;
    virtual void exec(const std::string& command) { (void)command; }
};

struct MacroStep {
    enum class Kind { Tap, Down, Up, Text, Delay } kind = Kind::Tap;
    std::vector<uint16_t> keys;  // modifiers first, key last
    std::string text;
    int delay_ms = 0;
};

class Macro {
public:
    static bool parse(const std::vector<std::string>& steps, std::vector<MacroStep>& out,
                      std::string& error);
    // Expands a step list into key events scheduled relative to `now_us`.
    static void schedule(const std::vector<MacroStep>& steps, uint64_t now_us,
                         std::vector<std::pair<uint64_t, std::pair<uint16_t, bool>>>& out);
};

class Engine {
public:
    explicit Engine(OutputSink* sink = nullptr);
    void set_sink(OutputSink* sink) { sink_ = sink; }

    void set_profile(const Profile& p);
    // The engine owns the live profile; callers edit it in place through this
    // reference so there is exactly one source of truth.
    const Profile& profile() const noexcept { return profile_; }
    Profile& profile_mut() noexcept { return profile_; }

    void set_mode(int mode);
    int mode() const noexcept { return mode_; }

    bool shift_active() const noexcept { return shift_; }
    void set_shift(bool on);

    void set_page(MfdPage page);
    MfdPage page() const noexcept { return page_; }
    void next_page();

    void set_custom_text(const std::array<std::string, kMfdLineCount>& text);

    // Monotonic timestamp used for macros and the stopwatch.
    void set_now(uint64_t now_us) { now_us_ = now_us; }

    // Feeds a new input state.  `prev` is the previous state, used for edges.
    void process(const InputState& current, const InputState& prev);

    // Runs scheduled macro events and updates the stopwatch.
    void tick(uint64_t now_us);

    // Rendering (pure): returns the three MFD lines for the current page.
    std::array<std::string, kMfdLineCount> render_mfd(uint64_t now_us) const;

    // Callbacks used by the daemon to react to state changes.
    std::function<void(int)> on_mode_changed;
    std::function<void(MfdPage)> on_page_changed;
    std::function<void(bool)> on_shift_changed;

    // Stopwatch.
    void stopwatch_start(uint64_t now_us);
    void stopwatch_stop(uint64_t now_us);
    void stopwatch_reset();
    bool stopwatch_running() const noexcept { return sw_running_; }
    uint64_t stopwatch_elapsed_us(uint64_t now_us) const;

    // Currently active LED theme (per mode), empty when the mode has none.
    bool current_leds(std::array<LedColor, static_cast<std::size_t>(Led::Count)>& out) const;

private:
    void dispatch(const Action& a, bool pressed, Button button);
    void start_macro(const Action& a);
    void apply_axis(const InputState& st);
    void apply_hat(const InputState& st);

    OutputSink* sink_ = nullptr;
    Profile profile_ = Profile::make_default();
    int mode_ = 1;
    bool shift_ = false;
    MfdPage page_ = MfdPage::Status;

    // Macro playback.
    std::vector<std::pair<uint64_t, std::pair<uint16_t, bool>>> scheduled_;
    std::vector<uint16_t> held_keys_;

    // Stopwatch.
    bool sw_running_ = false;
    uint64_t sw_started_us_ = 0;
    uint64_t sw_accum_us_ = 0;

    std::array<bool, kButtonCount> last_buttons_{};
    std::array<int, kAxisCount> last_axis_{};
    int last_hat_ = -1;
    uint64_t now_us_ = 0;
};

// Formats milliseconds as MM:SS.cc
std::string format_stopwatch(uint64_t us);

}  // namespace x52

#endif  // X52_ENGINE_HPP
