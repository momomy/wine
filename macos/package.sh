#!/usr/bin/env zsh
# build.sh 결과물을 Whisky 런타임 패키지(Libraries.tar.gz)로 묶는다.
#
# Libraries/
#   Wine/{bin,lib,share}   ← make install-lib 결과 + x86_64 의존 dylib + mono/gecko
#   DXVK/{x32,x64}         ← DXVK-macOS (Whisky 의 DXVK 토글이 bottle 에 복사)
#   winetricks, verbs.txt
#   WhiskyWineVersion.plist
#
# GPTK(D3DMetal): GPTK_DIR(기본 <BUILD_ROOT>/../gptk3/redist, macos/fetch-gptk.sh 가 만든다)가 있으면 넣는다.
#   Wine/lib/external/{D3DMetal.framework,libd3dshared.dylib}
#   Wine/lib/gptk/wine/{x86_64-unix,x86_64-windows}/{d3d10,d3d11,d3d12,dxgi,atidxx64,nvapi64,nvngx,nvngx-on-metalfx}
# Wine 내장 DLL 을 덮어쓰지 않는다. 실행할 때 WINEDLLPATH_PREPEND 로 앞에 끼워 넣는다(docs/d3dmetal-port.md).
# GPTK_DIR=none 으로 제외할 수 있다.
#
# 사용법: macos/package.sh [BUILD_ROOT]
set -euo pipefail

SRC="${0:A:h:h}"
BUILD_ROOT="${1:-${SRC:h:h}/build/wine}"
DEPS="$BUILD_ROOT/deps/wine"
OUT="$BUILD_ROOT/Libraries"

[[ -x "$BUILD_ROOT/install/bin/wine" ]] || { echo "먼저 macos/build.sh 실행"; exit 1; }

rm -rf "$OUT" "$BUILD_ROOT/Libraries.tar.gz"
mkdir -p "$OUT/Wine"
cp -a "$BUILD_ROOT/install/." "$OUT/Wine/"
rm -rf "$OUT/Wine/share/man" "$OUT/Wine/include"

# x86_64 의존 dylib (freetype, gnutls, SDL2, vulkan-loader, MoltenVK 와 그 의존성)
for f in "$DEPS"/lib/*.dylib; do cp -a "$f" "$OUT/Wine/lib/"; done
# Vulkan ICD manifest: vulkan-loader 가 이것으로 MoltenVK 를 찾는다. 없으면 VK_KHR_surface 가 없어
# winevulkan/wined3d(vulkan)/DXVK/CEF 가 모두 GPU 를 못 쓴다.
# 로더의 첫 검색 경로는 "<실행 파일 디렉터리>/vulkan/icd.d" (= lib/wine/x86_64-unix/vulkan/icd.d) 이고
# library_path 는 json 파일 기준 상대 경로다.
mkdir -p "$OUT/Wine/lib/wine/x86_64-unix/vulkan/icd.d"
cat > "$OUT/Wine/lib/wine/x86_64-unix/vulkan/icd.d/MoltenVK_icd.json" <<'JSON'
{
    "file_format_version" : "1.0.0",
    "ICD": {
        "library_path": "../../../../libMoltenVK.dylib",
        "api_version" : "1.4.0",
        "is_portability_driver" : true
    }
}
JSON
# 공유 mono / gecko (prefix 마다 설치하지 않음)
mkdir -p "$OUT/Wine/share/wine"
cp -a "$DEPS/share/wine/mono" "$DEPS/share/wine/gecko" "$OUT/Wine/share/wine/"

cp -a "$SRC/macos/DXVK" "$OUT/DXVK"

GPTK_DIR="${GPTK_DIR:-${BUILD_ROOT:h}/gptk3/redist}"
if [[ -d "$GPTK_DIR/lib/external/D3DMetal.framework" ]]; then
  echo "==> GPTK 포함: $GPTK_DIR"
  mkdir -p "$OUT/Wine/lib/external" "$OUT/Wine/lib/gptk"
  cp -a "$GPTK_DIR/lib/external/." "$OUT/Wine/lib/external/"
  cp -a "$GPTK_DIR/lib/wine" "$OUT/Wine/lib/gptk/wine"
  # GPTK 3.0 의 *.so 는 ../../external/libd3dshared.dylib 심볼릭 링크다(2.1 은 rpath 로 같은 곳을 찾는다)
  ln -s ../external "$OUT/Wine/lib/gptk/external"
fi
cp -a "$SRC/macos/WhiskyWineVersion.plist" "$OUT/"

curl -fsSL -o "$OUT/winetricks" https://raw.githubusercontent.com/Winetricks/winetricks/master/src/winetricks
chmod +x "$OUT/winetricks"
curl -fsSL -o "$OUT/verbs.txt" https://raw.githubusercontent.com/Winetricks/winetricks/master/files/verbs/all.txt

( cd "$BUILD_ROOT" && find Libraries -type f -exec shasum {} + > SUMS.sha && tar -czf Libraries.tar.gz Libraries )
echo "==> $BUILD_ROOT/Libraries.tar.gz ($(du -h "$BUILD_ROOT/Libraries.tar.gz" | cut -f1))"
