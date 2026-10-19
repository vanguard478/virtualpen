// Monitor discovery and tablet-to-monitor pinning for GNOME (mutter).
//
// Monitors are read from org.gnome.Mutter.DisplayConfig, the same source the
// GNOME Settings "Displays" panel uses, and the tablet is pinned by writing
// the per-device "output" key mutter's input mapper watches. Both go through
// the gdbus/gsettings tools: GLib refuses the session bus in a process with
// file capabilities (the host has cap_sys_nice), a child process does not.
#pragma once

#include <string>
#include <vector>

namespace vp {

struct Monitor {
    std::string connector; // e.g. "DP-1", "HDMI-1", "eDP-1"
    std::string vendor, product, serial; // EDID, as mutter matches them
    std::string name;      // display name as shown in GNOME Settings
    int width = 0, height = 0; // current mode in pixels, rotation applied
    int x = 0, y = 0;      // position in the desktop layout
    double scale = 1.0;
    bool enabled = false, primary = false, builtin = false;
};

// Parses the text form of a DisplayConfig.GetCurrentState reply (as printed
// by `gdbus call`).
bool parseDisplayState(const std::string &text, std::vector<Monitor> &out, std::string &err);

bool queryMonitors(std::vector<Monitor> &out, std::string &err);

void printMonitors(const std::vector<Monitor> &monitors);

// Matches a connector ("DP-1"), a 1-based index from printMonitors, or the
// display name (exact, or a unique substring). Case-insensitive.
const Monitor *findMonitor(const std::vector<Monitor> &monitors, const std::string &id, std::string &err);

// Pins the virtual tablet to the monitor, or with nullptr returns it to
// GNOME's automatic mapping. Persists in dconf until changed.
bool pinTabletToMonitor(const Monitor *monitor, std::string &err);

} // namespace vp
