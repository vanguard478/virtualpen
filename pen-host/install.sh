#!/bin/sh
# Builds and installs virtualpen-host, its udev rules and a systemd user unit.
# Run from the pen-host directory: ./install.sh
set -eu

cd "$(dirname "$0")"

if ! pkg-config --exists libusb-1.0 glib-2.0 libavcodec libswscale x11 xext xrandr; then
    echo "Installing build dependencies (sudo)..."
    sudo apt-get install -y build-essential cmake pkg-config libusb-1.0-0-dev libglib2.0-dev \
        libavcodec-dev libavutil-dev libswscale-dev libx11-dev libxext-dev libxrandr-dev
fi

cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
./build/pen-test

sudo install -m 0755 build/virtualpen-host /usr/local/bin/virtualpen-host
# Allows SCHED_FIFO for the host's single event thread.
sudo setcap cap_sys_nice+ep /usr/local/bin/virtualpen-host || echo "setcap failed; host will run without real-time priority"

sudo install -m 0644 70-virtualpen.rules /etc/udev/rules.d/70-virtualpen.rules
sudo udevadm control --reload
sudo udevadm trigger

mkdir -p "$HOME/.config/systemd/user"
install -m 0644 virtualpen-host.service "$HOME/.config/systemd/user/virtualpen-host.service"
systemctl --user daemon-reload

echo
echo "Installed. Replug the tablet, then either run 'virtualpen-host' in a terminal or"
echo "  systemctl --user enable --now virtualpen-host"
echo "(edit ~/.config/systemd/user/virtualpen-host.service to add options such as --screen)."
