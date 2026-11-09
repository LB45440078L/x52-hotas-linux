// SPDX-License-Identifier: BSD-3-Clause
//
// Application settings, persisted to ~/.config/x52pro/settings.json.
//
// Profiles hold everything that belongs to a controller configuration;
// AppSettings holds the things that belong to the installation and the last
// session (which profile was active, which mode, which MFD page) so a restart
// comes back exactly where the user left it.
//
#ifndef X52_SETTINGS_HPP
#define X52_SETTINGS_HPP

#include <string>

namespace x52 {

struct AppSettings {
    // Startup behaviour.
    bool autostart = false;         // install an XDG autostart entry
    bool start_minimized = false;   // start with the window hidden
    bool verify_link = true;        // run the USB link check at startup

    // Last session, restored on start.
    std::string last_profile = "Default";
    int last_mode = 1;
    std::string last_page = "status";

    // Persistence behaviour.
    bool autosave = true;           // save the profile shortly after a change

    std::string to_json_string(int indent = 2) const;
    static bool from_json_string(const std::string& text, AppSettings& out);

    bool save(std::string* error = nullptr) const;
    bool load(std::string* error = nullptr);
};

std::string settings_directory();     // ~/.config/x52pro
std::string settings_path();          // .../settings.json
std::string autostart_path();         // ~/.config/autostart/org.x52pro.Panel.desktop

// Writes or removes the autostart entry for the panel.
bool write_autostart_entry(bool enabled, bool minimized, std::string* error = nullptr);
bool autostart_entry_exists();

}  // namespace x52

#endif  // X52_SETTINGS_HPP
