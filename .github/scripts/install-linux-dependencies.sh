#!/usr/bin/env bash
set -euo pipefail

sudo apt-get update
sudo apt-get install --yes --no-install-recommends \
  autoconf autoconf-archive automake libtool libltdl-dev \
  nasm yasm pkg-config python3-jinja2 python3-venv libclang-rt-dev \
  libasound2-dev libpulse-dev libaudio-dev libjack-dev libsndio-dev \
  libdbus-1-dev libibus-1.0-dev libudev-dev libpipewire-0.3-dev \
  libgl1-mesa-dev libgles2-mesa-dev libegl1-mesa-dev \
  libdrm-dev libgbm-dev \
  libx11-dev libxft-dev libxext-dev libxfixes-dev libxi-dev libxrandr-dev \
  libxrender-dev libxcursor-dev libxss-dev libxtst-dev \
  libxkbcommon-dev libwayland-dev libdecor-0-dev liburing-dev

dpkg-query --show libltdl-dev libclang-rt-dev
