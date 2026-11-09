// SPDX-License-Identifier: BSD-3-Clause
#include "x52/settings.hpp"

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace x52 {
namespace {

std::string home() {
    const char* h = std::getenv("HOME");
    return h ? std::string(h) : std::string(".");
}

std::string config_home() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return xdg;
    return home() + "/.config";
}

// Resolves the panel binary next to this executable, so the autostart entry
// works regardless of PATH.
std::string panel_command() {
    char buf[4096];
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        std::filesystem::path exe(buf);
        std::filesystem::path sibling = exe.parent_path() / "x52panel";
        if (std::filesystem::exists(sibling)) return sibling.string();
    }
    return "x52panel";
}

}  // namespace

std::string settings_directory() { return config_home() + "/x52pro"; }
std::string settings_path() { return settings_directory() + "/settings.json"; }
std::string autostart_path() { return config_home() + "/autostart/org.x52pro.Panel.desktop"; }

std::string AppSettings::to_json_string(int indent) const {
    nlohmann::json j;
    j["version"] = 1;
    j["autostart"] = autostart;
    j["start_minimized"] = start_minimized;
    j["verify_link"] = verify_link;
    j["virtual_joystick"] = virtual_joystick;
    j["autosave"] = autosave;
    j["last_profile"] = last_profile;
    j["last_mode"] = last_mode;
    j["last_page"] = last_page;
    return j.dump(indent);
}

bool AppSettings::from_json_string(const std::string& text, AppSettings& out) {
    try {
        nlohmann::json j = nlohmann::json::parse(text);
        if (!j.is_object()) return false;
        out.autostart = j.value("autostart", out.autostart);
        out.start_minimized = j.value("start_minimized", out.start_minimized);
        out.verify_link = j.value("verify_link", out.verify_link);
        out.virtual_joystick = j.value("virtual_joystick", out.virtual_joystick);
        out.autosave = j.value("autosave", out.autosave);
        out.last_profile = j.value("last_profile", out.last_profile);
        out.last_mode = j.value("last_mode", out.last_mode);
        out.last_page = j.value("last_page", out.last_page);
        if (out.last_mode < 1 || out.last_mode > 3) out.last_mode = 1;
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

bool AppSettings::save(std::string* error) const {
    std::error_code ec;
    std::filesystem::create_directories(settings_directory(), ec);
    std::filesystem::path tmp = settings_path() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot write " + tmp.string();
            return false;
        }
        f << to_json_string(2) << "\n";
        if (!f) {
            if (error) *error = "write failed for " + tmp.string();
            return false;
        }
    }
    // Atomic replace so a crash never leaves a truncated settings file.
    std::filesystem::rename(tmp, settings_path(), ec);
    if (ec) {
        if (error) *error = ec.message();
        return false;
    }
    return true;
}

bool AppSettings::load(std::string* error) {
    std::ifstream f(settings_path(), std::ios::binary);
    if (!f) {
        if (error) *error = "no settings file yet";
        return false;
    }
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string err;
    if (!from_json_string(ss.str(), *this)) {
        if (error) *error = "invalid settings file";
        return false;
    }
    (void)err;
    return true;
}

bool autostart_entry_exists() {
    std::error_code ec;
    return std::filesystem::exists(autostart_path(), ec);
}

bool write_autostart_entry(bool enabled, bool minimized, std::string* error) {
    std::error_code ec;
    if (!enabled) {
        std::filesystem::remove(autostart_path(), ec);
        if (ec) {
            if (error) *error = ec.message();
            return false;
        }
        return true;
    }

    std::filesystem::create_directories(std::filesystem::path(autostart_path()).parent_path(), ec);
    std::ofstream f(autostart_path(), std::ios::trunc);
    if (!f) {
        if (error) *error = "cannot write " + autostart_path();
        return false;
    }
    f << "[Desktop Entry]\n"
      << "Type=Application\n"
      << "Name=X52 Pro Control\n"
      << "Comment=Logitech/Saitek X52 Pro control panel\n"
      << "Exec=" << panel_command() << (minimized ? " --minimized" : "") << "\n"
      << "Icon=org.x52pro.Panel\n"
      << "Terminal=false\n"
      << "X-GNOME-Autostart-enabled=true\n"
      << "NoDisplay=false\n";
    if (!f) {
        if (error) *error = "write failed for " + autostart_path();
        return false;
    }
    return true;
}

}  // namespace x52
