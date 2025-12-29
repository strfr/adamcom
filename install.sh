#!/usr/bin/env bash
set -euo pipefail

# ADAMCOM Installer
# Installs the latest release from GitHub

echo "=================================================================="
echo "                    ADAMCOM Installer                             "
echo "  Serial & CAN Terminal with Presets and Multi-Repeat Mode        "
echo "=================================================================="
echo ""

# Check for required dependencies
echo "[*] Checking dependencies..."
if ! command -v curl &> /dev/null; then
    echo "[X] curl is required but not installed." >&2
    echo "    Install with: sudo apt install curl" >&2
    exit 1
fi

# Check for readline (required at runtime)
if ! dpkg -s libreadline8 &> /dev/null 2>&1; then
    echo "[*] Installing libreadline..."
    sudo apt update
    sudo apt install -y libreadline8
fi

# 1) Fetch the "latest release" metadata from GitHub's API
API="https://api.github.com/repos/strfr/adamcom/releases/latest"
echo "[*] Fetching latest release info..."
json=$(curl -sL "$API")

# 2) Extract the URL of the .deb asset (amd64 build)
deb_url=$(printf '%s\n' "$json" \
  | grep '"browser_download_url":' \
  | grep 'amd64.deb' \
  | head -n1 \
  | cut -d '"' -f4)

if [[ -z "$deb_url" ]]; then
  echo "[!] Could not find .deb download URL in GitHub release." >&2
  echo "    Falling back to building from source..." >&2
  
  # Check for build dependencies
  if ! command -v g++ &> /dev/null; then
      echo "[*] Installing build dependencies..."
      sudo apt update
      sudo apt install -y build-essential libreadline-dev git
  fi
  
  # Build from source as fallback
  tmpdir=$(mktemp -d)
  trap 'rm -rf "$tmpdir"' EXIT
  
  echo "[*] Cloning repository..."
  git clone --depth 1 https://github.com/strfr/adamcom.git "$tmpdir/adamcom"
  
  echo "[*] Building..."
  cd "$tmpdir/adamcom"
  make
  
  echo "[*] Installing..."
  sudo make install
  
  echo ""
  echo "[OK] ADAMCOM installed successfully from source!"
  echo ""
  echo "Usage:"
  echo "  adamcom -d /dev/ttyUSB0 -b 115200   # Serial mode"
  echo "  adamcom -c can0 --canbitrate 500000 # CAN mode"
  echo ""
  echo "Press Ctrl-T for settings menu, /help for commands"
  exit 0
fi

# 3) Download & install via apt (automatically resolves deps)
tmpdeb=$(mktemp --suffix=.deb)
trap 'rm -f "$tmpdeb"' EXIT

echo "[*] Downloading $deb_url"
curl -sL "$deb_url" -o "$tmpdeb"

echo "[*] Installing..."
sudo apt update
sudo apt install -y "$tmpdeb"

echo ""
echo "[OK] ADAMCOM installed successfully!"
echo ""
echo "Usage:"
echo "  adamcom -d /dev/ttyUSB0 -b 115200   # Serial mode"
echo "  adamcom -c can0 --canbitrate 500000 # CAN mode"
echo ""
echo "Features:"
echo "  - 10 persistent presets (Alt+1-9,0)"
echo "  - Multi-preset repeat mode (/p N -r -t MS)"
echo "  - Interactive menu (Ctrl-T)"
echo "  - Slash commands (/help)"
echo ""
