#include "monitors.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <glib.h>
#include <sys/wait.h>
#include <unistd.h>

#include "pen.h"

namespace vp {

namespace {

const char *const kStateType = "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})";

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

std::string tabletSettingsPath() {
    char path[96];
    snprintf(path, sizeof(path), "org.gnome.desktop.peripherals.tablet:/org/gnome/desktop/peripherals/tablets/%04x:%04x/",
             UINPUT_VENDOR, UINPUT_PRODUCT);
    return path;
}

// Runs argv, captures stdout. Returns the exit status, or -1 if it could not run.
int run(const std::vector<std::string> &args, std::string *out) {
    int fds[2];
    if (pipe(fds) != 0) return -1;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char *> argv;
        for (const std::string &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    char buf[4096];
    ssize_t n;
    while ((n = read(fds[0], buf, sizeof(buf))) > 0)
        if (out) out->append(buf, size_t(n));
    close(fds[0]);
    int status = 0;
    if (waitpid(pid, &status, 0) < 0 || !WIFEXITED(status)) return -1;
    return WEXITSTATUS(status);
}

} // namespace

bool parseDisplayState(const std::string &text, std::vector<Monitor> &out, std::string &err) {
    GError *error = nullptr;
    GVariant *state = g_variant_parse(G_VARIANT_TYPE(kStateType), text.c_str(), nullptr, nullptr, &error);
    if (!state) {
        err = std::string("cannot parse display state: ") + error->message;
        g_error_free(error);
        return false;
    }
    out.clear();

    GVariant *monitors = g_variant_get_child_value(state, 1);
    for (gsize i = 0, n = g_variant_n_children(monitors); i < n; i++) {
        GVariant *mon = g_variant_get_child_value(monitors, i);
        Monitor m;
        const char *connector, *vendor, *product, *serial;
        GVariant *spec = g_variant_get_child_value(mon, 0);
        g_variant_get(spec, "(&s&s&s&s)", &connector, &vendor, &product, &serial);
        m.connector = connector;
        m.vendor = vendor;
        m.product = product;
        m.serial = serial;
        g_variant_unref(spec);

        GVariant *modes = g_variant_get_child_value(mon, 1);
        for (gsize j = 0, nm = g_variant_n_children(modes); j < nm; j++) {
            GVariant *mode = g_variant_get_child_value(modes, j);
            GVariant *w = g_variant_get_child_value(mode, 1), *h = g_variant_get_child_value(mode, 2);
            GVariant *props = g_variant_get_child_value(mode, 6);
            gboolean current = FALSE;
            g_variant_lookup(props, "is-current", "b", &current);
            if (current) {
                m.width = g_variant_get_int32(w);
                m.height = g_variant_get_int32(h);
            }
            g_variant_unref(props);
            g_variant_unref(w);
            g_variant_unref(h);
            g_variant_unref(mode);
        }
        g_variant_unref(modes);

        GVariant *props = g_variant_get_child_value(mon, 2);
        const char *name = nullptr;
        if (g_variant_lookup(props, "display-name", "&s", &name)) m.name = name;
        gboolean builtin = FALSE;
        g_variant_lookup(props, "is-builtin", "b", &builtin);
        m.builtin = builtin;
        g_variant_unref(props);
        g_variant_unref(mon);
        out.push_back(m);
    }
    g_variant_unref(monitors);

    GVariant *logical = g_variant_get_child_value(state, 2);
    for (gsize i = 0, n = g_variant_n_children(logical); i < n; i++) {
        GVariant *lm = g_variant_get_child_value(logical, i);
        gint32 x, y;
        gdouble scale;
        guint32 transform;
        gboolean primary;
        GVariant *specs = nullptr, *props = nullptr;
        g_variant_get(lm, "(iidub@a(ssss)@a{sv})", &x, &y, &scale, &transform, &primary, &specs, &props);
        for (gsize j = 0, ns = g_variant_n_children(specs); j < ns; j++) {
            const char *connector;
            g_variant_get_child(specs, j, "(&s&s&s&s)", &connector, nullptr, nullptr, nullptr);
            for (Monitor &m : out) {
                if (m.connector != connector) continue;
                m.enabled = true;
                m.x = x;
                m.y = y;
                m.scale = scale;
                m.primary = primary;
                // Odd transforms rotate by 90 or 270 degrees.
                if (transform % 2 == 1) std::swap(m.width, m.height);
            }
        }
        g_variant_unref(specs);
        g_variant_unref(props);
        g_variant_unref(lm);
    }
    g_variant_unref(logical);
    g_variant_unref(state);
    return true;
}

bool queryMonitors(std::vector<Monitor> &out, std::string &err) {
    std::string text;
    int rc = run({"gdbus", "call", "--session", "--dest", "org.gnome.Mutter.DisplayConfig", "--object-path",
                  "/org/gnome/Mutter/DisplayConfig", "--method", "org.gnome.Mutter.DisplayConfig.GetCurrentState"},
                 &text);
    if (rc != 0) {
        err = rc < 0 || rc == 127 ? "cannot run gdbus (package libglib2.0-bin)"
                                  : "GNOME's display service did not answer; is this a GNOME session?";
        return false;
    }
    return parseDisplayState(text, out, err);
}

void printMonitors(const std::vector<Monitor> &monitors) {
    printf("  #  %-10s %-11s %-9s %-6s %s\n", "connector", "resolution", "position", "scale", "name");
    int i = 1;
    for (const Monitor &m : monitors) {
        char res[24], pos[24];
        snprintf(res, sizeof(res), "%dx%d", m.width, m.height);
        if (m.enabled) snprintf(pos, sizeof(pos), "%d,%d", m.x, m.y);
        else snprintf(pos, sizeof(pos), "off");
        printf("  %d  %-10s %-11s %-9s %-6.2f %s%s%s\n", i++, m.connector.c_str(), res, pos, m.scale,
               m.name.c_str(), m.primary ? " [primary]" : "", m.builtin ? " [built-in]" : "");
    }
}

const Monitor *findMonitor(const std::vector<Monitor> &monitors, const std::string &id, std::string &err) {
    std::string want = lower(id);
    for (const Monitor &m : monitors)
        if (lower(m.connector) == want) return &m;
    if (!want.empty() && std::all_of(want.begin(), want.end(), ::isdigit)) {
        size_t idx = size_t(std::stoul(want));
        if (idx >= 1 && idx <= monitors.size()) return &monitors[idx - 1];
        err = "there is no monitor number " + id;
        return nullptr;
    }
    for (const Monitor &m : monitors)
        if (lower(m.name) == want) return &m;
    const Monitor *hit = nullptr;
    int hits = 0;
    for (const Monitor &m : monitors) {
        if (lower(m.name).find(want) != std::string::npos ||
            lower(m.vendor + " " + m.product).find(want) != std::string::npos) {
            hit = &m;
            hits++;
        }
    }
    if (hits == 1) return hit;
    err = hits > 1 ? "\"" + id + "\" matches more than one monitor; use the connector name"
                   : "no monitor matches \"" + id + "\"";
    return nullptr;
}

bool pinTabletToMonitor(const Monitor *m, std::string &err) {
    const std::string path = tabletSettingsPath();
    int rc;
    if (!m) {
        rc = run({"gsettings", "reset", path, "output"}, nullptr);
    } else {
        // The 4th value (connector) lets mutter tell identical monitors apart.
        const char *edid[] = {m->vendor.c_str(), m->product.c_str(), m->serial.c_str(), m->connector.c_str(),
                              nullptr};
        GVariant *v = g_variant_ref_sink(g_variant_new_strv(edid, -1));
        gchar *text = g_variant_print(v, FALSE);
        rc = run({"gsettings", "set", path, "output", text}, nullptr);
        g_free(text);
        g_variant_unref(v);
        // keep-aspect would letterbox a second time on top of our own fit.
        if (rc == 0) rc = run({"gsettings", "set", path, "keep-aspect", "false"}, nullptr);
    }
    if (rc != 0) {
        err = rc < 0 || rc == 127 ? "cannot run gsettings (package libglib2.0-bin)"
                                  : "gsettings rejected the tablet mapping";
        return false;
    }
    return true;
}

} // namespace vp
