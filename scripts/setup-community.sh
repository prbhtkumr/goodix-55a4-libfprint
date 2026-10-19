#!/usr/bin/env bash
# Build the community 55a4 driver in developer mode (keeps its work/ tree so it
# can be edited and rebuilt). Read-only towards the sensor and the system:
# no sudo, no installs, no NVM writes. System install afterwards is a separate,
# consented step (see README.md).
#
# Arch build deps (gcc/ninja/cmake/openssl/nss/cairo/glib2/opencv/libgudev/
# pixman are expected to be present already):
#   sudo pacman -S --needed --noconfirm doctest gobject-introspection \
#       libgusb libgusb-docs meson python-mako python-tqdm
set -euo pipefail

UPSTREAM=https://github.com/Hydrogell/goodix-27c6-55a4-fingerprint-linux
# Pinned for reproducibility (2026-08-30). Bump deliberately.
PIN="${PIN:-fe27b4812d60b4709d3dfb2b72f1e1f2704ede18}"

here="$(cd "$(dirname "$0")/.." && pwd)"

# The upstream tree must live in a colon-free directory: python -m venv
# refuses colon paths outright, PKG_CONFIG_PATH splits them at the colon, and
# other tooling will keep biting, so the entire upstream build tree lives in
# a sibling dir next to this repository. Override: GX55A4_WORKROOT.
workroot="${GX55A4_WORKROOT:-$(cd "$here/.." && pwd)/gx55a4-work}"
case "$workroot" in
    *:*) echo "ERROR: workroot must not contain ':': $workroot" >&2; exit 1 ;;
esac
repo="$workroot/goodix-27c6-55a4-fingerprint-linux"

mkdir -p "$workroot"
if [ ! -d "$repo/.git" ]; then
    git clone "$UPSTREAM" "$repo"
fi
git -C "$repo" fetch --quiet origin
git -C "$repo" checkout --quiet "$PIN"

# Verify essential host build dependencies
for cmd in meson ninja pkg-config; do
    command -v "$cmd" >/dev/null 2>&1 || { echo "ERROR: Missing build dependency: $cmd" >&2; exit 1; }
done
pkg-config --exists glib-2.0 libusb-1.0 || { echo "ERROR: Missing pkg-config deps: glib-2.0 or libusb-1.0" >&2; exit 1; }

# Arch ships OpenCV 5 as `opencv5.pc`; the patch's libfprint/sigfm/meson.build
# hardcodes dependency('opencv4'). No opencv4 package exists in Arch repos, so
# alias the .pc locally (nothing installed system-wide). Remove once the base
# fork or the patch names the opencv dep version-agnostically.
compat_pc="$workroot/pkgconfig-compat"
mkdir -p "$compat_pc"
if [ -f /usr/lib/pkgconfig/opencv5.pc ]; then
    sed 's/^Name: OpenCV$/Name: OpenCV (compat alias for opencv5)/' \
        /usr/lib/pkgconfig/opencv5.pc > "$compat_pc/opencv4.pc"
fi
export PKG_CONFIG_PATH="$compat_pc${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
echo "== pkg-config resolves opencv4 as: $(pkg-config --modversion opencv4 2>&1) =="

# Substitute the local 55a4 driver patch in place of Hydrogell's older
# patch so setup.sh applies and compiles it directly against pristine d1ca62a.
patch_file="$here/patches/libfprint-55a4-local-fixes.patch"
cp -f "$patch_file" "$repo/patches/55a4-driver.patch"

echo "== building at $PIN (this also clones TheWeirdDev/libfprint 55b4-experimental) =="
(cd "$repo" && ./setup.sh)

cat <<EOF

Build tree ready: $repo
Next steps (each is a separate, consented step; see README.md):
  read-only device probe : $repo/work/goodix-fp-dump/.venv/bin/python $repo/scripts/research/probe_psk.py
  system install         : sudo bash $repo/scripts/install-system.sh
  enrol / verify         : fprintd-enroll && fprintd-verify
EOF
