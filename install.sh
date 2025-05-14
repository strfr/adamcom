#!/usr/bin/env bash
set -euo pipefail

# 1) Fetch the “latest release” metadata from GitHub’s API
API="https://api.github.com/repos/strfr/adamcom/releases/latest"
json=$(curl -sL "$API")

# 2) Extract the URL of the .deb asset (amd64 build)
deb_url=$(printf '%s\n' "$json" \
  | grep '"browser_download_url":' \
  | grep 'amd64.deb' \
  | head -n1 \
  | cut -d '"' -f4)

if [[ -z "$deb_url" ]]; then
  echo "❌ Could not find .deb download URL in GitHub release." >&2
  exit 1
fi

# 3) Download & install via apt (automatically resolves deps)
tmpdeb=$(mktemp --suffix=.deb)
trap 'rm -f "$tmpdeb"' EXIT

echo "⬇️  Downloading $deb_url"
curl -sL "$deb_url" -o "$tmpdeb"

echo "⚙️  Installing..."
sudo apt update
sudo apt install -y "$tmpdeb"

echo "✅ Installed latest adamcom from $deb_url"
