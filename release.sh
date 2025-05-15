 
#!/usr/bin/env bash
set -euo pipefail

# Make sure we’re in the project root
cd "$(dirname "$0")"

# Require dpkg-parsechangelog and dch (from devscripts)
command -v dpkg-parsechangelog >/dev/null 2>&1 || {
  echo "Error: dpkg-parsechangelog not found. Install devscripts." >&2
  exit 1
}
command -v dch >/dev/null 2>&1 || {
  echo "Error: dch not found. Install devscripts." >&2
  exit 1
}

# 1. Read current version
CUR_VER=$(dpkg-parsechangelog --show-field Version)
# Split upstream and Debian revision
UPSTREAM=${CUR_VER%%-*}
DEBREV=${CUR_VER##*-}
# Compute new Debian revision
NEWDEBREV=$((DEBREV + 1))
NEW_VER="${UPSTREAM}-${NEWDEBREV}"

echo "Bumping version: $CUR_VER → $NEW_VER"

# 2. Update changelog
#   - Creates a new entry at top with version NEW_VER, distribution 'unstable',
#     today’s date, and commits message from args or prompt.
if [ $# -gt 0 ]; then
  MSG="$*"
else
  MSG="Release $NEW_VER"
fi
dch --newversion "$NEW_VER" --distribution unstable "$MSG"

# 3. Build the package without signing
echo "Building .deb..."
debuild -us -uc

echo "Done! New .deb files are in $(dirname "$(pwd)")"
