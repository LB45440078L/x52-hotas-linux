// SPDX-License-Identifier: BSD-3-Clause
//
// x52panel - GTK4 / libadwaita control panel for the X52 Pro.
//
// A thin client of x52d: the daemon owns the device, the panel renders state
// and sends commands.  Layout follows the system-settings pattern: sidebar
// navigation, grouped cards, large page titles.
//
#include <adwaita.h>
#include <gtk/gtk.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

#include <glib-unix.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "x52/client.hpp"
#include "x52/profile.hpp"
#include "x52/protocol.hpp"
#include "x52/settings.hpp"

using json = nlohmann::json;
using namespace x52;

namespace {

constexpr int kPollMs = 250;

const char* kActionNames[] = {"Passthrough", "Swallow", "Keyboard key", "Mouse button",
                              "Mouse wheel", "Macro", "Switch mode", "MFD page",
                              "Run command", "Joystick button", nullptr};
const char* kActionIds[] = {"passthrough", "none", "key", "mouse", "wheel",
                            "macro", "mode", "mfd_page", "exec", "button", nullptr};
const char* kPageNames[] = {"Clock", "Stopwatch", "Status", "Custom text", "Profile",
                            "Telemetry", nullptr};
const char* kPageIds[] = {"clock", "stopwatch", "status", "custom", "profile", "telemetry",
                          nullptr};
const char* kTelemetrySources[] = {"Disabled", "War Thunder", nullptr};
const char* kTelemetrySourceIds[] = {"none", "warthunder", nullptr};
const char* kRuleLedNames[] = {"fire", "a", "b", "d", "e", "t1", "t2", "t3", "pov", "clutch",
                               "throttle", nullptr};
const char* kRuleColors[] = {"off", "red", "amber", "green", nullptr};
const char* kAxisLabels[] = {"Roll (X)", "Pitch (Y)", "Yaw (twist)", "Throttle",
                             "Rotary X", "Rotary Y", "Slider", nullptr};
const char* kAxisIds[] = {"x", "y", "twist", "throttle", "rotary-x", "rotary-y", "slider",
                          nullptr};
// Zero-based axis index as the kernel and games enumerate it (X Y Z RX RY RZ
// THROTTLE).  Games show these 1-based in their dropdowns, so add one.
const int kAxisGameIds[] = {0, 1, 5, 2, 3, 4, 6};

struct App {
    GtkApplication* app = nullptr;
    GtkWindow* window = nullptr;
    GtkWidget* stack = nullptr;
    GtkWidget* window_subtitle = nullptr;

    GtkWidget* pill = nullptr;
    GtkWidget* mfd_lines[3] = {nullptr, nullptr, nullptr};
    GtkWidget* mode_buttons[3] = {nullptr, nullptr, nullptr};
    GtkWidget* page_drop = nullptr;

    GtkWidget* axis_bars[kAxisCount] = {nullptr};
    GtkWidget* axis_values[kAxisCount] = {nullptr};

    GtkWidget* text_entries[3] = {nullptr, nullptr, nullptr};
    GtkWidget* mfd_bright = nullptr;
    GtkWidget* led_bright = nullptr;
    GtkWidget* clock24 = nullptr;
    GtkWidget* offs2 = nullptr;
    GtkWidget* offs3 = nullptr;
    GtkWidget* stopwatch_label = nullptr;

    GtkWidget* led_dots[static_cast<int>(Led::Count)] = {nullptr};

    GtkWidget* layer_mode_drop = nullptr;
    GtkWidget* shift_switch = nullptr;
    GtkWidget* buttons_box = nullptr;
    GtkWidget* btn_action[kButtonCount] = {nullptr};
    GtkWidget* btn_arg[kButtonCount] = {nullptr};

    GtkWidget* button_pills[kButtonCount] = {nullptr};
    GtkWidget* stick_area = nullptr;
    GtkWidget* throttle_area = nullptr;
    GtkWidget* hat_area = nullptr;
    GtkWidget* mini_area = nullptr;
    GtkWidget* hat_label = nullptr;
    GtkWidget* mini_label = nullptr;
    int pov_hat = 0;
    int thumb_x = 8;
    int thumb_y = 8;
    std::string hat_text;
    std::string mini_text;
    GtkWidget* drift_button = nullptr;
    GtkWidget* drift_detail = nullptr;
    GtkWidget* checks_box = nullptr;
    GtkWidget* checks_pill = nullptr;
    GtkWidget* feel_drop = nullptr;
    GtkWidget* feel_deadzone = nullptr;
    GtkWidget* feel_curve = nullptr;

    GtkWidget* profile_list = nullptr;
    GtkWidget* profile_name = nullptr;

    GtkWidget* autostart_switch = nullptr;
    GtkWidget* minimized_switch = nullptr;
    GtkWidget* verify_switch = nullptr;
    GtkWidget* autosave_switch = nullptr;
    GtkWidget* tele_source = nullptr;
    GtkWidget* tele_pill = nullptr;
    GtkWidget* tele_status = nullptr;
    GtkWidget* tele_lines[3] = {nullptr, nullptr, nullptr};
    GtkWidget* tele_values = nullptr;
    GtkWidget* tele_rules_box = nullptr;
    std::vector<GtkWidget*> rule_condition;
    std::vector<GtkWidget*> rule_led;
    std::vector<GtkWidget*> rule_color;

    GtkWidget* link_pill = nullptr;
    GtkWidget* link_detail = nullptr;
    bool start_minimized = false;

    Client client;          // request/response
    Client stream_client;   // push events, non-blocking
    guint stream_watch = 0;
    uint64_t last_frame_us = 0;
    uint64_t last_buttons_mask = ~0ull;
    std::array<double, kAxisCount> last_frac{};
    std::array<int, kAxisCount> last_pct{};
    json profile = json::object();
    json state = json::object();
    double stick_x = 0.5;
    double stick_y = 0.5;
    double throttle = 0.0;
    bool suppress = false;
    guint poll_id = 0;
};

// ---------------------------------------------------------------------------
// styling
// ---------------------------------------------------------------------------
void css_install() {
    const char* css =
        ".sidebar-pane { background-color: alpha(currentColor, 0.035); }\n"
        ".page-title { font-size: 26px; font-weight: 800; letter-spacing: -0.02em; }\n"
        ".page-sub { font-size: 13px; opacity: 0.55; }\n"
        ".card { background-color: alpha(currentColor, 0.05); border-radius: 14px; }\n"
        ".card-head { padding: 14px 18px 2px 18px; }\n"
        ".card-title { font-weight: 700; font-size: 11px; letter-spacing: 0.09em; opacity: 0.55; }\n"
        ".row { padding: 11px 18px; min-height: 30px; }\n"
        ".row-title { font-size: 14px; }\n"
        ".row-sub { font-size: 12px; opacity: 0.55; }\n"
        ".sep { min-height: 1px; background-color: alpha(currentColor, 0.08); margin: 0 18px; }\n"
        ".lcd { background-color: #06140a; border-radius: 12px; padding: 16px 20px; }\n"
        ".lcd-line { color: #7dff92; font-family: monospace; font-size: 20px; letter-spacing: 3px; }\n"
        ".pill { border-radius: 999px; padding: 3px 11px; font-size: 11px; font-weight: 700; }\n"
        ".pill-ok { background-color: alpha(#2ec27e, 0.20); color: #2ec27e; }\n"
        ".pill-warn { background-color: alpha(#e5a50a, 0.22); color: #e5a50a; }\n"
        ".pill-bad { background-color: alpha(#e01b24, 0.22); color: #ff6b6b; }\n"
        ".key-pill { border-radius: 8px; padding: 7px 3px; font-size: 11px; font-weight: 600;\n"
        "            background-color: alpha(currentColor, 0.07); }\n"
        ".key-pill.on { background-color: #2ec27e; color: #04180c; }\n"
        ".key-pill.mode { background-color: alpha(#3584e4, 0.28); }\n"
        ".led-dot { min-width: 10px; min-height: 10px; border-radius: 999px; }\n"
        ".led-off { background-color: alpha(currentColor, 0.25); }\n"
        ".led-red { background-color: #ff453a; }\n"
        ".led-amber { background-color: #ff9f0a; }\n"
        ".led-green { background-color: #32d74b; }\n"
        ".mono { font-family: monospace; }\n"
        ".big-number { font-size: 20px; font-weight: 700; font-family: monospace; }\n";

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// ---------------------------------------------------------------------------
// daemon plumbing
// ---------------------------------------------------------------------------
bool ensure_client(App* a) {
    if (a->client.is_connected()) return true;
    return a->client.connect(default_socket_path(), 600);
}

json call(App* a, const std::string& cmd, const json& args = json::object()) {
    json result;
    std::string error;
    if (!ensure_client(a)) return json{{"__error", "daemon not running"}};
    if (!a->client.request(cmd, args, result, error)) return json{{"__error", error}};
    return result;
}

void toast(App* a, const std::string& message) {
    if (!a->window) return;
    AdwToastOverlay* overlay =
        ADW_TOAST_OVERLAY(g_object_get_data(G_OBJECT(a->window), "overlay"));
    if (!overlay) return;
    adw_toast_overlay_add_toast(overlay, adw_toast_new(message.c_str()));
}

// ---------------------------------------------------------------------------
// builders
// ---------------------------------------------------------------------------
GtkWidget* page_root(const char* title, const char* subtitle, GtkWidget** body_out) {
    GtkWidget* scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroll), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
    gtk_widget_set_margin_top(box, 26);
    gtk_widget_set_margin_bottom(box, 34);
    gtk_widget_set_margin_start(box, 28);
    gtk_widget_set_margin_end(box, 28);

    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    GtkWidget* t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "page-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_box_append(GTK_BOX(head), t);
    if (subtitle) {
        GtkWidget* s = gtk_label_new(subtitle);
        gtk_widget_add_css_class(s, "page-sub");
        gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 70);
        gtk_box_append(GTK_BOX(head), s);
    }
    gtk_box_append(GTK_BOX(box), head);

    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), box);
    *body_out = box;
    return scroll;
}

GtkWidget* card(const char* title, const char* subtitle) {
    GtkWidget* c = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(c, "card");
    if (title) {
        GtkWidget* head = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
        gtk_widget_add_css_class(head, "card-head");
        GtkWidget* t = gtk_label_new(title);
        gtk_widget_add_css_class(t, "card-title");
        gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
        gtk_box_append(GTK_BOX(head), t);
        if (subtitle) {
            GtkWidget* s = gtk_label_new(subtitle);
            gtk_widget_add_css_class(s, "row-sub");
            gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
            gtk_label_set_wrap(GTK_LABEL(s), TRUE);
            gtk_box_append(GTK_BOX(head), s);
        }
        gtk_box_append(GTK_BOX(c), head);
    }
    return c;
}

GtkWidget* row(const char* title, const char* subtitle, GtkWidget* control) {
    GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    gtk_widget_add_css_class(r, "row");
    GtkWidget* text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 1);
    gtk_widget_set_hexpand(text, TRUE);
    GtkWidget* t = gtk_label_new(title);
    gtk_widget_add_css_class(t, "row-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    gtk_box_append(GTK_BOX(text), t);
    if (subtitle) {
        GtkWidget* s = gtk_label_new(subtitle);
        gtk_widget_add_css_class(s, "row-sub");
        gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
        gtk_label_set_wrap(GTK_LABEL(s), TRUE);
        gtk_label_set_max_width_chars(GTK_LABEL(s), 55);
        gtk_box_append(GTK_BOX(text), s);
    }
    gtk_box_append(GTK_BOX(r), text);
    if (control) {
        gtk_widget_set_valign(control, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(r), control);
    }
    return r;
}

void add_row(GtkWidget* c, GtkWidget* r) {
    gtk_box_append(GTK_BOX(c), r);
    GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(sep, "sep");
    gtk_box_append(GTK_BOX(c), sep);
}

GtkWidget* lcd_widget(App* a) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_add_css_class(box, "lcd");
    for (int i = 0; i < 3; ++i) {
        GtkWidget* line = gtk_label_new("----------------");
        gtk_widget_add_css_class(line, "lcd-line");
        gtk_label_set_xalign(GTK_LABEL(line), 0.0f);
        gtk_label_set_width_chars(GTK_LABEL(line), 16);
        gtk_box_append(GTK_BOX(box), line);
        a->mfd_lines[i] = line;
    }
    return box;
}

GtkWidget* slider(int min, int max, int value, GtkWidget** out) {
    GtkWidget* s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1);
    gtk_range_set_value(GTK_RANGE(s), value);
    gtk_scale_set_draw_value(GTK_SCALE(s), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    gtk_widget_set_size_request(s, 200, -1);
    *out = s;
    return s;
}

void set_pill(GtkWidget* label, const char* level, const char* text) {
    gtk_label_set_text(GTK_LABEL(label), text);
    for (const char* c : {"pill-ok", "pill-warn", "pill-bad"}) {
        gtk_widget_remove_css_class(label, c);
    }
    gtk_widget_add_css_class(label, "pill");
    gtk_widget_add_css_class(label, level);
}

// ---------------------------------------------------------------------------
// state rendering
// ---------------------------------------------------------------------------
const char* hat_direction_name(int hat) {
    static const char* names[9] = {"CENTRE", "N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return (hat >= 0 && hat <= 8) ? names[hat] : "--";
}

// Eight direction lamps; the active one lights up.
void draw_hat(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    App* a = static_cast<App*>(data);
    double cx = width / 2.0;
    double cy = height / 2.0;
    double r = std::fmin(width, height) / 2.0 - 10.0;

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.18);
    cairo_set_line_width(cr, 1.4);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);

    for (int i = 1; i <= 8; ++i) {
        double ang = -M_PI / 2.0 + (i - 1) * M_PI / 4.0;
        double mx = cx + std::cos(ang) * r * 0.70;
        double my = cy + std::sin(ang) * r * 0.70;
        bool on = (a->pov_hat == i);
        cairo_set_source_rgba(cr, on ? 0.18 : 0.55, on ? 0.85 : 0.60, on ? 0.45 : 0.65,
                              on ? 1.0 : 0.28);
        cairo_arc(cr, mx, my, on ? 9.0 : 6.0, 0, 2 * M_PI);
        cairo_fill(cr);
    }

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, a->pov_hat == 0 ? 0.55 : 0.15);
    cairo_arc(cr, cx, cy, 4.0, 0, 2 * M_PI);
    cairo_fill(cr);
}

// Throttle ministick: raw 4-bit position on a square pad.
void draw_ministick(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    App* a = static_cast<App*>(data);
    double pad = 10.0;
    double size = std::fmin(width, height) - 2 * pad;

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.10);
    cairo_rectangle(cr, pad, pad, size, size);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.30);
    cairo_set_line_width(cr, 1.0);
    cairo_move_to(cr, pad + size / 2, pad);
    cairo_line_to(cr, pad + size / 2, pad + size);
    cairo_move_to(cr, pad, pad + size / 2);
    cairo_line_to(cr, pad + size, pad + size / 2);
    cairo_stroke(cr);

    // dead zone square
    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.18);
    cairo_rectangle(cr, pad + size * 0.38, pad + size * 0.38, size * 0.24, size * 0.24);
    cairo_stroke(cr);

    double fx = pad + (a->thumb_x / 15.0) * size;
    double fy = pad + (a->thumb_y / 15.0) * size;
    cairo_set_source_rgba(cr, 0.20, 0.85, 0.45, 0.30);
    cairo_arc(cr, fx, fy, 10, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.20, 0.95, 0.50, 1.0);
    cairo_arc(cr, fx, fy, 5, 0, 2 * M_PI);
    cairo_fill(cr);
}

void draw_stick(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    App* a = static_cast<App*>(data);
    double cx = width / 2.0;
    double cy = height / 2.0;
    double r = std::fmin(width, height) / 2.0 - 10.0;

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.18);
    cairo_set_line_width(cr, 1.0);
    cairo_arc(cr, cx, cy, r, 0, 2 * M_PI);
    cairo_stroke(cr);
    cairo_arc(cr, cx, cy, r * 0.55, 0, 2 * M_PI);
    cairo_stroke(cr);

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.35);
    cairo_move_to(cr, cx - r, cy);
    cairo_line_to(cr, cx + r, cy);
    cairo_move_to(cr, cx, cy - r);
    cairo_line_to(cr, cx, cy + r);
    cairo_stroke(cr);

    double px = cx + (a->stick_x * 2.0 - 1.0) * r * 0.92;
    double py = cy - (a->stick_y * 2.0 - 1.0) * r * 0.92;
    cairo_set_source_rgba(cr, 0.20, 0.85, 0.45, 0.35);
    cairo_arc(cr, px, py, 11, 0, 2 * M_PI);
    cairo_fill(cr);
    cairo_set_source_rgba(cr, 0.20, 0.95, 0.50, 1.0);
    cairo_arc(cr, px, py, 6, 0, 2 * M_PI);
    cairo_fill(cr);
}

void draw_throttle(GtkDrawingArea*, cairo_t* cr, int width, int height, gpointer data) {
    App* a = static_cast<App*>(data);
    double pad = 10;
    double track = width - 2 * pad;
    double h = height - 2 * pad;

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.16);
    cairo_rectangle(cr, pad, pad, track, h);
    cairo_fill(cr);

    double fill = h * a->throttle;
    cairo_set_source_rgba(cr, 0.20, 0.85, 0.45, 0.9);
    cairo_rectangle(cr, pad, pad + h - fill, track, fill);
    cairo_fill(cr);

    cairo_set_source_rgba(cr, 0.5, 0.55, 0.6, 0.5);
    cairo_set_line_width(cr, 1.0);
    for (int i = 0; i <= 4; ++i) {
        double y = pad + h * i / 4.0;
        cairo_move_to(cr, pad, y);
        cairo_line_to(cr, pad + track, y);
    }
    cairo_stroke(cr);
}

void refresh_profile(App* a);
void update_state(App* a);
void on_clock_settings(GObject*, GParamSpec*, gpointer data);

void set_button_pills(App* a, uint64_t mask) {
    if (mask == a->last_buttons_mask) return;
    a->last_buttons_mask = mask;
    for (int i = 0; i < static_cast<int>(kButtonCount); ++i) {
        GtkWidget* w = a->button_pills[i];
        if (!w) continue;
        if ((mask >> i) & 1ull) gtk_widget_add_css_class(w, "on");
        else gtk_widget_remove_css_class(w, "on");
    }
}

// High-rate path: raw axis values and a button bitmask, straight from the
// daemon's input loop.  Touches only widgets whose value actually changed.
void apply_input(App* a, const json& d) {
    static constexpr int kMax[kAxisCount] = {1023, 1023, 1023, 255, 255, 255, 255};

    if (d.contains("a") && d["a"].is_array() && d["a"].size() >= kAxisCount) {
        for (std::size_t i = 0; i < kAxisCount && i < d["a"].size(); ++i) {
            double frac = static_cast<double>(d["a"][i].get<int>()) / kMax[i];
            if (frac > 1.0) frac = 1.0;
            if (frac < 0.0) frac = 0.0;

            if (a->axis_bars[i] && std::fabs(frac - a->last_frac[i]) > 0.0015) {
                gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->axis_bars[i]), frac);
                a->last_frac[i] = frac;
            }
            int pct = static_cast<int>(frac * 100.0 + 0.5);
            if (a->axis_values[i] && pct != a->last_pct[i]) {
                char buf[16];
                std::snprintf(buf, sizeof(buf), "%3d%%", pct);
                gtk_label_set_text(GTK_LABEL(a->axis_values[i]), buf);
                a->last_pct[i] = pct;
            }
        }
        a->stick_x = static_cast<double>(d["a"][0].get<int>()) / 1023.0;
        a->stick_y = static_cast<double>(d["a"][1].get<int>()) / 1023.0;
        a->throttle = static_cast<double>(d["a"][3].get<int>()) / 255.0;
        if (a->stick_area) gtk_widget_queue_draw(a->stick_area);
        if (a->throttle_area) gtk_widget_queue_draw(a->throttle_area);
    }
    if (d.contains("b")) set_button_pills(a, d["b"].get<uint64_t>());

    if (d.contains("h")) {
        int hat = d["h"].get<int>();
        if (hat != a->pov_hat) {
            a->pov_hat = hat;
            if (a->hat_area) gtk_widget_queue_draw(a->hat_area);
        }
        std::string text = std::string("POV hat: ") + hat_direction_name(hat);
        if (a->hat_label && text != a->hat_text) {
            a->hat_text = text;
            gtk_label_set_text(GTK_LABEL(a->hat_label), text.c_str());
        }
    }

    if (d.contains("tx") && d.contains("ty")) {
        int tx = d["tx"].get<int>();
        int ty = d["ty"].get<int>();
        if (tx != a->thumb_x || ty != a->thumb_y) {
            a->thumb_x = tx;
            a->thumb_y = ty;
            if (a->mini_area) gtk_widget_queue_draw(a->mini_area);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "Ministick: %2d, %2d", tx, ty);
        std::string text = buf;
        if (a->mini_label && text != a->mini_text) {
            a->mini_text = text;
            gtk_label_set_text(GTK_LABEL(a->mini_label), text.c_str());
        }
    }
}

void apply_state(App* a, const json& st) {
    a->state = st;

    bool connected = st.value("connected", false);
    if (!connected) set_pill(a->pill, "pill-bad", "no device");
    else if (st.value("grabbed", false)) set_pill(a->pill, "pill-ok", "capturing");
    else set_pill(a->pill, "pill-warn", "connected");

    if (a->window_subtitle) {
        std::string sub = st.value("profile", std::string("-"));
        sub += "  ·  mode " + std::to_string(st.value("mode", 1));
        if (!st["last_error"].get<std::string>().empty()) sub += "  ·  " + st["last_error"].get<std::string>();
        gtk_label_set_text(GTK_LABEL(a->window_subtitle), sub.c_str());
    }

    for (int i = 0; i < 3; ++i) {
        std::string line = st["mfd"][i].get<std::string>();
        if (line.size() < 16) line.append(16 - line.size(), ' ');
        gtk_label_set_text(GTK_LABEL(a->mfd_lines[i]), line.c_str());
    }

    int mode = st.value("mode", 1);
    for (int i = 0; i < 3; ++i) {
        if (!a->mode_buttons[i]) continue;
        if (i + 1 == mode) gtk_widget_add_css_class(a->mode_buttons[i], "suggested-action");
        else gtk_widget_remove_css_class(a->mode_buttons[i], "suggested-action");
    }

    for (std::size_t i = 0; i < kAxisCount; ++i) {
        double v = st["axes"][std::string(axis_name(static_cast<Axis>(i)))].get<double>();
        if (a->axis_bars[i]) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->axis_bars[i]), v);
        if (a->axis_values[i]) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%3d%%", static_cast<int>(v * 100));
            gtk_label_set_text(GTK_LABEL(a->axis_values[i]), buf);
        }
    }

    a->stick_x = st["axes"]["x"].get<double>();
    a->stick_y = st["axes"]["y"].get<double>();
    a->throttle = st["axes"]["throttle"].get<double>();
    if (a->stick_area) gtk_widget_queue_draw(a->stick_area);
    if (a->throttle_area) gtk_widget_queue_draw(a->throttle_area);

    uint64_t mask = 0;
    for (const auto& b : st["buttons"]) {
        int idx = b.get<int>();
        if (idx >= 0 && idx < static_cast<int>(kButtonCount)) mask |= (1ull << idx);
    }
    a->last_buttons_mask ^= ~0ull;  // force a refresh after a full state
    set_button_pills(a, mask);

    for (int i = 0; i < static_cast<int>(Led::Count); ++i) {
        if (!a->led_dots[i]) continue;
        std::string color =
            st["leds"].value(std::string(led_name(static_cast<Led>(i))), std::string("off"));
        for (const char* c : {"led-off", "led-red", "led-amber", "led-green"}) {
            gtk_widget_remove_css_class(a->led_dots[i], c);
        }
        char css[32];
        std::snprintf(css, sizeof(css), "led-%s", color.c_str());
        gtk_widget_add_css_class(a->led_dots[i], css);
    }

    if (a->stopwatch_label) {
        long ms = st["stopwatch"].value("elapsed_ms", 0L);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%02ld:%02ld.%02ld  %s", ms / 60000, (ms / 1000) % 60,
                      (ms % 1000) / 10, st["stopwatch"].value("running", false) ? "running" : "stopped");
        gtk_label_set_text(GTK_LABEL(a->stopwatch_label), buf);
    }

    if (a->page_drop) {
        a->suppress = true;
        std::string page = st.value("page", std::string("status"));
        for (int i = 0; kPageIds[i]; ++i) {
            if (page == kPageIds[i]) gtk_drop_down_set_selected(GTK_DROP_DOWN(a->page_drop), i);
        }
        a->suppress = false;
    }

    if (a->drift_detail && st.contains("drift") && st["drift"].value("samples", 0) > 0) {
        std::string text;
        for (const char* id : {"x", "y", "twist"}) {
            const json& d = st["drift"]["axes"][id];
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%-6s rest %+6.1f%%   jitter %5.1f%%   samples %d\n", id,
                          d.value("offset", 0.0) * 100.0, d.value("jitter", 0.0) * 100.0,
                          st["drift"].value("samples", 0));
            text += buf;
        }
        gtk_label_set_text(GTK_LABEL(a->drift_detail), text.c_str());
    }
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
void on_mode_clicked(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode"));
    json r = call(a, "set_mode", {{"mode", mode}});
    if (r.contains("__error")) toast(a, r["__error"].get<std::string>());
}

void on_page_changed(GObject* obj, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    if (a->suppress) return;
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (!kPageIds[idx]) return;
    call(a, "set_page", {{"page", kPageIds[idx]}});
}

void on_text_apply(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    json lines = json::array();
    for (auto* e : a->text_entries) lines.push_back(gtk_editable_get_text(GTK_EDITABLE(e)));
    json r = call(a, "set_text", {{"lines", lines}});
    if (r.contains("__error")) toast(a, r["__error"].get<std::string>());
    else toast(a, "Display updated");
}

void on_bright_changed(GtkRange* range, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "set_brightness", {{"mfd", static_cast<int>(gtk_range_get_value(range))}});
}

void on_led_bright_changed(GtkRange* range, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "set_brightness", {{"led", static_cast<int>(gtk_range_get_value(range))}});
}

void on_led_preset(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* color = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "color"));
    json r = call(a, "set_led", {{"led", "all"}, {"color", color}});
    if (r.contains("__error")) toast(a, r["__error"].get<std::string>());
}

void on_led_color(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* led = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "led"));
    const char* color = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "color"));
    json r = call(a, "set_led", {{"led", led}, {"color", color}, {"persist", true}});
    if (r.contains("__error")) toast(a, r["__error"].get<std::string>());
    refresh_profile(a);
}

void on_clock_settings(GObject*, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    json args;
    args["h24"] = gtk_switch_get_active(GTK_SWITCH(a->clock24)) != FALSE;
    args["clock2_offset"] = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->offs2)));
    args["clock3_offset"] = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->offs3)));
    call(a, "set_clock", args);
}

void on_clock_now(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "clock_now");
    toast(a, "Clock synchronised");
}

void on_stopwatch(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* op = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "op"));
    call(a, "stopwatch", {{"op", op}});
}

void on_shift_toggled(GObject* obj, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "set_shift", {{"on", gtk_switch_get_active(GTK_SWITCH(obj)) != FALSE}});
}

// --- bindings ---------------------------------------------------------------
std::string action_arg(const json& a) {
    std::string type = a.value("action", std::string("passthrough"));
    if (type == "key") return a.value("key", std::string());
    if (type == "exec") return a.value("command", std::string());
    if (type == "mode") return std::to_string(a.value("mode", 1));
    if (type == "mfd_page") return a.value("page", std::string("status"));
    if (type == "button") return std::to_string(a.value("button", 0));
    if (type == "wheel") return std::to_string(a.value("delta", 1));
    if (type == "mouse") return a.value("button", std::string("left"));
    if (type == "macro") {
        std::string joined;
        for (const auto& s : a.value("sequence", json::array())) {
            if (!joined.empty()) joined += ", ";
            joined += s.get<std::string>();
        }
        return joined;
    }
    return {};
}

int action_index(const json& a) {
    std::string type = a.value("action", std::string("passthrough"));
    for (int i = 0; kActionIds[i]; ++i) {
        if (type == kActionIds[i]) return i;
    }
    return 0;
}

GtkWidget* make_binding_row(App* a, int index, bool shift_layer) {
    int mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->layer_mode_drop)) + 1;
    std::string mkey = std::to_string(mode);
    std::string layer = shift_layer ? "shift_buttons" : "buttons";

    json action = json{{"action", "passthrough"}};
    if (a->profile.contains("modes") && a->profile["modes"].contains(mkey) &&
        a->profile["modes"][mkey].contains(layer) &&
        a->profile["modes"][mkey][layer].contains(std::to_string(index))) {
        action = a->profile["modes"][mkey][layer][std::to_string(index)];
    }

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_add_css_class(box, "row");

    GtkWidget* label = gtk_label_new(button_name(static_cast<Button>(index)).data());
    gtk_widget_set_size_request(label, 132, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_widget_add_css_class(label, "row-title");
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget* drop = gtk_drop_down_new_from_strings(kActionNames);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), action_index(action));
    gtk_widget_set_size_request(drop, 158, -1);
    gtk_box_append(GTK_BOX(box), drop);

    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), action_arg(action).c_str());
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "key · command · page · macro steps");
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_box_append(GTK_BOX(box), entry);

    a->btn_action[index] = drop;
    a->btn_arg[index] = entry;
    return box;
}

void rebuild_bindings(App* a) {
    if (!a->buttons_box) return;
    GtkWidget* child = gtk_widget_get_first_child(a->buttons_box);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(a->buttons_box), child);
        child = next;
    }
    bool shift = gtk_switch_get_active(GTK_SWITCH(a->shift_switch)) != FALSE;
    for (int i = 0; i < static_cast<int>(kButtonCount); ++i) {
        gtk_box_append(GTK_BOX(a->buttons_box), make_binding_row(a, i, shift));
    }
}

void on_layer_changed(GObject*, GParamSpec*, gpointer data) {
    rebuild_bindings(static_cast<App*>(data));
}

void on_apply_bindings(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    int mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->layer_mode_drop)) + 1;
    bool shift = gtk_switch_get_active(GTK_SWITCH(a->shift_switch)) != FALSE;
    std::string key = shift ? "shift_buttons" : "buttons";
    std::string mkey = std::to_string(mode);

    if (!a->profile.contains("modes")) {
        toast(a, "Profile not loaded");
        return;
    }
    json& layer = a->profile["modes"][mkey];
    if (!layer.is_object()) layer = json::object();
    if (!layer.contains(key) || !layer[key].is_object()) layer[key] = json::object();

    for (int i = 0; i < static_cast<int>(kButtonCount); ++i) {
        if (!a->btn_action[i]) continue;
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->btn_action[i]));
        std::string type = kActionIds[sel];
        std::string arg = gtk_editable_get_text(GTK_EDITABLE(a->btn_arg[i]));
        std::string bkey = std::to_string(i);

        if (type == "passthrough") {
            layer[key].erase(bkey);
            continue;
        }
        json action{{"action", type}};
        if (type == "key") action["key"] = arg;
        else if (type == "exec") action["command"] = arg;
        else if (type == "mode") action["mode"] = std::atoi(arg.c_str());
        else if (type == "mfd_page") action["page"] = arg;
        else if (type == "button") action["button"] = std::atoi(arg.c_str());
        else if (type == "wheel") action["delta"] = std::atoi(arg.c_str());
        else if (type == "mouse") action["button"] = arg;
        else if (type == "macro") {
            json seq = json::array();
            std::string item;
            for (char c : arg) {
                if (c == ',') {
                    if (!item.empty()) seq.push_back(item);
                    item.clear();
                } else if (c != ' ') {
                    item.push_back(c);
                }
            }
            if (!item.empty()) seq.push_back(item);
            action["sequence"] = seq;
        }
        layer[key][bkey] = action;
    }

    json r = call(a, "profile", {{"action", "set"}, {"profile", a->profile}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    refresh_profile(a);
    toast(a, "Bindings applied");
}

// --- diagnostics ------------------------------------------------------------
gboolean on_drift_finished(gpointer data);

void on_drift_start(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    json r = call(a, "drift", {{"op", "start"}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    gtk_widget_set_sensitive(GTK_WIDGET(b), FALSE);
    gtk_label_set_text(GTK_LABEL(a->drift_detail), "Measuring… keep the stick still for 5 seconds.");
    g_timeout_add(5000, on_drift_finished, a);
}

gboolean on_drift_finished(gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "drift", {{"op", "stop"}});
    if (a->drift_button) gtk_widget_set_sensitive(a->drift_button, TRUE);
    update_state(a);
    return G_SOURCE_REMOVE;
}

void on_drift_apply(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    const json& axes = a->state["drift"]["axes"];
    int applied = 0;
    for (const char* id : {"x", "y", "twist"}) {
        double offset = axes[id].value("offset", 0.0);
        if (std::fabs(offset) < 0.01) continue;
        json r = call(a, "apply_axis", {{"axis", id}, {"center", offset}});
        if (!r.contains("__error")) ++applied;
    }
    if (applied == 0) {
        toast(a, "No drift to correct");
        return;
    }
    refresh_profile(a);
    char msg[64];
    std::snprintf(msg, sizeof(msg), "Centre correction applied to %d axis(es)", applied);
    toast(a, msg);
}

void rebuild_checks(App* a, const json& diag) {
    if (!a->checks_box) return;
    GtkWidget* child = gtk_widget_get_first_child(a->checks_box);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(a->checks_box), child);
        child = next;
    }
    int fails = 0, warns = 0;
    if (!diag.contains("checks")) return;
    for (const auto& c : diag["checks"]) {
        std::string level = c.value("level", std::string("ok"));
        if (level == "fail") ++fails;
        else if (level == "warn") ++warns;

        GtkWidget* r = row(c.value("name", std::string("?")).c_str(),
                           c.value("detail", std::string()).c_str(), nullptr);
        GtkWidget* pill = gtk_label_new(level.c_str());
        gtk_widget_add_css_class(pill, "pill");
        gtk_widget_add_css_class(pill, level == "fail" ? "pill-bad"
                                                       : (level == "warn" ? "pill-warn" : "pill-ok"));
        gtk_widget_set_valign(pill, GTK_ALIGN_CENTER);
        gtk_box_append(GTK_BOX(r), pill);
        gtk_box_append(GTK_BOX(a->checks_box), r);
        GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(sep, "sep");
        gtk_box_append(GTK_BOX(a->checks_box), sep);
    }
    if (a->checks_pill) {
        char buf[64];
        if (fails) std::snprintf(buf, sizeof(buf), "%d issue(s)", fails);
        else if (warns) std::snprintf(buf, sizeof(buf), "%d warning(s)", warns);
        else std::snprintf(buf, sizeof(buf), "all good");
        set_pill(a->checks_pill, fails ? "pill-bad" : (warns ? "pill-warn" : "pill-ok"), buf);
    }
}

void on_run_checks(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    json d = call(a, "diagnostics");
    if (d.contains("__error")) {
        toast(a, d["__error"].get<std::string>());
        return;
    }
    rebuild_checks(a, d);
}

void on_feel_axis(GObject* obj, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    int idx = static_cast<int>(gtk_drop_down_get_selected(GTK_DROP_DOWN(obj)));
    if (idx < 0 || !kAxisIds[idx]) return;
    const json& axes = a->profile["axes"];
    std::string id = kAxisIds[idx];
    a->suppress = true;
    if (axes.contains(id)) {
        gtk_range_set_value(GTK_RANGE(a->feel_deadzone), axes[id].value("deadzone", 0.0));
        gtk_label_set_text(GTK_LABEL(a->feel_curve),
                           axes[id].value("curve", 1.0) > 1.05 ? "soft"
                           : (axes[id].value("curve", 1.0) < 0.95 ? "hard" : "linear"));
    } else {
        gtk_range_set_value(GTK_RANGE(a->feel_deadzone), 0.0);
        gtk_label_set_text(GTK_LABEL(a->feel_curve), "linear");
    }
    a->suppress = false;
}

void on_feel_preset(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    int idx = static_cast<int>(gtk_drop_down_get_selected(GTK_DROP_DOWN(a->feel_drop)));
    if (idx < 0 || !kAxisIds[idx]) return;
    double curve = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "curve")) / 100.0;
    json r = call(a, "apply_axis", {{"axis", kAxisIds[idx]}, {"curve", curve}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    refresh_profile(a);
    gtk_label_set_text(GTK_LABEL(a->feel_curve),
                       curve > 1.05 ? "soft" : (curve < 0.95 ? "hard" : "linear"));
    toast(a, "Axis response updated");
}

void on_feel_deadzone(GtkRange* range, gpointer data) {
    App* a = static_cast<App*>(data);
    if (a->suppress) return;
    int idx = static_cast<int>(gtk_drop_down_get_selected(GTK_DROP_DOWN(a->feel_drop)));
    if (idx < 0 || !kAxisIds[idx]) return;
    call(a, "apply_axis", {{"axis", kAxisIds[idx]}, {"deadzone", gtk_range_get_value(range)}});
}

// --- profiles ---------------------------------------------------------------
void reload_profiles(App* a) {
    json r = call(a, "profile", {{"action", "list"}});
    if (r.contains("__error") || !a->profile_list) return;
    GtkWidget* child = gtk_widget_get_first_child(a->profile_list);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(a->profile_list), child);
        child = next;
    }
    std::string current = r.value("current", std::string());
    for (const auto& p : r["profiles"]) {
        std::string name = p.get<std::string>();
        GtkWidget* lr = gtk_list_box_row_new();
        GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(box, 8);
        gtk_widget_set_margin_bottom(box, 8);
        gtk_widget_set_margin_start(box, 12);
        gtk_widget_set_margin_end(box, 12);
        GtkWidget* lbl = gtk_label_new(name.c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_hexpand(lbl, TRUE);
        gtk_box_append(GTK_BOX(box), lbl);
        if (name == current) {
            GtkWidget* tag = gtk_label_new("active");
            set_pill(tag, "pill-ok", "active");
            gtk_box_append(GTK_BOX(box), tag);
        }
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(lr), box);
        g_object_set_data_full(G_OBJECT(lr), "name", g_strdup(name.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(a->profile_list), lr);
    }
}

void on_profile_activated(GtkListBox*, GtkListBoxRow* lr, gpointer data) {
    App* a = static_cast<App*>(data);
    if (!lr) return;
    const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(lr), "name"));
    if (!name) return;
    json r = call(a, "profile", {{"action", "load"}, {"name", name}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    refresh_profile(a);
    reload_profiles(a);
    toast(a, std::string("Loaded ") + name);
}

void on_profile_save(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* name = gtk_editable_get_text(GTK_EDITABLE(a->profile_name));
    if (!name || !*name) {
        toast(a, "Enter a profile name");
        return;
    }
    json r = call(a, "profile", {{"action", "save"}, {"name", name}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    reload_profiles(a);
    toast(a, std::string("Saved ") + name);
}

void on_profile_new(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* name = gtk_editable_get_text(GTK_EDITABLE(a->profile_name));
    if (!name || !*name) {
        toast(a, "Enter a name first");
        return;
    }
    json r = call(a, "profile", {{"action", "create"}, {"name", name}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    call(a, "profile", {{"action", "save"}, {"name", name}});
    refresh_profile(a);
    reload_profiles(a);
    toast(a, "New profile created");
}

void on_profile_delete(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* name = gtk_editable_get_text(GTK_EDITABLE(a->profile_name));
    if (!name || !*name) return;
    json r = call(a, "profile", {{"action", "delete"}, {"name", name}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    reload_profiles(a);
    toast(a, "Profile deleted");
}

void refresh_profile(App* a) {
    json r = call(a, "profile", {{"action", "get"}});
    if (r.contains("__error") || !r.contains("profile")) return;
    a->profile = r["profile"];
    const json& s = a->profile["settings"];

    if (s.contains("custom_text") && s["custom_text"].is_array()) {
        for (int i = 0; i < 3; ++i) {
            if (!a->text_entries[i] || i >= static_cast<int>(s["custom_text"].size())) continue;
            gtk_editable_set_text(GTK_EDITABLE(a->text_entries[i]),
                                  s["custom_text"][i].get<std::string>().c_str());
        }
    }
    if (a->mfd_bright && s.contains("mfd_brightness")) {
        g_signal_handlers_block_by_func(a->mfd_bright, (gpointer)on_bright_changed, a);
        gtk_range_set_value(GTK_RANGE(a->mfd_bright), s["mfd_brightness"].get<int>());
        g_signal_handlers_unblock_by_func(a->mfd_bright, (gpointer)on_bright_changed, a);
    }
    if (a->led_bright && s.contains("led_brightness")) {
        g_signal_handlers_block_by_func(a->led_bright, (gpointer)on_led_bright_changed, a);
        gtk_range_set_value(GTK_RANGE(a->led_bright), s["led_brightness"].get<int>());
        g_signal_handlers_unblock_by_func(a->led_bright, (gpointer)on_led_bright_changed, a);
    }
    if (a->clock24 && s.contains("clock_24h")) {
        gtk_switch_set_active(GTK_SWITCH(a->clock24), s["clock_24h"].get<bool>());
    }
    if (a->offs2 && s.contains("clock2_offset")) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offs2), s["clock2_offset"].get<int>());
    }
    if (a->offs3 && s.contains("clock3_offset")) {
        gtk_spin_button_set_value(GTK_SPIN_BUTTON(a->offs3), s["clock3_offset"].get<int>());
    }
}

// ---------------------------------------------------------------------------
// pages
// ---------------------------------------------------------------------------
GtkWidget* build_overview(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("Overview", "Live state of the throttle and stick.", &body);

    GtkWidget* card_device = card("DEVICE", nullptr);
    GtkWidget* status_row = row("Logitech X52 Pro H.O.T.A.S.",
                                "vendor channel 0x91 · 3x16 MFD · 11 LEDs", nullptr);
    a->pill = gtk_label_new("…");
    set_pill(a->pill, "pill-warn", "connecting");
    gtk_widget_set_valign(a->pill, GTK_ALIGN_CENTER);
    gtk_box_append(GTK_BOX(status_row), a->pill);
    add_row(card_device, status_row);
    gtk_box_append(GTK_BOX(body), card_device);

    GtkWidget* card_mfd = card("THROTTLE DISPLAY", nullptr);
    GtkWidget* lcd_wrap = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_margin_top(lcd_wrap, 8);
    gtk_widget_set_margin_bottom(lcd_wrap, 14);
    gtk_widget_set_margin_start(lcd_wrap, 18);
    gtk_widget_set_margin_end(lcd_wrap, 18);
    gtk_box_append(GTK_BOX(lcd_wrap), lcd_widget(a));
    gtk_box_append(GTK_BOX(card_mfd), lcd_wrap);
    gtk_box_append(GTK_BOX(body), card_mfd);

    GtkWidget* card_mode = card("MODE", "The hardware switch follows this selection.");
    GtkWidget* seg = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(seg, "linked");
    for (int i = 0; i < 3; ++i) {
        char label[16];
        std::snprintf(label, sizeof(label), "Mode %d", i + 1);
        GtkWidget* b = gtk_button_new_with_label(label);
        g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(i + 1));
        g_signal_connect(b, "clicked", G_CALLBACK(on_mode_clicked), a);
        gtk_box_append(GTK_BOX(seg), b);
        a->mode_buttons[i] = b;
    }
    add_row(card_mode, row("Active mode", nullptr, seg));

    GtkWidget* page_drop = gtk_drop_down_new_from_strings(kPageNames);
    gtk_widget_set_size_request(page_drop, 170, -1);
    g_signal_connect(page_drop, "notify::selected", G_CALLBACK(on_page_changed), a);
    a->page_drop = page_drop;
    add_row(card_mode, row("MFD page", "What the display is showing", page_drop));
    gtk_box_append(GTK_BOX(body), card_mode);

    GtkWidget* card_axes = card("AXES", nullptr);
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        GtkWidget* bar = gtk_progress_bar_new();
        gtk_widget_set_size_request(bar, 220, -1);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), FALSE);
        a->axis_bars[i] = bar;
        GtkWidget* val = gtk_label_new("  0%");
        gtk_widget_add_css_class(val, "mono");
        a->axis_values[i] = val;
        GtkWidget* ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_box_append(GTK_BOX(ctl), bar);
        gtk_box_append(GTK_BOX(ctl), val);
        add_row(card_axes, row(std::string(axis_name(static_cast<Axis>(i))).c_str(), nullptr, ctl));
    }
    gtk_box_append(GTK_BOX(body), card_axes);
    return page;
}

GtkWidget* build_display(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("Display",
                                "Write the three MFD lines, set brightness and program the clock.",
                                &body);

    GtkWidget* card_text = card("CUSTOM TEXT", "16 fixed-width characters per line, uppercase");
    for (int i = 0; i < 3; ++i) {
        GtkWidget* e = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(e), 16);
        gtk_editable_set_width_chars(GTK_EDITABLE(e), 18);
        gtk_entry_set_placeholder_text(GTK_ENTRY(e), "line text");
        a->text_entries[i] = e;
        char title[16];
        std::snprintf(title, sizeof(title), "Line %d", i + 1);
        add_row(card_text, row(title, nullptr, e));
    }
    GtkWidget* apply = gtk_button_new_with_label("Write to display");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_widget_set_halign(apply, GTK_ALIGN_END);
    gtk_widget_set_margin_top(apply, 10);
    gtk_widget_set_margin_bottom(apply, 14);
    gtk_widget_set_margin_end(apply, 18);
    g_signal_connect(apply, "clicked", G_CALLBACK(on_text_apply), a);
    gtk_box_append(GTK_BOX(card_text), apply);
    gtk_box_append(GTK_BOX(body), card_text);

    GtkWidget* card_light = card("BRIGHTNESS", nullptr);
    GtkWidget* mfd_s = nullptr;
    add_row(card_light, row("Display", "0 – 255", slider(0, 255, 180, &mfd_s)));
    a->mfd_bright = mfd_s;
    g_signal_connect(mfd_s, "value-changed", G_CALLBACK(on_bright_changed), a);
    GtkWidget* led_s = nullptr;
    add_row(card_light, row("LEDs", "0 – 255", slider(0, 255, 180, &led_s)));
    a->led_bright = led_s;
    g_signal_connect(led_s, "value-changed", G_CALLBACK(on_led_bright_changed), a);
    gtk_box_append(GTK_BOX(body), card_light);

    GtkWidget* card_clock = card("CLOCK",
                                 "The device keeps its own time; clocks 2 and 3 are offsets.");
    GtkWidget* h24 = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(h24), TRUE);
    a->clock24 = h24;
    add_row(card_clock, row("24 hour format", nullptr, h24));

    GtkWidget* s2 = gtk_spin_button_new_with_range(-1439, 1439, 15);
    a->offs2 = s2;
    add_row(card_clock, row("Clock 2 offset", "minutes from the base clock", s2));
    GtkWidget* s3 = gtk_spin_button_new_with_range(-1439, 1439, 15);
    a->offs3 = s3;
    add_row(card_clock, row("Clock 3 offset", "minutes from the base clock", s3));

    GtkWidget* sync = gtk_button_new_with_label("Set from system");
    g_signal_connect(sync, "clicked", G_CALLBACK(on_clock_now), a);
    add_row(card_clock, row("Synchronise", "copy date and time from this computer", sync));
    gtk_box_append(GTK_BOX(body), card_clock);

    g_signal_connect(h24, "notify::active", G_CALLBACK(on_clock_settings), a);
    g_signal_connect(s2, "value-changed", G_CALLBACK(on_clock_settings), a);
    g_signal_connect(s3, "value-changed", G_CALLBACK(on_clock_settings), a);

    GtkWidget* card_sw = card("STOPWATCH", nullptr);
    a->stopwatch_label = gtk_label_new("00:00.00  stopped");
    gtk_widget_add_css_class(a->stopwatch_label, "mono");
    add_row(card_sw, row("Elapsed", "shown on the stopwatch page", a->stopwatch_label));
    GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(btns, "linked");
    const char* ops[] = {"start", "stop", "reset"};
    const char* labels[] = {"Start", "Stop", "Reset"};
    for (int i = 0; i < 3; ++i) {
        GtkWidget* b = gtk_button_new_with_label(labels[i]);
        g_object_set_data(G_OBJECT(b), "op", const_cast<char*>(ops[i]));
        g_signal_connect(b, "clicked", G_CALLBACK(on_stopwatch), a);
        gtk_box_append(GTK_BOX(btns), b);
    }
    add_row(card_sw, row("Controls", nullptr, btns));
    gtk_box_append(GTK_BOX(body), card_sw);
    return page;
}

GtkWidget* build_leds(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("LEDs", "Per-mode colours, stored in the active profile.", &body);

    GtkWidget* quick = card("ALL LEDS", nullptr);
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char* presets[] = {"Off", "Red", "Amber", "Green"};
    const char* colors[] = {"off", "red", "amber", "green"};
    for (int i = 0; i < 4; ++i) {
        GtkWidget* b = gtk_button_new_with_label(presets[i]);
        g_object_set_data(G_OBJECT(b), "color", const_cast<char*>(colors[i]));
        g_signal_connect(b, "clicked", G_CALLBACK(on_led_preset), a);
        gtk_box_append(GTK_BOX(box), b);
    }
    add_row(quick, row("Preset", "applies to every LED", box));
    gtk_box_append(GTK_BOX(body), quick);

    GtkWidget* group = card("INDIVIDUAL LEDS", nullptr);
    for (std::size_t i = 0; i < static_cast<std::size_t>(Led::Count); ++i) {
        Led led = static_cast<Led>(i);
        GtkWidget* r = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
        gtk_widget_add_css_class(r, "row");

        GtkWidget* dot = gtk_label_new("");
        gtk_widget_add_css_class(dot, "led-dot");
        gtk_widget_add_css_class(dot, "led-off");
        a->led_dots[i] = dot;
        gtk_box_append(GTK_BOX(r), dot);

        GtkWidget* name = gtk_label_new(std::string(led_name(led)).c_str());
        gtk_widget_add_css_class(name, "row-title");
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_widget_set_hexpand(name, TRUE);
        gtk_box_append(GTK_BOX(r), name);

        GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(btns, "linked");
        const char* labels[] = {"Off", "R", "A", "G"};
        for (int c = 0; c < 4; ++c) {
            GtkWidget* b = gtk_button_new_with_label(labels[c]);
            gtk_widget_add_css_class(b, "flat");
            g_object_set_data_full(G_OBJECT(b), "led",
                                   g_strdup(std::string(led_name(led)).c_str()), g_free);
            g_object_set_data(G_OBJECT(b), "color", const_cast<char*>(colors[c]));
            g_signal_connect(b, "clicked", G_CALLBACK(on_led_color), a);
            gtk_box_append(GTK_BOX(btns), b);
        }
        gtk_box_append(GTK_BOX(r), btns);
        gtk_box_append(GTK_BOX(group), r);
        GtkWidget* sep = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
        gtk_widget_add_css_class(sep, "sep");
        gtk_box_append(GTK_BOX(group), sep);
    }
    gtk_box_append(GTK_BOX(body), group);

    GtkWidget* ind = card("INDICATORS", nullptr);
    GtkWidget* shift = gtk_switch_new();
    g_signal_connect(shift, "notify::active", G_CALLBACK(on_shift_toggled), a);
    add_row(ind, row("Shift indicator", "the dot on the MFD", shift));
    gtk_box_append(GTK_BOX(body), ind);
    return page;
}

GtkWidget* build_buttons(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("Buttons",
                                "Every button can send a key, a macro, a mode change or a command.",
                                &body);

    GtkWidget* header = card("LAYER", nullptr);
    static const char* mode_names[] = {"Mode 1", "Mode 2", "Mode 3", nullptr};
    GtkWidget* mode_drop = gtk_drop_down_new_from_strings(mode_names);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(mode_drop), 0);
    a->layer_mode_drop = mode_drop;
    add_row(header, row("Mode", "each mode has its own bindings", mode_drop));

    GtkWidget* shift = gtk_switch_new();
    a->shift_switch = shift;
    add_row(header, row("Shift layer", "pinky held: the second set of bindings", shift));
    gtk_box_append(GTK_BOX(body), header);

    GtkWidget* list = card("BINDINGS", "unset shift entries fall back to the base layer");
    GtkWidget* container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    a->buttons_box = container;
    gtk_box_append(GTK_BOX(list), container);
    GtkWidget* apply = gtk_button_new_with_label("Apply bindings");
    gtk_widget_add_css_class(apply, "suggested-action");
    gtk_widget_set_halign(apply, GTK_ALIGN_END);
    gtk_widget_set_margin_top(apply, 10);
    gtk_widget_set_margin_bottom(apply, 14);
    gtk_widget_set_margin_end(apply, 18);
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply_bindings), a);
    gtk_box_append(GTK_BOX(list), apply);
    gtk_box_append(GTK_BOX(body), list);

    g_signal_connect(mode_drop, "notify::selected", G_CALLBACK(on_layer_changed), a);
    g_signal_connect(shift, "notify::active", G_CALLBACK(on_layer_changed), a);
    return page;
}

GtkWidget* build_test(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("Test",
                                "Live input, drift measurement, axis feel and a full diagnosis.",
                                &body);

    // --- live input ---------------------------------------------------------
    GtkWidget* live = card("LIVE INPUT", "press anything: the matching cell lights up");
    GtkWidget* viz = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 22);
    gtk_widget_set_margin_top(viz, 10);
    gtk_widget_set_margin_bottom(viz, 12);
    gtk_widget_set_margin_start(viz, 18);
    gtk_widget_set_margin_end(viz, 18);

    GtkWidget* stick = gtk_drawing_area_new();
    gtk_widget_set_size_request(stick, 170, 170);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(stick), draw_stick, a, nullptr);
    a->stick_area = stick;
    gtk_box_append(GTK_BOX(viz), stick);

    GtkWidget* throttle = gtk_drawing_area_new();
    gtk_widget_set_size_request(throttle, 46, 170);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(throttle), draw_throttle, a, nullptr);
    a->throttle_area = throttle;
    gtk_box_append(GTK_BOX(viz), throttle);

    GtkWidget* meters = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_hexpand(meters, TRUE);
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        GtkWidget* line = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        char axis_label[64];
        std::snprintf(axis_label, sizeof axis_label, "%d  %s",
                      kAxisGameIds[i] + 1, kAxisLabels[i]);
        GtkWidget* name = gtk_label_new(axis_label);
        char axis_tip[128];
        std::snprintf(axis_tip, sizeof axis_tip,
                      "War Thunder: C1 axis %d\n.blk: axisId:i=%d",
                      kAxisGameIds[i] + 1, kAxisGameIds[i]);
        gtk_widget_set_tooltip_text(name, axis_tip);
        gtk_label_set_xalign(GTK_LABEL(name), 0.0f);
        gtk_widget_add_css_class(name, "row-sub");
        gtk_widget_set_size_request(name, 116, -1);
        GtkWidget* bar = gtk_progress_bar_new();
        gtk_widget_set_hexpand(bar, TRUE);
        a->axis_bars[i] = bar;
        GtkWidget* val = gtk_label_new("  0%");
        gtk_widget_add_css_class(val, "mono");
        gtk_widget_set_size_request(val, 44, -1);
        a->axis_values[i] = val;
        gtk_box_append(GTK_BOX(line), name);
        gtk_box_append(GTK_BOX(line), bar);
        gtk_box_append(GTK_BOX(line), val);
        gtk_box_append(GTK_BOX(meters), line);
    }
    gtk_box_append(GTK_BOX(viz), meters);
    gtk_box_append(GTK_BOX(live), viz);

    GtkWidget* grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 5);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 5);
    gtk_widget_set_margin_start(grid, 18);
    gtk_widget_set_margin_end(grid, 18);
    gtk_widget_set_margin_bottom(grid, 16);
    for (int i = 0; i < static_cast<int>(kButtonCount); ++i) {
        // War Thunder counts from one and prefixes the controller ("C1 button 2"),
        // the .blk counts from zero.  Lead with the number people read in the
        // game and keep the file's index in the tooltip.
        char btn_label[96];
        std::snprintf(btn_label, sizeof btn_label, "%d  %s", i + 1,
                      button_name(static_cast<Button>(i)).data());
        GtkWidget* p = gtk_label_new(btn_label);
        char btn_tip[128];
        std::snprintf(btn_tip, sizeof btn_tip,
                      "War Thunder: C1 button %d\n.blk: joyButton:i=%d", i + 1, i);
        gtk_widget_set_tooltip_text(p, btn_tip);
        gtk_widget_add_css_class(p, "key-pill");
        gtk_label_set_ellipsize(GTK_LABEL(p), PANGO_ELLIPSIZE_END);
        gtk_widget_set_size_request(p, 124, -1);
        gtk_grid_attach(GTK_GRID(grid), p, i % 6, i / 6, 1, 1);
        a->button_pills[i] = p;
    }
    gtk_box_append(GTK_BOX(live), grid);

    GtkWidget* grid_note = gtk_label_new(
        "Numbered as War Thunder shows them — C1 button 1 is the trigger. "
        "The .blk file counts from zero, one lower; hover a key for both.");
    gtk_widget_add_css_class(grid_note, "row-sub");
    gtk_label_set_xalign(GTK_LABEL(grid_note), 0.0f);
    gtk_widget_set_margin_start(grid_note, 18);
    gtk_widget_set_margin_top(grid_note, 6);
    gtk_box_append(GTK_BOX(live), grid_note);

    GtkWidget* extra = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 26);
    gtk_widget_set_margin_top(extra, 14);
    gtk_widget_set_margin_bottom(extra, 18);
    gtk_widget_set_margin_start(extra, 18);
    gtk_widget_set_margin_end(extra, 18);

    GtkWidget* hat_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* hat = gtk_drawing_area_new();
    gtk_widget_set_size_request(hat, 120, 120);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(hat), draw_hat, a, nullptr);
    a->hat_area = hat;
    gtk_box_append(GTK_BOX(hat_box), hat);
    GtkWidget* hat_caption = gtk_label_new("POV hat");
    gtk_widget_add_css_class(hat_caption, "row-sub");
    gtk_box_append(GTK_BOX(hat_box), hat_caption);
    a->hat_label = gtk_label_new("POV hat: CENTRE");
    gtk_widget_add_css_class(a->hat_label, "mono");
    gtk_box_append(GTK_BOX(hat_box), a->hat_label);
    a->hat_text = "POV hat: CENTRE";
    gtk_box_append(GTK_BOX(extra), hat_box);

    GtkWidget* mini_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    GtkWidget* mini = gtk_drawing_area_new();
    gtk_widget_set_size_request(mini, 120, 120);
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(mini), draw_ministick, a, nullptr);
    a->mini_area = mini;
    gtk_box_append(GTK_BOX(mini_box), mini);
    GtkWidget* mini_caption = gtk_label_new("Throttle ministick");
    gtk_widget_add_css_class(mini_caption, "row-sub");
    gtk_box_append(GTK_BOX(mini_box), mini_caption);
    a->mini_label = gtk_label_new("Ministick:  8,  8");
    gtk_widget_add_css_class(a->mini_label, "mono");
    gtk_box_append(GTK_BOX(mini_box), a->mini_label);
    a->mini_text = "Ministick:  8,  8";
    gtk_box_append(GTK_BOX(extra), mini_box);

    gtk_box_append(GTK_BOX(live), extra);
    gtk_box_append(GTK_BOX(body), live);

    // --- drift --------------------------------------------------------------
    GtkWidget* drift = card("DRIFT & CALIBRATION",
                            "measures the rest position of roll, pitch and yaw");
    GtkWidget* drift_btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* start = gtk_button_new_with_label("Run 5 s test");
    gtk_widget_add_css_class(start, "suggested-action");
    g_signal_connect(start, "clicked", G_CALLBACK(on_drift_start), a);
    a->drift_button = start;
    GtkWidget* fix = gtk_button_new_with_label("Apply centre correction");
    g_signal_connect(fix, "clicked", G_CALLBACK(on_drift_apply), a);
    gtk_box_append(GTK_BOX(drift_btns), start);
    gtk_box_append(GTK_BOX(drift_btns), fix);
    add_row(drift, row("Measurement", "leave the stick untouched while it runs", drift_btns));

    a->drift_detail = gtk_label_new("No measurement yet.");
    gtk_widget_add_css_class(a->drift_detail, "mono");
    gtk_label_set_xalign(GTK_LABEL(a->drift_detail), 0.0f);
    gtk_widget_set_margin_start(a->drift_detail, 18);
    gtk_widget_set_margin_end(a->drift_detail, 18);
    gtk_widget_set_margin_bottom(a->drift_detail, 14);
    gtk_box_append(GTK_BOX(drift), a->drift_detail);
    gtk_box_append(GTK_BOX(body), drift);

    // --- axis feel ----------------------------------------------------------
    GtkWidget* feel = card("AXIS FEEL", "soft response around the centre, or a harder edge");
    GtkWidget* feel_drop = gtk_drop_down_new_from_strings(kAxisLabels);
    gtk_widget_set_size_request(feel_drop, 170, -1);
    a->feel_drop = feel_drop;
    add_row(feel, row("Axis", nullptr, feel_drop));

    GtkWidget* presets = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(presets, "linked");
    struct Preset {
        const char* label;
        int curve;
    };
    const Preset kPresets[] = {{"Soft", 160}, {"Linear", 100}, {"Hard", 60}};
    for (const auto& p : kPresets) {
        GtkWidget* b = gtk_button_new_with_label(p.label);
        g_object_set_data(G_OBJECT(b), "curve", GINT_TO_POINTER(p.curve));
        g_signal_connect(b, "clicked", G_CALLBACK(on_feel_preset), a);
        gtk_box_append(GTK_BOX(presets), b);
    }
    add_row(feel, row("Response", "soft rounds the centre, hard sharpens it", presets));

    GtkWidget* dz = nullptr;
    GtkWidget* dz_slider = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, 0.0, 0.30, 0.01);
    gtk_scale_set_draw_value(GTK_SCALE(dz_slider), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(dz_slider), GTK_POS_RIGHT);
    gtk_widget_set_size_request(dz_slider, 200, -1);
    dz = dz_slider;
    a->feel_deadzone = dz;
    g_signal_connect(dz, "value-changed", G_CALLBACK(on_feel_deadzone), a);
    add_row(feel, row("Deadzone", "ignore small movements around the centre", dz));

    a->feel_curve = gtk_label_new("linear");
    gtk_widget_add_css_class(a->feel_curve, "mono");
    add_row(feel, row("Current response", nullptr, a->feel_curve));
    gtk_box_append(GTK_BOX(body), feel);

    // --- diagnosis ----------------------------------------------------------
    GtkWidget* diag = card("DIAGNOSIS", "permissions, endpoints, grab, rest positions");
    GtkWidget* run = gtk_button_new_with_label("Run diagnosis");
    a->checks_pill = gtk_label_new("not run");
    set_pill(a->checks_pill, "pill-warn", "not run");
    GtkWidget* diag_ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(diag_ctl), a->checks_pill);
    gtk_box_append(GTK_BOX(diag_ctl), run);
    add_row(diag, row("Health check", nullptr, diag_ctl));
    a->checks_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_append(GTK_BOX(diag), a->checks_box);
    gtk_box_append(GTK_BOX(body), diag);

    g_signal_connect(run, "clicked", G_CALLBACK(on_run_checks), a);
    g_signal_connect(feel_drop, "notify::selected", G_CALLBACK(on_feel_axis), a);
    return page;
}

void on_setting_toggled(GObject* obj, GParamSpec*, gpointer data);
void refresh_telemetry(App* a);

void apply_settings(App* a, const json& s) {
    struct Pair {
        GtkWidget* w;
        const char* key;
    };
    const Pair pairs[] = {{a->autostart_switch, "autostart"},
                          {a->minimized_switch, "start_minimized"},
                          {a->verify_switch, "verify_link"},
                          {a->autosave_switch, "autosave"}};
    for (const auto& p : pairs) {
        if (!p.w) continue;
        g_signal_handlers_block_by_func(p.w, (gpointer)on_setting_toggled, a);
        gtk_switch_set_active(GTK_SWITCH(p.w), s.value(p.key, false));
        g_signal_handlers_unblock_by_func(p.w, (gpointer)on_setting_toggled, a);
    }
}

void load_settings(App* a) {
    json s = call(a, "get_settings");
    if (s.contains("__error")) return;
    apply_settings(a, s);
}

void on_setting_toggled(GObject* obj, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* key = static_cast<const char*>(g_object_get_data(obj, "key"));
    if (!key) return;
    json args;
    args[key] = gtk_switch_get_active(GTK_SWITCH(obj)) != FALSE;
    json r = call(a, "set_settings", args);
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    if (std::string(key) == "start_minimized" || std::string(key) == "autostart") {
        toast(a, "Startup entry updated");
    }
}

void on_link_test(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    if (a->link_pill) set_pill(a->link_pill, "pill-warn", "testing…");
    json r = call(a, "link_test");
    if (r.contains("__error")) {
        if (a->link_pill) set_pill(a->link_pill, "pill-bad", "failed");
        if (a->link_detail) gtk_label_set_text(GTK_LABEL(a->link_detail), r["__error"].get<std::string>().c_str());
        return;
    }
    bool ok = r.value("valid", false);
    if (a->link_pill) set_pill(a->link_pill, ok ? "pill-ok" : "pill-warn", ok ? "healthy" : "degraded");
    if (a->link_detail) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
                      "%d transfers, %d failed\nwrite average %.2f ms, worst %.2f ms\n"
                      "input frames in the last 2 s: %d\n%s",
                      r.value("samples", 0), r.value("failures", 0), r.value("avg_ms", 0.0),
                      r.value("max_ms", 0.0), r.value("frames_2s", 0),
                      r.value("note", std::string()).c_str());
        gtk_label_set_text(GTK_LABEL(a->link_detail), buf);
    }
}

void rebuild_rule_rows(App* a);

void on_tele_source(GObject* obj, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    guint idx = gtk_drop_down_get_selected(GTK_DROP_DOWN(obj));
    if (!kTelemetrySourceIds[idx]) return;
    json r = call(a, "set_telemetry", {{"source", kTelemetrySourceIds[idx]}});
    if (r.contains("__error")) toast(a, r["__error"].get<std::string>());
    else toast(a, "Telemetry source updated");
    refresh_telemetry(a);
}

void on_tele_apply_lines(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    json lines = json::array();
    for (auto* e : a->tele_lines) lines.push_back(gtk_editable_get_text(GTK_EDITABLE(e)));
    json r = call(a, "set_telemetry", {{"lines", lines}});
    if (r.contains("__error")) toast(a, r["__error"].get<std::string>());
    else toast(a, "MFD layout updated");
}

void on_tele_show(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "set_page", {{"page", "telemetry"}});
    toast(a, "MFD switched to telemetry");
}

void on_rule_add(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    if (!a->profile.contains("telemetry_rules") || !a->profile["telemetry_rules"].is_array()) {
        a->profile["telemetry_rules"] = json::array();
    }
    a->profile["telemetry_rules"].push_back(
        json{{"when", "gear < 0.05"}, {"led", "t3"}, {"color", "red"}, {"enabled", true}});
    rebuild_rule_rows(a);
}

void on_rule_apply(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    json rules = json::array();
    for (std::size_t i = 0; i < a->rule_condition.size(); ++i) {
        std::string cond = gtk_editable_get_text(GTK_EDITABLE(a->rule_condition[i]));
        if (cond.empty()) continue;
        guint led = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->rule_led[i]));
        guint color = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->rule_color[i]));
        rules.push_back(json{{"when", cond},
                             {"led", kRuleLedNames[led]},
                             {"color", kRuleColors[color]},
                             {"enabled", true}});
    }
    json r = call(a, "set_telemetry", {{"rules", rules}});
    if (r.contains("__error")) {
        toast(a, r["__error"].get<std::string>());
        return;
    }
    a->profile["telemetry_rules"] = rules;
    toast(a, "LED rules applied");
}

void rebuild_rule_rows(App* a) {
    if (!a->tele_rules_box) return;
    a->rule_condition.clear();
    a->rule_led.clear();
    a->rule_color.clear();

    GtkWidget* child = gtk_widget_get_first_child(a->tele_rules_box);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(a->tele_rules_box), child);
        child = next;
    }

    json rules = a->profile.contains("telemetry_rules") && a->profile["telemetry_rules"].is_array()
                     ? a->profile["telemetry_rules"]
                     : json::array();

    for (const auto& rule : rules) {
        GtkWidget* rowbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
        gtk_widget_add_css_class(rowbox, "row");

        GtkWidget* when = gtk_entry_new();
        gtk_editable_set_text(GTK_EDITABLE(when), rule.value("when", std::string()).c_str());
        gtk_entry_set_placeholder_text(GTK_ENTRY(when), "gear < 0.05");
        gtk_widget_set_hexpand(when, TRUE);
        gtk_box_append(GTK_BOX(rowbox), when);

        GtkWidget* arrow = gtk_label_new("→");
        gtk_widget_add_css_class(arrow, "row-sub");
        gtk_box_append(GTK_BOX(rowbox), arrow);

        GtkWidget* led = gtk_drop_down_new_from_strings(kRuleLedNames);
        std::string led_value = rule.value("led", std::string("fire"));
        for (int i = 0; kRuleLedNames[i]; ++i) {
            if (led_value == kRuleLedNames[i]) gtk_drop_down_set_selected(GTK_DROP_DOWN(led), i);
        }
        gtk_widget_set_size_request(led, 110, -1);
        gtk_box_append(GTK_BOX(rowbox), led);

        GtkWidget* color = gtk_drop_down_new_from_strings(kRuleColors);
        std::string color_value = rule.value("color", std::string("amber"));
        for (int i = 0; kRuleColors[i]; ++i) {
            if (color_value == kRuleColors[i]) gtk_drop_down_set_selected(GTK_DROP_DOWN(color), i);
        }
        gtk_widget_set_size_request(color, 100, -1);
        gtk_box_append(GTK_BOX(rowbox), color);

        a->rule_condition.push_back(when);
        a->rule_led.push_back(led);
        a->rule_color.push_back(color);
        gtk_box_append(GTK_BOX(a->tele_rules_box), rowbox);
    }
}

void refresh_telemetry(App* a) {
    json t = call(a, "telemetry");
    if (t.contains("__error")) return;

    bool valid = t.value("valid", false);
    bool connected = t.value("connected", false);
    if (a->tele_pill) {
        if (valid) set_pill(a->tele_pill, "pill-ok", "live");
        else if (connected) set_pill(a->tele_pill, "pill-warn", "no flight");
        else set_pill(a->tele_pill, "pill-bad", "offline");
    }
    if (a->tele_status) {
        std::string text = valid ? "receiving telemetry"
                                 : t.value("error", std::string("no data"));
        gtk_label_set_text(GTK_LABEL(a->tele_status), text.c_str());
    }
    if (a->tele_values) {
        std::string text;
        if (valid) {
            char buf[900];
            std::snprintf(buf, sizeof(buf),
                          "ias %.0f   tas %.0f   alt %.0f\nmach %.2f   g %.1f   aoa %.1f\n"
                          "vy %.1f   gear %.2f   fuel %.0f%%",
                          t["values"].value("ias", 0.0), t["values"].value("tas", 0.0),
                          t["values"].value("alt", 0.0), t["values"].value("mach", 0.0),
                          t["values"].value("g", 0.0), t["values"].value("aoa", 0.0),
                          t["values"].value("vy", 0.0), t["values"].value("gear", 0.0),
                          t["values"].value("fuel_pct", 0.0));
            text = buf;
        } else {
            text = "waiting for the game…";
        }
        gtk_label_set_text(GTK_LABEL(a->tele_values), text.c_str());
    }
    if (t.contains("lines") && t["lines"].is_array()) {
        for (int i = 0; i < 3; ++i) {
            if (!a->tele_lines[i]) continue;
            std::string line = t["lines"][i].get<std::string>();
            if (line.empty()) continue;
            /* do not overwrite what the user is typing */ (void)line;
        }
    }
}

GtkWidget* build_telemetry(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("Telemetry",
                                "Flight data from a running game, shown on the MFD and LEDs.",
                                &body);

    GtkWidget* source_card = card("SOURCE", "read-only local API, no game hooks");
    GtkWidget* drop = gtk_drop_down_new_from_strings(kTelemetrySources);
    gtk_widget_set_size_request(drop, 170, -1);
    a->tele_source = drop;
    a->tele_pill = gtk_label_new("offline");
    set_pill(a->tele_pill, "pill-bad", "offline");
    GtkWidget* src_ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(src_ctl), a->tele_pill);
    gtk_box_append(GTK_BOX(src_ctl), drop);
    add_row(source_card, row("Game", "War Thunder: localhost:8111, needs a battle", src_ctl));

    a->tele_status = gtk_label_new("idle");
    gtk_widget_add_css_class(a->tele_status, "row-sub");
    add_row(source_card, row("Status", nullptr, a->tele_status));
    gtk_box_append(GTK_BOX(body), source_card);

    GtkWidget* live = card("LIVE VALUES", nullptr);
    a->tele_values = gtk_label_new("waiting for the game…");
    gtk_widget_add_css_class(a->tele_values, "mono");
    gtk_label_set_xalign(GTK_LABEL(a->tele_values), 0.0f);
    gtk_widget_set_margin_start(a->tele_values, 18);
    gtk_widget_set_margin_end(a->tele_values, 18);
    gtk_widget_set_margin_bottom(a->tele_values, 14);
    gtk_box_append(GTK_BOX(live), a->tele_values);
    gtk_box_append(GTK_BOX(body), live);

    GtkWidget* layout = card("MFD LAYOUT",
                             "{ias} {alt} {mach} {g} {gear} {fuel_pct} {compass} {vy} …");
    for (int i = 0; i < 3; ++i) {
        GtkWidget* e = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(e), 32);
        gtk_editable_set_width_chars(GTK_EDITABLE(e), 24);
        a->tele_lines[i] = e;
        char title[16];
        std::snprintf(title, sizeof(title), "Line %d", i + 1);
        add_row(layout, row(title, nullptr, e));
    }
    GtkWidget* layout_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* apply = gtk_button_new_with_label("Apply layout");
    gtk_widget_add_css_class(apply, "suggested-action");
    GtkWidget* show = gtk_button_new_with_label("Show on MFD");
    gtk_box_append(GTK_BOX(layout_buttons), apply);
    gtk_box_append(GTK_BOX(layout_buttons), show);
    add_row(layout, row("Apply", "32 characters per line", layout_buttons));
    g_signal_connect(apply, "clicked", G_CALLBACK(on_tele_apply_lines), a);
    g_signal_connect(show, "clicked", G_CALLBACK(on_tele_show), a);
    gtk_box_append(GTK_BOX(body), layout);

    GtkWidget* rules = card("LED RULES",
                            "condition on telemetry → LED colour; later rules win");
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    a->tele_rules_box = box;
    gtk_box_append(GTK_BOX(rules), box);
    GtkWidget* rule_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget* add = gtk_button_new_with_label("Add rule");
    GtkWidget* apply_rules = gtk_button_new_with_label("Apply rules");
    gtk_widget_add_css_class(apply_rules, "suggested-action");
    gtk_box_append(GTK_BOX(rule_buttons), add);
    gtk_box_append(GTK_BOX(rule_buttons), apply_rules);
    add_row(rules, row("Example", "gear < 0.05 and alt > 100", rule_buttons));
    g_signal_connect(add, "clicked", G_CALLBACK(on_rule_add), a);
    g_signal_connect(apply_rules, "clicked", G_CALLBACK(on_rule_apply), a);
    gtk_box_append(GTK_BOX(body), rules);

    g_signal_connect(drop, "notify::selected", G_CALLBACK(on_tele_source), a);
    return page;
}

GtkWidget* build_settings(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("Settings",
                                "Startup behaviour, persistence and a check of the USB link.",
                                &body);

    GtkWidget* startup = card("STARTUP", nullptr);

    GtkWidget* autostart = gtk_switch_new();
    g_object_set_data(G_OBJECT(autostart), "key", const_cast<char*>("autostart"));
    g_signal_connect(autostart, "notify::active", G_CALLBACK(on_setting_toggled), a);
    a->autostart_switch = autostart;
    add_row(startup, row("Start with the system",
                         "adds an entry to ~/.config/autostart", autostart));

    GtkWidget* minimized = gtk_switch_new();
    g_object_set_data(G_OBJECT(minimized), "key", const_cast<char*>("start_minimized"));
    g_signal_connect(minimized, "notify::active", G_CALLBACK(on_setting_toggled), a);
    a->minimized_switch = minimized;
    add_row(startup, row("Start minimised", "the daemon keeps running, the window stays hidden",
                         minimized));

    GtkWidget* verify = gtk_switch_new();
    g_object_set_data(G_OBJECT(verify), "key", const_cast<char*>("verify_link"));
    g_signal_connect(verify, "notify::active", G_CALLBACK(on_setting_toggled), a);
    a->verify_switch = verify;
    add_row(startup, row("Verify the link at startup", "one quick control-transfer check", verify));
    gtk_box_append(GTK_BOX(body), startup);

    GtkWidget* persist = card("PERSISTENCE",
                             "changes are written a moment after you stop editing");
    GtkWidget* autosave = gtk_switch_new();
    g_object_set_data(G_OBJECT(autosave), "key", const_cast<char*>("autosave"));
    g_signal_connect(autosave, "notify::active", G_CALLBACK(on_setting_toggled), a);
    a->autosave_switch = autosave;
    add_row(persist, row("Save automatically", "profile edits are written to disk", autosave));
    add_row(persist, row("Settings file", settings_path().c_str(), nullptr));
    add_row(persist, row("Profiles", profile_directory().c_str(), nullptr));
    gtk_box_append(GTK_BOX(body), persist);

    GtkWidget* link = card("USB LINK", "measured on the vendor control channel");
    GtkWidget* run = gtk_button_new_with_label("Run test");
    a->link_pill = gtk_label_new("not run");
    set_pill(a->link_pill, "pill-warn", "not run");
    GtkWidget* ctl = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_box_append(GTK_BOX(ctl), a->link_pill);
    gtk_box_append(GTK_BOX(ctl), run);
    add_row(link, row("Transmission", "20 control transfers, timed", ctl));
    a->link_detail = gtk_label_new("Run the test to measure write latency.");
    gtk_widget_add_css_class(a->link_detail, "mono");
    gtk_label_set_xalign(GTK_LABEL(a->link_detail), 0.0f);
    gtk_widget_set_margin_start(a->link_detail, 18);
    gtk_widget_set_margin_end(a->link_detail, 18);
    gtk_widget_set_margin_bottom(a->link_detail, 14);
    gtk_box_append(GTK_BOX(link), a->link_detail);
    g_signal_connect(run, "clicked", G_CALLBACK(on_link_test), a);
    gtk_box_append(GTK_BOX(body), link);

    GtkWidget* caps = card("WHAT THE HARDWARE CAN DO",
                           "everything this driver drives, including the parts the vendor UI hides");
    add_row(caps, row("Throttle display", "3 x 16 characters plus a clock/date area", nullptr));
    add_row(caps, row("LEDs", "11 LEDs, 9 of them bicolour (red / green / amber)", nullptr));
    add_row(caps, row("Clocks", "3 clocks: base + two minute offsets, 12/24 hour", nullptr));
    add_row(caps, row("Indicators", "shift lamp, throttle + POV blink module", nullptr));
    add_row(caps, row("POV hat", "8 directions, reported in the input report", nullptr));
    add_row(caps, row("Throttle ministick", "2 x 4-bit axes, exposed as a second hat", nullptr));
    add_row(caps, row("Vendor channel", "request 0x91, write-only: the device has no readback",
                      nullptr));
    add_row(caps, row("On-device profiles",
                      "the Windows driver exposes Toronto/Sydney/Programming interfaces for "
                      "downloading profiles into the firmware; not implemented here",
                      nullptr));
    gtk_box_append(GTK_BOX(body), caps);
    return page;
}

GtkWidget* build_profiles(App* a) {
    GtkWidget* body = nullptr;
    GtkWidget* page = page_root("Profiles",
                                "JSON files in ~/.config/x52pro/profiles. Click one to activate it.",
                                &body);

    GtkWidget* group = card("SAVED PROFILES", nullptr);
    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    g_signal_connect(list, "row-activated", G_CALLBACK(on_profile_activated), a);
    a->profile_list = list;
    gtk_box_append(GTK_BOX(group), list);
    gtk_box_append(GTK_BOX(body), group);

    GtkWidget* manage = card("MANAGE", nullptr);
    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), "My profile");
    a->profile_name = entry;
    add_row(manage, row("Profile name", "used when saving or creating", entry));

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_add_css_class(actions, "linked");
    GtkWidget* save = gtk_button_new_with_label("Save");
    GtkWidget* newp = gtk_button_new_with_label("New");
    GtkWidget* del = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(del, "destructive-action");
    g_signal_connect(save, "clicked", G_CALLBACK(on_profile_save), a);
    g_signal_connect(newp, "clicked", G_CALLBACK(on_profile_new), a);
    g_signal_connect(del, "clicked", G_CALLBACK(on_profile_delete), a);
    gtk_box_append(GTK_BOX(actions), save);
    gtk_box_append(GTK_BOX(actions), newp);
    gtk_box_append(GTK_BOX(actions), del);
    add_row(manage, row("Actions", "save overwrites the current name", actions));
    gtk_box_append(GTK_BOX(body), manage);

    GtkWidget* about = card("ABOUT", nullptr);
    add_row(about, row("Driver", "x52d · vendor protocol 0x91", nullptr));
    add_row(about, row("Control socket", default_socket_path().c_str(), nullptr));
    gtk_box_append(GTK_BOX(body), about);
    return page;
}

// ---------------------------------------------------------------------------
// window
// ---------------------------------------------------------------------------
GtkWidget* sidebar(App* a) {
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_add_css_class(box, "sidebar-pane");

    GtkWidget* head = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
    gtk_widget_set_margin_top(head, 18);
    gtk_widget_set_margin_bottom(head, 14);
    gtk_widget_set_margin_start(head, 18);
    GtkWidget* icon = gtk_image_new_from_icon_name("input-gaming-symbolic");
    gtk_image_set_pixel_size(GTK_IMAGE(icon), 22);
    gtk_box_append(GTK_BOX(head), icon);
    GtkWidget* titles = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    GtkWidget* t = gtk_label_new("X52 Pro");
    gtk_widget_add_css_class(t, "row-title");
    gtk_label_set_xalign(GTK_LABEL(t), 0.0f);
    GtkWidget* s = gtk_label_new("H.O.T.A.S.");
    gtk_widget_add_css_class(s, "row-sub");
    gtk_label_set_xalign(GTK_LABEL(s), 0.0f);
    gtk_box_append(GTK_BOX(titles), t);
    gtk_box_append(GTK_BOX(titles), s);
    gtk_box_append(GTK_BOX(head), titles);
    gtk_box_append(GTK_BOX(box), head);

    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_SINGLE);
    gtk_widget_add_css_class(list, "navigation-sidebar");
    gtk_widget_set_margin_start(list, 6);
    gtk_widget_set_margin_end(list, 6);

    struct Entry {
        const char* page;
        const char* icon;
        const char* label;
    };
    const Entry entries[] = {
        {"overview", "view-grid-symbolic", "Overview"},
        {"display", "video-display-symbolic", "Display"},
        {"leds", "preferences-color-symbolic", "LEDs"},
        {"buttons", "input-keyboard-symbolic", "Buttons"},
        {"test", "utilities-system-monitor-symbolic", "Test"},
        {"profiles", "folder-symbolic", "Profiles"},
        {"telemetry", "network-transmit-receive-symbolic", "Telemetry"},
        {"settings", "emblem-system-symbolic", "Settings"},
    };
    for (const auto& e : entries) {
        GtkWidget* r = gtk_list_box_row_new();
        GtkWidget* rowbox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
        gtk_widget_set_margin_top(rowbox, 7);
        gtk_widget_set_margin_bottom(rowbox, 7);
        GtkWidget* img = gtk_image_new_from_icon_name(e.icon);
        gtk_box_append(GTK_BOX(rowbox), img);
        GtkWidget* lbl = gtk_label_new(e.label);
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_box_append(GTK_BOX(rowbox), lbl);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(r), rowbox);
        g_object_set_data(G_OBJECT(r), "page", const_cast<char*>(e.page));
        gtk_list_box_append(GTK_LIST_BOX(list), r);
    }

    g_signal_connect(list, "row-selected", G_CALLBACK(+[](GtkListBox*, GtkListBoxRow* r, gpointer d) {
                         App* app = static_cast<App*>(d);
                         if (!r) return;
                         const char* page =
                             static_cast<const char*>(g_object_get_data(G_OBJECT(r), "page"));
                         if (page) adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(app->stack), page);
                     }),
                     a);

    GtkWidget* scroller = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scroller), GTK_POLICY_NEVER,
                                   GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroller), list);
    gtk_widget_set_vexpand(scroller, TRUE);
    gtk_box_append(GTK_BOX(box), scroller);
    return box;
}

void dump_tree(GtkWidget* w, int depth) {
    if (depth > 9) return;
    GtkAllocation alloc{};
    gtk_widget_get_allocation(w, &alloc);
    std::fprintf(stderr, "%*s%s %dx%d vis=%d\n", depth * 2, "", G_OBJECT_TYPE_NAME(w),
                 alloc.width, alloc.height, gtk_widget_get_visible(w) ? 1 : 0);
    for (GtkWidget* c = gtk_widget_get_first_child(w); c; c = gtk_widget_get_next_sibling(c)) {
        dump_tree(c, depth + 1);
    }
}

gboolean screenshot_later(gpointer data) {
    App* a = static_cast<App*>(data);
    const char* path = std::getenv("X52PANEL_SHOT");
    if (!path) return G_SOURCE_REMOVE;

    GtkWidget* w = GTK_WIDGET(a->window);
    int width = gtk_widget_get_width(w);
    int height = gtk_widget_get_height(w);
    GdkPaintable* paintable = GDK_PAINTABLE(gtk_widget_paintable_new(w));

    GtkSnapshot* snapshot = gtk_snapshot_new();
    gdk_paintable_snapshot(paintable, snapshot, width, height);
    GskRenderNode* node = gtk_snapshot_free_to_node(snapshot);

    GError* error = nullptr;
    GskRenderer* renderer = gsk_cairo_renderer_new();
    if (!gsk_renderer_realize(renderer, nullptr, &error)) {
        std::fprintf(stderr, "shot: realize failed: %s\n", error ? error->message : "?");
        return G_SOURCE_REMOVE;
    }
    graphene_rect_t rect = GRAPHENE_RECT_INIT(0, 0, (float)width, (float)height);
    GdkTexture* texture = gsk_renderer_render_texture(renderer, node, &rect);
    if (texture) {
        gdk_texture_save_to_png(texture, path);
        std::fprintf(stderr, "shot: wrote %s (%dx%d)\n", path, width, height);
    } else {
        std::fprintf(stderr, "shot: render_texture failed\n");
    }
    g_object_unref(texture);
    gsk_render_node_unref(node);
    gsk_renderer_unrealize(renderer);
    g_object_unref(renderer);
    g_object_unref(paintable);
    std::fflush(stderr);
    return G_SOURCE_REMOVE;
}

gboolean dump_tree_later(gpointer data) {
    App* a = static_cast<App*>(data);
    std::fprintf(stderr, "--- widget tree ---\n");
    dump_tree(GTK_WIDGET(a->window), 0);
    std::fflush(stderr);
    return G_SOURCE_REMOVE;
}

void handle_event(App* a, const std::string& line) {
    json ev;
    try {
        ev = json::parse(line);
    } catch (const std::exception&) {
        return;
    }
    if (!ev.contains("event")) return;
    const std::string kind = ev["event"].get<std::string>();
    if (kind == "input") {
        a->last_frame_us = static_cast<uint64_t>(g_get_monotonic_time());
        apply_input(a, ev["data"]);
    } else if (kind == "state") {
        a->last_frame_us = static_cast<uint64_t>(g_get_monotonic_time());
        apply_state(a, ev["data"]);
    }
}

gboolean on_stream_event(int fd, GIOCondition cond, gpointer data) {
    App* a = static_cast<App*>(data);
    if (cond & (G_IO_HUP | G_IO_ERR | G_IO_NVAL)) {
        a->stream_client.close();
        a->stream_watch = 0;
        return G_SOURCE_REMOVE;
    }
    std::string line;
    int budget = 400;  // bounded work per wakeup
    while (budget-- > 0 && a->stream_client.poll_line(line)) handle_event(a, line);
    return G_SOURCE_CONTINUE;
}

void connect_stream(App* a) {
    if (a->stream_client.is_connected()) return;
    if (!a->stream_client.connect(default_socket_path(), 800)) return;
    json reply;
    std::string error;
    if (!a->stream_client.request("subscribe", {{"stream", true}}, reply, error)) {
        a->stream_client.close();
        return;
    }
    a->stream_client.set_nonblocking(true);
    a->stream_watch = g_unix_fd_add(a->stream_client.fd(), G_IO_IN, on_stream_event, a);
    a->last_frame_us = static_cast<uint64_t>(g_get_monotonic_time());
}

void update_state(App* a) {
    json st = call(a, "get_state");
    if (st.contains("__error")) {
        set_pill(a->pill, "pill-bad", "offline");
        if (a->window_subtitle) {
            gtk_label_set_text(GTK_LABEL(a->window_subtitle), "daemon not running");
        }
        return;
    }
    apply_state(a, st);
}

gboolean on_poll(gpointer data) {
    App* a = static_cast<App*>(data);
    uint64_t now = static_cast<uint64_t>(g_get_monotonic_time());
    if (!a->stream_client.is_connected()) connect_stream(a);
    // Fallback only: normal updates arrive as pushed frames.
    if (a->last_frame_us == 0 || now - a->last_frame_us > 2500000ull) update_state(a);
    refresh_telemetry(a);
    return G_SOURCE_CONTINUE;
}

void on_refresh_clicked(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "reconnect");
    if (const char* page = std::getenv("X52PANEL_PAGE")) {
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(a->stack), page);
    }

    refresh_profile(a);
    reload_profiles(a);
    rebuild_bindings(a);
    load_settings(a);
    rebuild_rule_rows(a);

    // Telemetry source + layout come from the profile.
    if (a->profile.contains("settings")) {
        const json& ps = a->profile["settings"];
        std::string src = ps.value("telemetry_source", std::string("none"));
        if (a->tele_source) {
            for (int i = 0; kTelemetrySourceIds[i]; ++i) {
                if (src == kTelemetrySourceIds[i]) {
                    g_signal_handlers_block_by_func(a->tele_source, (gpointer)on_tele_source, a);
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->tele_source), i);
                    g_signal_handlers_unblock_by_func(a->tele_source, (gpointer)on_tele_source, a);
                }
            }
        }
        if (ps.contains("telemetry_lines") && ps["telemetry_lines"].is_array()) {
            for (int i = 0; i < 3 && i < static_cast<int>(ps["telemetry_lines"].size()); ++i) {
                if (!a->tele_lines[i]) continue;
                gtk_editable_set_text(GTK_EDITABLE(a->tele_lines[i]),
                                      ps["telemetry_lines"][i].get<std::string>().c_str());
            }
        }
    }
    refresh_telemetry(a);
    update_state(a);
    toast(a, "Reconnected");
}

void on_activate(GtkApplication* app, gpointer data) {
    App* a = static_cast<App*>(data);
    if (a->window) {
        // Second launch: just bring the existing window forward.
        gtk_window_present(GTK_WINDOW(a->window));
        return;
    }
    a->app = app;

    css_install();

    a->window = GTK_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(a->window), "X52 Pro");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 1080, 760);

    AdwToastOverlay* overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    g_object_set_data(G_OBJECT(a->window), "overlay", overlay);

    GtkWidget* toolbar = adw_toolbar_view_new();
    AdwHeaderBar* header = ADW_HEADER_BAR(adw_header_bar_new());

    GtkWidget* title_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_valign(title_box, GTK_ALIGN_CENTER);
    GtkWidget* title = gtk_label_new("X52 Pro");
    gtk_widget_add_css_class(title, "row-title");
    a->window_subtitle = gtk_label_new("connecting…");
    gtk_widget_add_css_class(a->window_subtitle, "row-sub");
    gtk_box_append(GTK_BOX(title_box), title);
    gtk_box_append(GTK_BOX(title_box), a->window_subtitle);
    adw_header_bar_set_title_widget(header, title_box);

    GtkWidget* refresh = gtk_button_new_from_icon_name("view-refresh-symbolic");
    gtk_widget_set_tooltip_text(refresh, "Reconnect and reload");
    g_signal_connect(refresh, "clicked", G_CALLBACK(on_refresh_clicked), a);
    adw_header_bar_pack_end(header, refresh);

    a->stack = adw_view_stack_new();
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_overview(a), "overview", "Overview");
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_display(a), "display", "Display");
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_leds(a), "leds", "LEDs");
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_buttons(a), "buttons", "Buttons");
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_test(a), "test", "Test");
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_profiles(a), "profiles", "Profiles");
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_telemetry(a), "telemetry", "Telemetry");
    adw_view_stack_add_titled(ADW_VIEW_STACK(a->stack), build_settings(a), "settings", "Settings");

    GtkWidget* split = adw_overlay_split_view_new();
    adw_overlay_split_view_set_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), sidebar(a));
    adw_overlay_split_view_set_content(ADW_OVERLAY_SPLIT_VIEW(split), a->stack);
    adw_overlay_split_view_set_show_sidebar(ADW_OVERLAY_SPLIT_VIEW(split), TRUE);
    adw_overlay_split_view_set_min_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split), 220);
    adw_overlay_split_view_set_max_sidebar_width(ADW_OVERLAY_SPLIT_VIEW(split), 260);

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(header));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), split);
    adw_toast_overlay_set_child(overlay, toolbar);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(a->window), GTK_WIDGET(overlay));

    if (const char* page = std::getenv("X52PANEL_PAGE")) {
        adw_view_stack_set_visible_child_name(ADW_VIEW_STACK(a->stack), page);
    }

    refresh_profile(a);
    reload_profiles(a);
    rebuild_bindings(a);
    load_settings(a);
    rebuild_rule_rows(a);

    // Telemetry source + layout come from the profile.
    if (a->profile.contains("settings")) {
        const json& ps = a->profile["settings"];
        std::string src = ps.value("telemetry_source", std::string("none"));
        if (a->tele_source) {
            for (int i = 0; kTelemetrySourceIds[i]; ++i) {
                if (src == kTelemetrySourceIds[i]) {
                    g_signal_handlers_block_by_func(a->tele_source, (gpointer)on_tele_source, a);
                    gtk_drop_down_set_selected(GTK_DROP_DOWN(a->tele_source), i);
                    g_signal_handlers_unblock_by_func(a->tele_source, (gpointer)on_tele_source, a);
                }
            }
        }
        if (ps.contains("telemetry_lines") && ps["telemetry_lines"].is_array()) {
            for (int i = 0; i < 3 && i < static_cast<int>(ps["telemetry_lines"].size()); ++i) {
                if (!a->tele_lines[i]) continue;
                gtk_editable_set_text(GTK_EDITABLE(a->tele_lines[i]),
                                      ps["telemetry_lines"][i].get<std::string>().c_str());
            }
        }
    }
    refresh_telemetry(a);
    update_state(a);

    for (std::size_t i = 0; i < kAxisCount; ++i) {
        a->last_frac[i] = -1.0;
        a->last_pct[i] = -1;
    }
    connect_stream(a);
    a->poll_id = g_timeout_add(1000, on_poll, a);
    if (std::getenv("X52PANEL_DUMP")) g_timeout_add(1500, dump_tree_later, a);
    if (std::getenv("X52PANEL_SHOT")) g_timeout_add(2200, screenshot_later, a);
    gtk_window_present(GTK_WINDOW(a->window));
}

void on_shutdown(GApplication*, gpointer data) {
    App* a = static_cast<App*>(data);
    if (a->poll_id) {
        g_source_remove(a->poll_id);
        a->poll_id = 0;
    }
    if (a->stream_watch) {
        g_source_remove(a->stream_watch);
        a->stream_watch = 0;
    }
    a->stream_client.close();
}

}  // namespace

int main(int argc, char** argv) {
    App app;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--minimized") == 0) app.start_minimized = true;
    }
    AdwApplication* application =
        adw_application_new("org.x52pro.Panel", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(on_shutdown), &app);
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
