#!/bin/bash
# ci/build.sh TAG OUT_DIR - everything the GitHub Actions build does, runnable on any Linux box:
#   1. clone Notepad++ TAG, apply the KEX patch, add the KEX suite and build notepad++.exe
#      (MinGW-w64 cross-compile)
#   2. put it into the official portable zip of that release -> OUT_DIR/Notepad++-<ver>-KEX.zip
#   3. run both test suites under Wine (skip with KEX_TEST=0)
# Exit codes: 2 = the patch doesn't apply to this release (needs updating), 4 = build failed, 3 = tests failed.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TAG=$1; VER=${TAG#v}
OUT=$(mkdir -p "$2" && cd "$2" && pwd); W=$OUT/work; mkdir -p "$W"

# the Notepad++ makefile calls plain g++/windres/...: point those at the MinGW-w64 tools
XBIN=$W/xbin; mkdir -p "$XBIN"
for t in ar dlltool objdump ranlib strip windres; do ln -sf "$(command -v x86_64-w64-mingw32-$t)" "$XBIN/$t"; done
ln -sf "$(command -v x86_64-w64-mingw32-g++-posix)" "$XBIN/g++"
ln -sf "$(command -v x86_64-w64-mingw32-gcc-posix)" "$XBIN/gcc"

KEX_XBIN=$XBIN bash "$ROOT/patch/kex-patch.sh" "$TAG" "$W"
SRC=$W/npp-$TAG
EXE=$SRC/PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe

NAME="Notepad++ $VER KEX"; PKG=$OUT/$NAME
rm -rf "$PKG"; mkdir -p "$PKG"
curl -fsSL -o "$W/portable.zip" \
  "https://github.com/notepad-plus-plus/notepad-plus-plus/releases/download/$TAG/npp.$VER.portable.x64.zip"
python3 -I -c 'import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])' "$W/portable.zip" "$PKG"
rm -rf "$PKG/updater"
cp "$EXE" "$PKG/notepad++.exe"
# placeholder for update scripts installed before the suite was built in (see ci/legacy-plugin-stub.c)
mkdir -p "$PKG/plugins/KexSuite"
x86_64-w64-mingw32-gcc -shared -s -o "$PKG/plugins/KexSuite/KexSuite.dll" "$ROOT/ci/legacy-plugin-stub.c"
cp "$ROOT"/windows/* "$PKG/"
echo "${KEX_REPO:-${GITHUB_REPOSITORY:-}}" > "$PKG/kex-repo.txt"
echo "$TAG" > "$PKG/kex-version.txt"
[ -f "$PKG/disableNppAutoUpdate.xml" ] ||
  echo '<!-- KEX: keeps the official updater from replacing the KEX build -->' > "$PKG/disableNppAutoUpdate.xml"
mkdir -p "$PKG/source"
if git -C "$ROOT" rev-parse HEAD >/dev/null 2>&1; then git -C "$ROOT" archive HEAD | tar -x -C "$PKG/source"
else cp -r "$ROOT"/{patch,suite,include,tests,windows,ci,README.md,CHANGELOG.md,LICENSE} "$PKG/source/" 2>/dev/null || true; rm -rf "$PKG/source/suite/test/screenshots"; fi

if [ "${KEX_TEST:-1}" = 1 ]; then
  bash "$ROOT/tests/run-tests.sh" "$PKG" "$SRC" "$OUT/test" || exit 3
fi

ZIP=$OUT/Notepad++-$VER-KEX.zip; rm -f "$ZIP"
python3 -I - "$OUT" "$NAME" "$ZIP" <<'PY'
import os, sys, zipfile
base, name, zpath = sys.argv[1:]
with zipfile.ZipFile(zpath, 'w', zipfile.ZIP_DEFLATED) as z:
    for d, _, files in os.walk(os.path.join(base, name)):
        for f in files:
            p = os.path.join(d, f)
            z.write(p, os.path.relpath(p, base))
PY
echo "== packaged: $ZIP"
