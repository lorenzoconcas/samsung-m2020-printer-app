#!/bin/sh
#
# Build the static dependencies of samsung-m2020-printer-app:
#
#   - libcups 2.4.x (static, with the PPD API that SpliX needs)
#   - PAPPL 1.4.x   (static)
#   - SpliX         (rastertoqpdl filter, built without JBIG: not needed by M2020)
#
# Everything is installed in $DEPS (default: ./build/deps); nothing is
# installed system-wide.  Works on macOS (Apple Silicon) and Linux (arm64/x86_64).
#
# Usage:
#   scripts/build-deps.sh                # install OS packages + build
#   SKIP_PKGS=1 scripts/build-deps.sh    # don't touch apt/brew
#

set -eu

TOP=$(cd "$(dirname "$0")/.." && pwd)
DEPS=${DEPS:-$TOP/build/deps}
SRC=${SRC:-$TOP/build/src}
OS=$(uname -s)

# Pinned, tested versions...
CUPS_URL=https://github.com/OpenPrinting/cups.git
CUPS_REF=${CUPS_REF:-a7a0f4fd4497180aed601836a3159f9f79e4375f}		# 2.4.x branch (2.4.21)
PAPPL_URL=https://github.com/michaelrsweet/pappl.git
PAPPL_REF=${PAPPL_REF:-v1.4.9}
SPLIX_URL=https://github.com/OpenPrinting/splix.git
SPLIX_REF=${SPLIX_REF:-7a802180c7b50802aaa16bbd2b3e326237286730}	# 2.0.2+ (M2020 support)

if [ "$OS" = Darwin ]; then
    JOBS=$(sysctl -n hw.ncpu)
else
    JOBS=$(nproc 2>/dev/null || echo 2)
fi

say() { printf '\n==> %s\n' "$*"; }

fetch() {
    # fetch DIR URL REF
    if [ ! -d "$1/.git" ]; then
        git init -q "$1"
        git -C "$1" remote add origin "$2"
    fi
    git -C "$1" fetch -q --depth 1 origin "$3"
    git -C "$1" checkout -q -f FETCH_HEAD
    git -C "$1" clean -qfdx
}


#
# 1. OS packages
#

if [ -z "${SKIP_PKGS:-}" ]; then
    say "Installing build dependencies"
    if [ "$OS" = Darwin ]; then
        command -v brew >/dev/null || { echo "Homebrew is required: https://brew.sh"; exit 1; }
        xcode-select -p >/dev/null 2>&1 || { echo "Run: xcode-select --install"; exit 1; }
        brew install pkgconf libusb libpng jpeg-turbo openssl@3
    else
        sudo apt-get update
        sudo apt-get install -y build-essential git pkg-config autoconf \
            libavahi-client-dev libusb-1.0-0-dev libssl-dev libpng-dev \
            libjpeg-dev zlib1g-dev
    fi
fi

if [ "$OS" = Darwin ]; then
    BREW=$(brew --prefix)
    for p in openssl@3 libusb libpng jpeg-turbo; do
        PKG_CONFIG_PATH="$BREW/opt/$p/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
    done
    CPPFLAGS="-I$BREW/opt/openssl@3/include ${CPPFLAGS:-}"
    LDFLAGS="-L$BREW/opt/openssl@3/lib ${LDFLAGS:-}"
    export CPPFLAGS LDFLAGS
fi

PKG_CONFIG_PATH="$DEPS/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export PKG_CONFIG_PATH

mkdir -p "$DEPS/lib/pkgconfig" "$DEPS/include/cups" "$DEPS/libexec/splix" "$SRC"


#
# 2. libcups (static)
#

say "Building libcups ($CUPS_REF)"
fetch "$SRC/cups" "$CUPS_URL" "$CUPS_REF"
(
    cd "$SRC/cups"
    ./configure --prefix="$DEPS" --libdir="$DEPS/lib" \
        --disable-shared --enable-static \
        --with-tls=openssl --with-dnssd=no \
        --disable-gssapi --disable-dbus >configure.log 2>&1 \
        || { tail -30 configure.log; exit 1; }
    make -j"$JOBS" -C cups libcups.a >make.log 2>&1 || { tail -30 make.log; exit 1; }
    cp cups/libcups.a "$DEPS/lib/"
    cp cups/*.h "$DEPS/include/cups/"
)

# pkg-config file for the *static* library: everything in Libs so that
# configure scripts that don't use --static still link.
CUPS_PRIV=$(grep '^Libs.private:' "$SRC/cups/cups.pc" | cut -d: -f2-)
CUPS_REQ=$(grep '^Requires.private:' "$SRC/cups/cups.pc" | cut -d: -f2-)
CUPS_REQ_LIBS=""
[ -n "$(echo $CUPS_REQ)" ] && CUPS_REQ_LIBS=$(pkg-config --libs $CUPS_REQ)
CUPS_VER=$(grep '^Version:' "$SRC/cups/cups.pc" | cut -d: -f2 | tr -d ' ')
EXTRA_LIBS="-lpthread -lm"
if [ "$OS" = Darwin ]; then
    EXTRA_LIBS="$EXTRA_LIBS -framework CoreFoundation -framework Security -framework SystemConfiguration -liconv -lresolv"
fi
cat >"$DEPS/lib/pkgconfig/cups.pc" <<EOF
prefix=$DEPS
includedir=\${prefix}/include
libdir=\${prefix}/lib
cups_serverroot=/etc/cups

Name: CUPS
Description: CUPS API Library (static, private build)
Version: $CUPS_VER
Cflags: -I\${includedir}
Libs: -L\${libdir} -lcups $CUPS_PRIV $CUPS_REQ_LIBS $EXTRA_LIBS
EOF


#
# 3. PAPPL (static)
#

say "Building PAPPL ($PAPPL_REF)"
fetch "$SRC/pappl" "$PAPPL_URL" "$PAPPL_REF"
(
    cd "$SRC/pappl"
    ./configure --prefix="$DEPS" --libdir="$DEPS/lib" --disable-shared \
        --with-tls=openssl >configure.log 2>&1 || { tail -30 configure.log; exit 1; }
    make -j"$JOBS" -C pappl >make.log 2>&1 || { tail -30 make.log; exit 1; }
    make -C pappl install >install.log 2>&1 || { tail -30 install.log; exit 1; }
)


#
# 4. SpliX rastertoqpdl
#

say "Building SpliX ($SPLIX_REF)"
fetch "$SRC/splix" "$SPLIX_URL" "$SPLIX_REF"
(
    cd "$SRC/splix"
    make -j"$JOBS" DISABLE_JBIG=1 \
        rastertoqpdl_LIBS="$(pkg-config --libs cups)" \
        rastertoqpdl_LDFLAGS="${LDFLAGS:-}" \
        pstoqpdl_LIBS="$(pkg-config --libs cups)" \
        pstoqpdl_LDFLAGS="${LDFLAGS:-}" >make.log 2>&1 \
        || { tail -30 make.log; exit 1; }
    cp optimized/rastertoqpdl "$DEPS/libexec/splix/rastertoqpdl"
)

say "Done. Dependencies are in $DEPS"
echo "Next:  make DEPS=$DEPS && sudo make install DEPS=$DEPS"
