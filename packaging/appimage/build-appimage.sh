#!/usr/bin/env bash
# Builds DreamStation-<version>-x86_64.AppImage from this checkout.
#
# Needs the build dependencies listed in README.md (libadwaitamm is optional:
# if it is not installed, it is built here, along with meson and mm-common
# when those are missing), plus curl, patchelf and Python 3.
# Everything is built under packaging/appimage/work; nothing is installed on
# the system and no root is needed.
#
# The AppImage runs on systems whose glibc is as new as, or newer than, the
# one it was built on, so build on the oldest distribution you want to reach.
# The compiler targets generic x86-64 whatever the build machine's CPU is.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
work=$here/work
prefix=$work/prefix
jobs=$(nproc)
version=$(sed -n 's/^ *VERSION *\([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt" | head -n1)
app_id=io.github.johnnygentoo.DreamStation
libadwaitamm_rev=326ea0f          # the revision DreamStation is tested with
generic_flags="-O2 -march=x86-64 -mtune=generic"

mkdir -p "$work" "$prefix"
cd "$work"
export PATH="$prefix/bin:$work/venv/bin:$PATH"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/lib64/pkgconfig:$prefix/share/pkgconfig:/usr/local/lib/pkgconfig:/usr/local/lib64/pkgconfig:${PKG_CONFIG_PATH:-}"
export LD_LIBRARY_PATH="$prefix/lib:$prefix/lib64:${LD_LIBRARY_PATH:-}"

step() { printf '\n==> %s\n' "$*"; }

if [ ! -e "$root/engine/flycast/CMakeLists.txt" ]; then
    echo "engine/flycast is empty: run 'git submodule update --init' first." >&2
    exit 1
fi

# ── libadwaitamm, if the system has none ────────────────────────────────────
if ! pkg-config --exists libadwaitamm-1; then
    if ! command -v meson >/dev/null; then
        step "meson (in a private Python environment)"
        python3 -m venv "$work/venv"
        "$work/venv/bin/pip" install --quiet meson ninja
    fi
    if ! command -v mm-common-get >/dev/null; then
        step "mm-common"
        [ -d mm-common ] || git clone --depth 1 https://gitlab.gnome.org/GNOME/mm-common.git
        [ -d mm-common/build ] || meson setup mm-common/build mm-common --prefix="$prefix"
        meson install -C mm-common/build
    fi
    # The binding generator (glibmm's gmmproc) is Perl and needs XML::Parser.
    export PERL5LIB="$prefix/perl5/lib/perl5:${PERL5LIB:-}"
    if ! perl -MXML::Parser -e1 2>/dev/null; then
        step "Perl XML::Parser"
        xml_parser=XML-Parser-2.47
        [ -d "$xml_parser" ] ||
            curl -fsSL "https://cpan.metacpan.org/authors/id/T/TO/TODDR/$xml_parser.tar.gz" | tar -xz
        (cd "$xml_parser" && perl Makefile.PL INSTALL_BASE="$prefix/perl5" >/dev/null &&
             make -j "$jobs" >/dev/null && make install >/dev/null)
    fi
    step "libadwaitamm $libadwaitamm_rev"
    [ -d libadwaitamm ] || git clone https://github.com/rofirrim/libadwaitamm.git
    git -C libadwaitamm checkout --quiet "$libadwaitamm_rev"
    rm -rf libadwaitamm/build
    CFLAGS="$generic_flags" CXXFLAGS="$generic_flags" \
        meson setup libadwaitamm/build libadwaitamm --prefix="$prefix" --libdir=lib \
            --buildtype=release -Dbuild-documentation=false -Dbuild-examples=false
    meson compile -C libadwaitamm/build -j "$jobs"
    meson install -C libadwaitamm/build
fi

# ── DreamStation ────────────────────────────────────────────────────────────
step "DreamStation $version"
cmake -S "$root" -B build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DDS_BUILD_TESTS=OFF -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_C_FLAGS="$generic_flags" -DCMAKE_CXX_FLAGS="$generic_flags"
cmake --build build -j "$jobs"
rm -rf AppDir
DESTDIR="$work/AppDir" cmake --install build --strip

# ── Bundle ──────────────────────────────────────────────────────────────────
step "AppImage"
fetch() { [ -x "$2" ] || { curl -fsSL -o "$2" "$1"; chmod +x "$2"; }; }
fetch https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-x86_64.AppImage \
      linuxdeploy-x86_64.AppImage
fetch https://raw.githubusercontent.com/linuxdeploy/linuxdeploy-plugin-gtk/master/linuxdeploy-plugin-gtk.sh \
      linuxdeploy-plugin-gtk.sh

export APPIMAGE_EXTRACT_AND_RUN=1        # no FUSE needed to run the tools
export DEPLOY_GTK_VERSION=4
export LINUXDEPLOY_OUTPUT_VERSION="$version"
export NO_STRIP=1                        # already stripped; linuxdeploy's strip is too old for some libraries
./linuxdeploy-x86_64.AppImage --appdir AppDir --plugin gtk \
    --desktop-file "AppDir/usr/share/applications/$app_id.desktop" \
    --icon-file "AppDir/usr/share/icons/hicolor/scalable/apps/$app_id.svg"

# The GTK plugin's start-up hook forces the Adwaita theme and the X11
# backend (workarounds for GTK 3). DreamStation is GTK 4: keep the user's
# theme unless APPIMAGE_GTK_THEME asks for another, and let GTK pick
# Wayland or X11 itself.
hook=AppDir/apprun-hooks/linuxdeploy-plugin-gtk.sh
sed -i -e '/^APPIMAGE_GTK_THEME=/d' \
       -e 's|^export GTK_THEME=.*|[ -n "${APPIMAGE_GTK_THEME:-}" ] \&\& export GTK_THEME="$APPIMAGE_GTK_THEME"|' \
       -e '/^export GDK_BACKEND=x11/d' "$hook"
grep -q GDK_BACKEND=x11 "$hook" && { echo "could not patch $hook" >&2; exit 1; }

./linuxdeploy-x86_64.AppImage --appdir AppDir --output appimage

out="DreamStation-$version-x86_64.AppImage"
mv -f DreamStation-*"$version"*-x86_64.AppImage "$out" 2>/dev/null || true
sha256sum "$out" > "$out.sha256"
step "Done: $work/$out"
