// SPDX-License-Identifier: GPL-2.0-or-later
//
// x52panel - GTK4 / libadwaita control panel for the X52 Pro.
//
// The panel is a thin client: it talks to x52d over the control socket and
// never touches the hardware directly, so the daemon can keep running while
// the window is closed.
//
#include <adwaita.h>
#include <gtk/gtk.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "x52/client.hpp"
#include "x52/profile.hpp"
#include "x52/protocol.hpp"

using json = nlohmann::json;
using namespace x52;

namespace {

constexpr int kPollMs = 300;

const char* kActionNames[] = {"Passthrough", "None", "Key", "Mouse button", "Wheel",
                              "Macro", "Switch mode", "MFD page", "Run command",
                              "Joystick button", nullptr};
const char* kActionIds[] = {"passthrough", "none", "key", "mouse", "wheel",
                            "macro", "mode", "mfd_page", "exec", "button"};
const char* kPageNames[] = {"Clock", "Stopwatch", "Status", "Custom text", "Profile", nullptr};
const char* kPageIds[] = {"clock", "stopwatch", "status", "custom", "profile"};

struct App {
    GtkApplication* app = nullptr;
    GtkWindow* window = nullptr;

    GtkWidget* stack = nullptr;
    GtkWidget* status_label = nullptr;
    GtkWidget* status_pill = nullptr;
    GtkWidget* mfd_lines[3] = {nullptr, nullptr, nullptr};
    GtkWidget* axis_bars[kAxisCount] = {nullptr};
    GtkWidget* axis_labels[kAxisCount] = {nullptr};
    GtkWidget* mode_buttons[3] = {nullptr};
    GtkWidget* page_drop = nullptr;
    GtkWidget* led_rows[static_cast<int>(Led::Count)] = {nullptr};

    // display page
    GtkWidget* text_entries[3] = {nullptr, nullptr, nullptr};
    GtkWidget* mfd_bright = nullptr;
    GtkWidget* led_bright = nullptr;
    GtkWidget* clock24 = nullptr;
    GtkWidget* offs2 = nullptr;
    GtkWidget* offs3 = nullptr;
    GtkWidget* stopwatch_label = nullptr;

    // buttons page
    GtkWidget* layer_mode_drop = nullptr;
    GtkWidget* shift_switch = nullptr;
    GtkWidget* button_rows[kButtonCount] = {nullptr};
    GtkWidget* button_action[kButtonCount] = {nullptr};
    GtkWidget* button_arg[kButtonCount] = {nullptr};
    GtkWidget* button_apply = nullptr;
    GtkWidget* buttons_box = nullptr;

    // profiles page
    GtkWidget* profile_list = nullptr;
    GtkWidget* profile_name_entry = nullptr;

    Client client;
    json profile = json::object();
    json state = json::object();
    guint poll_id = 0;
    bool suppress = false;
};

void css_install() {
    const char* css =
        ".lcd { background-color: #07160c; border-radius: 10px; padding: 14px 18px; }\n"
        ".lcd-line { color: #74ff88; font-family: monospace; font-size: 19px; "
        "letter-spacing: 3px; }\n"
        ".pill { border-radius: 999px; padding: 2px 10px; font-size: 12px; font-weight: bold; }\n"
        ".pill-ok { background-color: alpha(#33d17a, 0.22); color: #33d17a; }\n"
        ".pill-bad { background-color: alpha(#e01b24, 0.22); color: #ff6b6b; }\n"
        ".led-dot { min-width: 12px; min-height: 12px; border-radius: 999px; }\n"
        ".led-off { background-color: #555; }\n"
        ".led-red { background-color: #ff3b30; }\n"
        ".led-amber { background-color: #ff9f0a; }\n"
        ".led-green { background-color: #30d158; }\n"
        ".dim { opacity: 0.65; }\n";

    GtkCssProvider* provider = gtk_css_provider_new();
    gtk_css_provider_load_from_string(provider, css);
    gtk_style_context_add_provider_for_display(gdk_display_get_default(),
                                               GTK_STYLE_PROVIDER(provider),
                                               GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    g_object_unref(provider);
}

// ---------------------------------------------------------------------------
// daemon helpers
// ---------------------------------------------------------------------------
bool ensure_client(App* a) {
    if (a->client.is_connected()) return true;
    return a->client.connect(default_socket_path(), 800);
}

json call(App* a, const std::string& cmd, const json& args = json::object()) {
    json result;
    std::string error;
    if (!ensure_client(a)) {
        return json{{"__error", "daemon not reachable"}};
    }
    if (!a->client.request(cmd, args, result, error)) {
        return json{{"__error", error}};
    }
    return result;
}

void toast_error(App* a, const std::string& message) {
    if (!a->window) return;
    AdwToast* toast = adw_toast_new(message.c_str());
    AdwToastOverlay* overlay = ADW_TOAST_OVERLAY(g_object_get_data(G_OBJECT(a->window), "overlay"));
    if (overlay) adw_toast_overlay_add_toast(overlay, toast);
}

void refresh_profile(App* a) {
    json r = call(a, "profile", {{"action", "get"}});
    if (r.contains("__error")) return;
    if (r.contains("profile")) a->profile = r["profile"];
}

// ---------------------------------------------------------------------------
// widget helpers
// ---------------------------------------------------------------------------
GtkWidget* lcd_box(App* a) {
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

GtkWidget* row(const char* title, const char* subtitle) {
    AdwActionRow* r = ADW_ACTION_ROW(adw_action_row_new());
    adw_preferences_row_set_title(ADW_PREFERENCES_ROW(r), title);
    if (subtitle) adw_action_row_set_subtitle(r, subtitle);
    return GTK_WIDGET(r);
}

GtkWidget* slider(int min, int max, int value, GtkWidget** out) {
    GtkWidget* s = gtk_scale_new_with_range(GTK_ORIENTATION_HORIZONTAL, min, max, 1);
    gtk_range_set_value(GTK_RANGE(s), value);
    gtk_widget_set_size_request(s, 220, -1);
    gtk_scale_set_draw_value(GTK_SCALE(s), TRUE);
    gtk_scale_set_value_pos(GTK_SCALE(s), GTK_POS_RIGHT);
    *out = s;
    return s;
}

void set_pill(GtkWidget* label, bool ok, const char* text) {
    gtk_label_set_text(GTK_LABEL(label), text);
    gtk_widget_remove_css_class(label, "pill-ok");
    gtk_widget_remove_css_class(label, "pill-bad");
    gtk_widget_add_css_class(label, "pill");
    gtk_widget_add_css_class(label, ok ? "pill-ok" : "pill-bad");
}

// ---------------------------------------------------------------------------
// state -> UI
// ---------------------------------------------------------------------------
void update_state(App* a) {
    json st = call(a, "get_state");
    if (st.contains("__error")) {
        set_pill(a->status_pill, false, "daemon offline");
        gtk_label_set_text(GTK_LABEL(a->status_label), st["__error"].get<std::string>().c_str());
        return;
    }
    a->state = st;

    bool connected = st.value("connected", false);
    set_pill(a->status_pill, connected, connected ? "connected" : "no device");
    std::string subtitle = "Profile " + st.value("profile", std::string("-")) +
                           "  |  mode " + std::to_string(st.value("mode", 1));
    if (st.value("shift", false)) subtitle += " + shift";
    if (!st["last_error"].get<std::string>().empty()) {
        subtitle += "  |  " + st["last_error"].get<std::string>();
    }
    gtk_label_set_text(GTK_LABEL(a->status_label), subtitle.c_str());

    for (int i = 0; i < 3; ++i) {
        std::string line = st["mfd"][i].get<std::string>();
        if (line.size() < 16) line.append(16 - line.size(), ' ');
        gtk_label_set_text(GTK_LABEL(a->mfd_lines[i]), line.c_str());
    }

    for (std::size_t i = 0; i < kAxisCount; ++i) {
        double v = st["axes"][std::string(axis_name(static_cast<Axis>(i)))].get<double>();
        if (a->axis_bars[i]) gtk_progress_bar_set_fraction(GTK_PROGRESS_BAR(a->axis_bars[i]), v);
        if (a->axis_labels[i]) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%3d%%", static_cast<int>(v * 100));
            gtk_label_set_text(GTK_LABEL(a->axis_labels[i]), buf);
        }
    }

    int mode = st.value("mode", 1);
    for (int i = 0; i < 3; ++i) {
        if (!a->mode_buttons[i]) continue;
        if (i + 1 == mode) gtk_widget_add_css_class(a->mode_buttons[i], "suggested-action");
        else gtk_widget_remove_css_class(a->mode_buttons[i], "suggested-action");
    }

    if (a->stopwatch_label) {
        long ms = st["stopwatch"].value("elapsed_ms", 0L);
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%02ld:%02ld.%02ld  (%s)", ms / 60000,
                      (ms / 1000) % 60, (ms % 1000) / 10,
                      st["stopwatch"].value("running", false) ? "running" : "stopped");
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
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
void on_mode_clicked(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    int mode = GPOINTER_TO_INT(g_object_get_data(G_OBJECT(b), "mode"));
    json r = call(a, "set_mode", {{"mode", mode}});
    if (r.contains("__error")) toast_error(a, r["__error"].get<std::string>());
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
    for (auto* e : a->text_entries) {
        lines.push_back(gtk_editable_get_text(GTK_EDITABLE(e)));
    }
    json r = call(a, "set_text", {{"lines", lines}});
    if (r.contains("__error")) toast_error(a, r["__error"].get<std::string>());
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
    if (r.contains("__error")) toast_error(a, r["__error"].get<std::string>());
}

void on_led_color(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* led = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "led"));
    const char* color = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "color"));
    json r = call(a, "set_led", {{"led", led}, {"color", color}, {"persist", true}});
    if (r.contains("__error")) toast_error(a, r["__error"].get<std::string>());
    refresh_profile(a);
}

void on_clock_settings(GObject*, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    json args;
    args["h24"] = gtk_switch_get_active(GTK_SWITCH(a->clock24));
    args["clock2_offset"] = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->offs2)));
    args["clock3_offset"] = static_cast<int>(gtk_spin_button_get_value(GTK_SPIN_BUTTON(a->offs3)));
    call(a, "set_clock", args);
}

void on_clock_now(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "clock_now");
    toast_error(a, "Clock synchronised from the system");
}

void on_stopwatch(GtkButton* b, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* op = static_cast<const char*>(g_object_get_data(G_OBJECT(b), "op"));
    call(a, "stopwatch", {{"op", op}});
}

void on_shift_toggled(GObject* obj, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    call(a, "set_shift", {{"on", gtk_switch_get_active(GTK_SWITCH(obj))}});
}

// ---------------------------------------------------------------------------
// buttons page
// ---------------------------------------------------------------------------
std::string action_to_arg(const json& a) {
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

int action_type_index(const json& a) {
    std::string type = a.value("action", std::string("passthrough"));
    for (int i = 0; kActionIds[i]; ++i) {
        if (type == kActionIds[i]) return i;
    }
    return 0;
}

GtkWidget* make_button_row(App* a, int index, bool shift_layer);

void rebuild_button_rows(App* a);

void on_layer_changed(GObject*, GParamSpec*, gpointer data) {
    App* a = static_cast<App*>(data);
    rebuild_button_rows(a);
}

void on_apply_buttons(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    int mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->layer_mode_drop)) + 1;
    bool shift = gtk_switch_get_active(GTK_SWITCH(a->shift_switch));
    std::string key = shift ? "shift_buttons" : "buttons";
    std::string mode_key = std::to_string(mode);

    if (a->profile.is_null() || !a->profile.contains("modes")) {
        toast_error(a, "Profile not loaded");
        return;
    }

    json& layer = a->profile["modes"][mode_key];
    if (!layer.is_object()) layer = json::object();
    if (!layer.contains(key) || !layer[key].is_object()) layer[key] = json::object();

    for (int i = 0; i < static_cast<int>(kButtonCount); ++i) {
        GtkWidget* drop = a->button_action[i];
        GtkWidget* entry = a->button_arg[i];
        if (!drop) continue;
        guint sel = gtk_drop_down_get_selected(GTK_DROP_DOWN(drop));
        std::string type = kActionIds[sel];
        std::string arg = gtk_editable_get_text(GTK_EDITABLE(entry));
        std::string bkey = std::to_string(i);

        json action;
        if (type == "passthrough") {
            layer[key].erase(bkey);
            continue;
        }
        action["action"] = type;
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
        toast_error(a, r["__error"].get<std::string>());
        return;
    }
    refresh_profile(a);
    toast_error(a, "Bindings applied");
}

GtkWidget* make_button_row(App* a, int index, bool shift_layer) {
    int mode = gtk_drop_down_get_selected(GTK_DROP_DOWN(a->layer_mode_drop)) + 1;
    std::string mkey = std::to_string(mode);
    std::string layer_key = shift_layer ? "shift_buttons" : "buttons";

    json action = json{{"action", "passthrough"}};
    if (a->profile.contains("modes") && a->profile["modes"].contains(mkey) &&
        a->profile["modes"][mkey].contains(layer_key) &&
        a->profile["modes"][mkey][layer_key].contains(std::to_string(index))) {
        action = a->profile["modes"][mkey][layer_key][std::to_string(index)];
    }

    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(box, 2);
    gtk_widget_set_margin_bottom(box, 2);

    GtkWidget* label = gtk_label_new(button_name(static_cast<Button>(index)).data());
    gtk_widget_set_size_request(label, 130, -1);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0f);
    gtk_box_append(GTK_BOX(box), label);

    GtkWidget* drop = gtk_drop_down_new_from_strings(kActionNames);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(drop), action_type_index(action));
    gtk_widget_set_size_request(drop, 150, -1);
    gtk_box_append(GTK_BOX(box), drop);

    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), action_to_arg(action).c_str());
    gtk_widget_set_hexpand(entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(entry), "key / command / page / macro steps");
    gtk_box_append(GTK_BOX(box), entry);

    a->button_action[index] = drop;
    a->button_arg[index] = entry;
    return box;
}

void rebuild_button_rows(App* a) {
    if (!a->profile_list) return;
    GtkWidget* container = a->buttons_box;
    if (!container) return;
    GtkWidget* child = gtk_widget_get_first_child(container);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_box_remove(GTK_BOX(container), child);
        child = next;
    }
    bool shift = gtk_switch_get_active(GTK_SWITCH(a->shift_switch));
    for (int i = 0; i < static_cast<int>(kButtonCount); ++i) {
        gtk_box_append(GTK_BOX(container), make_button_row(a, i, shift));
    }
}

// ---------------------------------------------------------------------------
// profiles page
// ---------------------------------------------------------------------------
void reload_profile_list(App* a) {
    json r = call(a, "profile", {{"action", "list"}});
    if (r.contains("__error")) return;
    if (!a->profile_list) return;
    GtkWidget* child = gtk_widget_get_first_child(a->profile_list);
    while (child) {
        GtkWidget* next = gtk_widget_get_next_sibling(child);
        gtk_list_box_remove(GTK_LIST_BOX(a->profile_list), child);
        child = next;
    }
    std::string current = r.value("current", std::string());
    for (const auto& p : r["profiles"]) {
        std::string name = p.get<std::string>();
        GtkWidget* row = gtk_list_box_row_new();
        GtkWidget* lbl = gtk_label_new((name == current ? "\u25cf  " + name : "     " + name).c_str());
        gtk_label_set_xalign(GTK_LABEL(lbl), 0.0f);
        gtk_widget_set_margin_top(lbl, 6);
        gtk_widget_set_margin_bottom(lbl, 6);
        gtk_list_box_row_set_child(GTK_LIST_BOX_ROW(row), lbl);
        g_object_set_data_full(G_OBJECT(row), "name", g_strdup(name.c_str()), g_free);
        gtk_list_box_append(GTK_LIST_BOX(a->profile_list), row);
    }
}

void on_profile_activated(GtkListBox*, GtkListBoxRow* row, gpointer data) {
    App* a = static_cast<App*>(data);
    if (!row) return;
    const char* name = static_cast<const char*>(g_object_get_data(G_OBJECT(row), "name"));
    if (!name) return;
    json r = call(a, "profile", {{"action", "load"}, {"name", name}});
    if (r.contains("__error")) {
        toast_error(a, r["__error"].get<std::string>());
        return;
    }
    refresh_profile(a);
    reload_profile_list(a);
    toast_error(a, (std::string("Loaded ") + name).c_str());
}

void on_profile_save(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* name = gtk_editable_get_text(GTK_EDITABLE(a->profile_name_entry));
    if (!name || !*name) {
        toast_error(a, "Enter a profile name first");
        return;
    }
    json r = call(a, "profile", {{"action", "save"}, {"name", name}});
    if (r.contains("__error")) {
        toast_error(a, r["__error"].get<std::string>());
        return;
    }
    reload_profile_list(a);
    toast_error(a, (std::string("Saved ") + name).c_str());
}

void on_profile_new(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* name = gtk_editable_get_text(GTK_EDITABLE(a->profile_name_entry));
    if (!name || !*name) {
        toast_error(a, "Enter a name for the new profile");
        return;
    }
    json r = call(a, "profile", {{"action", "create"}, {"name", name}});
    if (r.contains("__error")) {
        toast_error(a, r["__error"].get<std::string>());
        return;
    }
    call(a, "profile", {{"action", "save"}, {"name", name}});
    refresh_profile(a);
    reload_profile_list(a);
    toast_error(a, "New profile created");
}

void on_profile_delete(GtkButton*, gpointer data) {
    App* a = static_cast<App*>(data);
    const char* name = gtk_editable_get_text(GTK_EDITABLE(a->profile_name_entry));
    if (!name || !*name) return;
    json r = call(a, "profile", {{"action", "delete"}, {"name", name}});
    if (r.contains("__error")) {
        toast_error(a, r["__error"].get<std::string>());
        return;
    }
    reload_profile_list(a);
    toast_error(a, "Profile deleted");
}

// ---------------------------------------------------------------------------
// page construction
// ---------------------------------------------------------------------------
GtkWidget* build_overview(App* a) {
    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup* device = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(device, "Device");
    GtkWidget* status_row = row("Logitech X52 Pro HOTAS", "Enumerating...");
    a->status_label = gtk_label_new("");
    gtk_widget_add_css_class(a->status_label, "dim");
    adw_action_row_add_suffix(ADW_ACTION_ROW(status_row), a->status_label);
    a->status_pill = gtk_label_new("offline");
    set_pill(a->status_pill, false, "offline");
    adw_action_row_add_suffix(ADW_ACTION_ROW(status_row), a->status_pill);
    adw_preferences_group_add(device, status_row);
    adw_preferences_page_add(page, device);

    AdwPreferencesGroup* display = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(display, "Throttle display");
    adw_preferences_group_set_description(display, "Live copy of the MFD");
    adw_preferences_group_add(display, lcd_box(a));
    adw_preferences_page_add(page, display);

    AdwPreferencesGroup* mode_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(mode_group, "Mode");
    GtkWidget* mode_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_margin_top(mode_box, 8);
    gtk_widget_set_margin_bottom(mode_box, 8);
    for (int i = 0; i < 3; ++i) {
        char label[16];
        std::snprintf(label, sizeof(label), "Mode %d", i + 1);
        GtkWidget* b = gtk_button_new_with_label(label);
        g_object_set_data(G_OBJECT(b), "mode", GINT_TO_POINTER(i + 1));
        g_signal_connect(b, "clicked", G_CALLBACK(on_mode_clicked), a);
        gtk_box_append(GTK_BOX(mode_box), b);
        a->mode_buttons[i] = b;
    }

    GtkWidget* page_drop = gtk_drop_down_new_from_strings(kPageNames);
    gtk_widget_set_size_request(page_drop, 160, -1);
    gtk_widget_set_hexpand(page_drop, TRUE);
    g_signal_connect(page_drop, "notify::selected", G_CALLBACK(on_page_changed), a);
    a->page_drop = page_drop;

    GtkWidget* mode_row = row("Active mode", "LED theme follows the mode selector");
    adw_action_row_add_suffix(ADW_ACTION_ROW(mode_row), mode_box);
    adw_preferences_group_add(mode_group, mode_row);

    GtkWidget* page_row = row("Display page", "What the MFD is showing right now");
    adw_action_row_add_suffix(ADW_ACTION_ROW(page_row), page_drop);
    adw_preferences_group_add(mode_group, page_row);
    adw_preferences_page_add(page, mode_group);

    AdwPreferencesGroup* axes = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(axes, "Axes");
    for (std::size_t i = 0; i < kAxisCount; ++i) {
        GtkWidget* r = row(std::string(axis_name(static_cast<Axis>(i))).c_str(), nullptr);
        GtkWidget* bar = gtk_progress_bar_new();
        gtk_widget_set_size_request(bar, 260, -1);
        gtk_progress_bar_set_show_text(GTK_PROGRESS_BAR(bar), FALSE);
        a->axis_bars[i] = bar;
        GtkWidget* value = gtk_label_new("  0%");
        a->axis_labels[i] = value;
        gtk_widget_add_css_class(value, "dim");
        adw_action_row_add_suffix(ADW_ACTION_ROW(r), bar);
        adw_action_row_add_suffix(ADW_ACTION_ROW(r), value);
        adw_preferences_group_add(axes, r);
    }
    adw_preferences_page_add(page, axes);

    return GTK_WIDGET(page);
}

GtkWidget* build_display(App* a) {
    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup* text = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(text, "Custom text");
    adw_preferences_group_set_description(text, "16 characters per line, ASCII");
    for (int i = 0; i < 3; ++i) {
        char title[32];
        std::snprintf(title, sizeof(title), "Line %d", i + 1);
        GtkWidget* r = row(title, nullptr);
        GtkWidget* e = gtk_entry_new();
        gtk_entry_set_max_length(GTK_ENTRY(e), 16);
        gtk_editable_set_width_chars(GTK_EDITABLE(e), 18);
        gtk_entry_set_placeholder_text(GTK_ENTRY(e), "line text");
        a->text_entries[i] = e;
        adw_action_row_add_suffix(ADW_ACTION_ROW(r), e);
        adw_preferences_group_add(text, r);
    }
    GtkWidget* apply = gtk_button_new_with_label("Write to display");
    gtk_widget_add_css_class(apply, "suggested-action");
    g_signal_connect(apply, "clicked", G_CALLBACK(on_text_apply), a);
    adw_preferences_group_add(text, apply);
    adw_preferences_page_add(page, text);

    AdwPreferencesGroup* light = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(light, "Brightness");
    GtkWidget* mfd_row = row("Display", "0 - 255");
    GtkWidget* mfd = nullptr;
    adw_action_row_add_suffix(ADW_ACTION_ROW(mfd_row), slider(0, 255, 180, &mfd));
    g_signal_connect(mfd, "value-changed", G_CALLBACK(on_bright_changed), a);
    a->mfd_bright = mfd;
    adw_preferences_group_add(light, mfd_row);

    GtkWidget* led_row = row("LEDs", "0 - 255");
    GtkWidget* led = nullptr;
    adw_action_row_add_suffix(ADW_ACTION_ROW(led_row), slider(0, 255, 180, &led));
    g_signal_connect(led, "value-changed", G_CALLBACK(on_led_bright_changed), a);
    a->led_bright = led;
    adw_preferences_group_add(light, led_row);
    adw_preferences_page_add(page, light);

    AdwPreferencesGroup* clock = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(clock, "Clock");
    adw_preferences_group_set_description(
        clock, "The device keeps the time itself; offsets drive clocks 2 and 3");

    GtkWidget* h24_row = row("24 hour format", nullptr);
    GtkWidget* h24 = gtk_switch_new();
    gtk_switch_set_active(GTK_SWITCH(h24), TRUE);
    adw_action_row_add_suffix(ADW_ACTION_ROW(h24_row), h24);
    g_signal_connect(h24, "notify::active", G_CALLBACK(on_clock_settings), a);
    a->clock24 = h24;
    adw_preferences_group_add(clock, h24_row);

    GtkWidget* o2 = row("Clock 2 offset", "minutes from the base clock");
    GtkWidget* s2 = gtk_spin_button_new_with_range(-1439, 1439, 15);
    adw_action_row_add_suffix(ADW_ACTION_ROW(o2), s2);
    g_signal_connect(s2, "value-changed", G_CALLBACK(+[](GtkSpinButton*, gpointer d) { on_clock_settings(nullptr, nullptr, d); }), a);
    a->offs2 = s2;
    adw_preferences_group_add(clock, o2);

    GtkWidget* o3 = row("Clock 3 offset", "minutes from the base clock");
    GtkWidget* s3 = gtk_spin_button_new_with_range(-1439, 1439, 15);
    adw_action_row_add_suffix(ADW_ACTION_ROW(o3), s3);
    g_signal_connect(s3, "value-changed", G_CALLBACK(+[](GtkSpinButton*, gpointer d) { on_clock_settings(nullptr, nullptr, d); }), a);
    a->offs3 = s3;
    adw_preferences_group_add(clock, o3);

    GtkWidget* sync = gtk_button_new_with_label("Set from system clock");
    g_signal_connect(sync, "clicked", G_CALLBACK(on_clock_now), a);
    GtkWidget* sync_row = row("Synchronise", "program date and time from this computer");
    adw_action_row_add_suffix(ADW_ACTION_ROW(sync_row), sync);
    adw_preferences_group_add(clock, sync_row);
    adw_preferences_page_add(page, clock);

    AdwPreferencesGroup* sw = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(sw, "Stopwatch");
    GtkWidget* sw_row = row("Elapsed", "host driven, shown on the MFD page");
    a->stopwatch_label = gtk_label_new("00:00.00");
    gtk_widget_add_css_class(a->stopwatch_label, "dim");
    adw_action_row_add_suffix(ADW_ACTION_ROW(sw_row), a->stopwatch_label);
    adw_preferences_group_add(sw, sw_row);

    GtkWidget* buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    const char* ops[] = {"start", "stop", "reset"};
    const char* labels[] = {"Start", "Stop", "Reset"};
    for (int i = 0; i < 3; ++i) {
        GtkWidget* b = gtk_button_new_with_label(labels[i]);
        g_object_set_data(G_OBJECT(b), "op", const_cast<char*>(ops[i]));
        g_signal_connect(b, "clicked", G_CALLBACK(on_stopwatch), a);
        gtk_box_append(GTK_BOX(buttons), b);
    }
    GtkWidget* sw_btns = row("Controls", nullptr);
    adw_action_row_add_suffix(ADW_ACTION_ROW(sw_btns), buttons);
    adw_preferences_group_add(sw, sw_btns);
    adw_preferences_page_add(page, sw);

    return GTK_WIDGET(page);
}

GtkWidget* build_leds(App* a) {
    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup* quick = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(quick, "All LEDs");
    GtkWidget* box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    struct Preset {
        const char* label;
        const char* color;
    };
    const Preset presets[] = {{"Off", "off"}, {"Red", "red"}, {"Amber", "amber"}, {"Green", "green"}};
    for (const auto& p : presets) {
        GtkWidget* b = gtk_button_new_with_label(p.label);
        g_object_set_data(G_OBJECT(b), "color", const_cast<char*>(p.color));
        g_signal_connect(b, "clicked", G_CALLBACK(on_led_preset), a);
        gtk_box_append(GTK_BOX(box), b);
    }
    GtkWidget* quick_row = row("Preset", nullptr);
    adw_action_row_add_suffix(ADW_ACTION_ROW(quick_row), box);
    adw_preferences_group_add(quick, quick_row);
    adw_preferences_page_add(page, quick);

    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Individual LEDs");
    adw_preferences_group_set_description(group, "Changes are stored in the active mode");

    for (std::size_t i = 0; i < static_cast<std::size_t>(Led::Count); ++i) {
        Led led = static_cast<Led>(i);
        GtkWidget* r = row(std::string(led_name(led)).c_str(), nullptr);
        GtkWidget* btns = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
        const char* colors[] = {"off", "red", "amber", "green"};
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
        adw_action_row_add_suffix(ADW_ACTION_ROW(r), btns);
        adw_preferences_group_add(group, r);
    }
    adw_preferences_page_add(page, group);

    AdwPreferencesGroup* misc = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(misc, "Indicators");
    GtkWidget* shift_row = row("Shift indicator", "the dot on the MFD");
    GtkWidget* shift = gtk_switch_new();
    g_signal_connect(shift, "notify::active", G_CALLBACK(on_shift_toggled), a);
    adw_action_row_add_suffix(ADW_ACTION_ROW(shift_row), shift);
    adw_preferences_group_add(misc, shift_row);
    adw_preferences_page_add(page, misc);

    return GTK_WIDGET(page);
}

GtkWidget* build_buttons(App* a) {
    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup* header = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(header, "Bindings");

    GtkWidget* mode_row = row("Mode", "each mode has its own layer");
    static const char* kModeNames[] = {"Mode 1", "Mode 2", "Mode 3", nullptr};
    GtkWidget* mode_drop = gtk_drop_down_new_from_strings(kModeNames);
    gtk_drop_down_set_selected(GTK_DROP_DOWN(mode_drop), 0);
    g_signal_connect(mode_drop, "notify::selected", G_CALLBACK(on_layer_changed), a);
    a->layer_mode_drop = mode_drop;
    adw_action_row_add_suffix(ADW_ACTION_ROW(mode_row), mode_drop);
    adw_preferences_group_add(header, mode_row);

    GtkWidget* shift_row = row("Shift layer", "pinky switch held: second set of bindings");
    GtkWidget* shift = gtk_switch_new();
    g_signal_connect(shift, "notify::active", G_CALLBACK(on_layer_changed), a);
    a->shift_switch = shift;
    adw_action_row_add_suffix(ADW_ACTION_ROW(shift_row), shift);
    adw_preferences_group_add(header, shift_row);

    GtkWidget* apply = gtk_button_new_with_label("Apply bindings");
    gtk_widget_add_css_class(apply, "suggested-action");
    g_signal_connect(apply, "clicked", G_CALLBACK(on_apply_buttons), a);
    adw_preferences_group_add(header, apply);
    adw_preferences_page_add(page, header);

    AdwPreferencesGroup* list_group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(list_group, "Buttons");
    GtkWidget* container = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    a->buttons_box = container;
    adw_preferences_group_add(list_group, container);
    adw_preferences_page_add(page, list_group);

    return GTK_WIDGET(page);
}

GtkWidget* build_profiles(App* a) {
    AdwPreferencesPage* page = ADW_PREFERENCES_PAGE(adw_preferences_page_new());

    AdwPreferencesGroup* group = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(group, "Profiles");
    adw_preferences_group_set_description(group, "Click a profile to activate it");

    GtkWidget* list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(list), GTK_SELECTION_NONE);
    gtk_widget_add_css_class(list, "boxed-list");
    g_signal_connect(list, "row-activated", G_CALLBACK(on_profile_activated), a);
    a->profile_list = list;
    adw_preferences_group_add(group, list);

    GtkWidget* name_row = row("Profile name", "used when saving");
    GtkWidget* entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(entry), "My profile");
    adw_action_row_add_suffix(ADW_ACTION_ROW(name_row), entry);
    a->profile_name_entry = entry;
    adw_preferences_group_add(group, name_row);

    GtkWidget* actions = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
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
    GtkWidget* actions_row = row("Manage", nullptr);
    adw_action_row_add_suffix(ADW_ACTION_ROW(actions_row), actions);
    adw_preferences_group_add(group, actions_row);
    adw_preferences_page_add(page, group);

    AdwPreferencesGroup* about = ADW_PREFERENCES_GROUP(adw_preferences_group_new());
    adw_preferences_group_set_title(about, "About");
    adw_preferences_group_add(about, row("X52 Pro Linux driver", "protocol 0x91 vendor API"));
    adw_preferences_group_add(about, row("Daemon", default_socket_path().c_str()));
    adw_preferences_page_add(page, about);

    return GTK_WIDGET(page);
}

gboolean on_poll(gpointer data) {
    App* a = static_cast<App*>(data);
    update_state(a);
    return G_SOURCE_CONTINUE;
}

void on_activate(GtkApplication* app, gpointer data) {
    App* a = static_cast<App*>(data);
    a->app = app;

    css_install();

    a->window = GTK_WINDOW(adw_application_window_new(app));
    gtk_window_set_title(GTK_WINDOW(a->window), "X52 Pro");
    gtk_window_set_default_size(GTK_WINDOW(a->window), 980, 760);

    AdwToastOverlay* overlay = ADW_TOAST_OVERLAY(adw_toast_overlay_new());
    g_object_set_data(G_OBJECT(a->window), "overlay", overlay);

    GtkWidget* toolbar = adw_toolbar_view_new();
    AdwHeaderBar* header = ADW_HEADER_BAR(adw_header_bar_new());
    GtkWidget* stack = adw_view_stack_new();
    a->stack = stack;

    GtkWidget* overview = build_overview(a);
    GtkWidget* display = build_display(a);
    GtkWidget* leds = build_leds(a);
    GtkWidget* buttons = build_buttons(a);
    GtkWidget* profiles = build_profiles(a);

    adw_view_stack_add_titled(ADW_VIEW_STACK(stack), overview, "overview", "Overview");
    adw_view_stack_add_titled(ADW_VIEW_STACK(stack), display, "display", "Display");
    adw_view_stack_add_titled(ADW_VIEW_STACK(stack), leds, "leds", "LEDs");
    adw_view_stack_add_titled(ADW_VIEW_STACK(stack), buttons, "buttons", "Buttons");
    adw_view_stack_add_titled(ADW_VIEW_STACK(stack), profiles, "profiles", "Profiles");

    AdwViewSwitcherTitle* switcher = ADW_VIEW_SWITCHER_TITLE(adw_view_switcher_title_new());
    adw_view_switcher_title_set_stack(switcher, ADW_VIEW_STACK(stack));
    adw_header_bar_set_title_widget(header, GTK_WIDGET(switcher));

    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(toolbar), GTK_WIDGET(header));
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(toolbar), stack);
    adw_toast_overlay_set_child(overlay, toolbar);
    adw_application_window_set_content(ADW_APPLICATION_WINDOW(a->window), GTK_WIDGET(overlay));

    refresh_profile(a);
    reload_profile_list(a);
    rebuild_button_rows(a);
    update_state(a);

    a->poll_id = g_timeout_add(kPollMs, on_poll, a);

    gtk_window_present(GTK_WINDOW(a->window));
}

void on_shutdown(GApplication*, gpointer data) {
    App* a = static_cast<App*>(data);
    if (a->poll_id) g_source_remove(a->poll_id);
}

}  // namespace

int main(int argc, char** argv) {
    App app;
    AdwApplication* application =
        adw_application_new("org.x52pro.Panel", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(application, "activate", G_CALLBACK(on_activate), &app);
    g_signal_connect(application, "shutdown", G_CALLBACK(on_shutdown), &app);

    // The panel is a client: if the daemon is not there, say so instead of
    // silently showing an empty window.
    int status = g_application_run(G_APPLICATION(application), argc, argv);
    g_object_unref(application);
    return status;
}
