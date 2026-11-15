#!/usr/bin/env zsh
# WhiskyWine (WineHQ 기반) macOS 빌드 스크립트
#
# Apple Silicon 에서 sudo / x86_64 Homebrew 없이 x86_64 Wine 을 빌드한다.
#  - unix 측: Apple clang -arch x86_64 (configure 테스트 실행은 Rosetta 2)
#  - PE 측  : Homebrew llvm clang + lld (--enable-archs=i386,x86_64, new WoW64)
#  - 의존 라이브러리: 헤더는 ARM Homebrew, x86_64 dylib 은 Gcenx macOS_Wine_builds 번들에서 가져온다
#
# 사용법: macos/build.sh [BUILD_ROOT]   (기본: <repo>/../../build/wine)
# 필요  : brew install bison lld llvm freetype gnutls sdl2 pkgconf
set -euo pipefail

SRC="${0:A:h:h}"
BUILD_ROOT="${1:-${SRC:h:h}/build/wine}"
DEPS_VERSION="${DEPS_VERSION:-11.21}"          # Gcenx 릴리스 태그 (x86_64 dylib, mono, gecko 출처)
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
BREW="$(brew --prefix)"

DEPS_DIR="$BUILD_ROOT/deps"
OBJ_DIR="$BUILD_ROOT/obj"
INSTALL_DIR="$BUILD_ROOT/install"

# ---------------------------------------------------------------- 1. x86_64 의존성
if [[ ! -d "$DEPS_DIR/wine/lib" ]]; then
  echo "==> Gcenx wine-devel-$DEPS_VERSION 에서 x86_64 의존성 추출"
  mkdir -p "$DEPS_DIR"
  tarball="$DEPS_DIR/wine-devel-$DEPS_VERSION-osx64.tar.xz"
  [[ -f "$tarball" ]] || curl -fL -o "$tarball" \
    "https://github.com/Gcenx/macOS_Wine_builds/releases/download/$DEPS_VERSION/wine-devel-$DEPS_VERSION-osx64.tar.xz"
  tar -xJf "$tarball" -C "$DEPS_DIR"
  mv "$DEPS_DIR/Wine Devel.app/Contents/Resources/wine" "$DEPS_DIR/wine"
  rm -rf "$DEPS_DIR/Wine Devel.app"
fi
DEPS_LIB="$DEPS_DIR/wine/lib"

# ---------------------------------------------------------------- 2. 툴체인
export PATH="$BREW/opt/bison/bin:$BREW/opt/lld/bin:$BREW/opt/llvm/bin:$PATH"
export CC="/usr/bin/clang -arch x86_64"
export CXX="/usr/bin/clang++ -arch x86_64"
export MACOSX_DEPLOYMENT_TARGET="${MACOSX_DEPLOYMENT_TARGET:-11.0}"
export CFLAGS="-O2 -g0"
export CROSSCFLAGS="-O2 -g0"
export LDFLAGS="-Wl,-headerpad_max_install_names -Wl,-rpath,@loader_path/ -Wl,-rpath,@loader_path/../../"
# pkg-config 가 ARM 라이브러리 경로(-L/opt/homebrew/lib)를 주입하지 않도록 직접 지정
export PKG_CONFIG_LIBDIR="/nonexistent"
export FREETYPE_CFLAGS="-I$BREW/include/freetype2" FREETYPE_LIBS="-L$DEPS_LIB -lfreetype"
export GNUTLS_CFLAGS="-I$BREW/include -I$BREW/opt/gnutls/include -I$BREW/opt/nettle/include -I$BREW/opt/libtasn1/include -I$BREW/opt/p11-kit/include/p11-kit-1" GNUTLS_LIBS="-L$DEPS_LIB -lgnutls"
export SDL2_CFLAGS="-I$BREW/include/SDL2 -D_THREAD_SAFE" SDL2_LIBS="-L$DEPS_LIB -lSDL2"
# dlopen 할 soname (Gcenx 런타임과 동일: 맨 이름 + rpath)
export ac_cv_lib_soname_freetype="libfreetype.6.dylib"
export ac_cv_lib_soname_gnutls="libgnutls.30.dylib"
export ac_cv_lib_soname_SDL2="libSDL2-2.0.0.dylib"
export ac_cv_lib_soname_vulkan="libvulkan.1.dylib"
export ac_cv_lib_soname_MoltenVK="libMoltenVK.dylib"

# ---------------------------------------------------------------- 3. configure
mkdir -p "$OBJ_DIR"
cd "$OBJ_DIR"
# 빌드 도구(sfnt2fon 등)는 freetype 을 직접 링크하고 rpath @loader_path/../../ (= obj/) 로 찾는다
ln -sf "$DEPS_LIB"/*.dylib "$OBJ_DIR/"
if [[ ! -f Makefile || "${RECONFIGURE:-0}" == 1 ]]; then
  echo "==> configure"
  "$SRC/configure" \
    --prefix= \
    --build=x86_64-apple-darwin \
    --enable-archs=i386,x86_64 \
    --with-mingw="$BREW/opt/llvm/bin/clang" \
    --disable-tests \
    --disable-winebth_sys \
    --without-alsa --without-capi --with-coreaudio --with-cups --without-dbus \
    --without-ffmpeg --with-freetype --with-gettext --without-gettextpo --without-gphoto \
    --with-gnutls --without-gssapi --without-gstreamer --without-inotify --without-krb5 \
    --without-netapi --with-opencl --without-opengl --without-oss --with-pcap \
    --with-pthread --without-pulse --without-sane --with-sdl --without-udev \
    --without-usb --without-v4l2 --without-va --with-vulkan --without-wayland --without-x \
    2>&1 | tee configure.out
fi

# ---------------------------------------------------------------- 4. build / install
[[ "${CONFIGURE_ONLY:-0}" == 1 ]] && exit 0
echo "==> make -j$JOBS"
make -j"$JOBS"
rm -rf "$INSTALL_DIR"
make install-lib DESTDIR="$INSTALL_DIR" >/dev/null
echo "==> 완료: $INSTALL_DIR"
