#!/bin/bash
# kex-patch.sh: put the KEX patch on any Notepad++ version, add the KEX suite and build it.
#
#   bash kex-patch.sh [version] [workdir]      version: a tag like v8.9.8.1, or "latest" (default)
#
# Steps: clone that version, run codemod.py (adds views 3 and 4 to every per-view settings pair,
# including pairs added upstream since), then apply the hand-written patch with a 3-way merge.
# base/ holds the files the patch was written against, so git can merge even across versions.
# kex-icon.py then makes the app icon from the KEX logo (kex-logo.png), and the KEX suite (../suite) is
# copied in as PowerEditor/src/KexSuite and built into notepad++.exe.
# Exits 2 on a merge conflict, with the conflicting files listed; 4 if the build fails.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
VERSION=${1:-latest}
W=${2:-$HOME/npp-kex}
REPO=https://github.com/notepad-plus-plus/notepad-plus-plus.git
if [ "$VERSION" = latest ]; then
  VERSION=$(git ls-remote --tags --refs "$REPO" 'v*' | awk -F/ '{print $3}' | grep -E '^v[0-9.]+$' | sort -V | tail -1)
fi
echo "== Notepad++ $VERSION"
mkdir -p "$W"
SRC="$W/npp-$VERSION"
[ -d "$SRC" ] || git clone -q -c core.autocrlf=false --depth 1 --branch "$VERSION" "$REPO" "$SRC"
cd "$SRC"
git config user.name "KEX patch"; git config user.email "kex@localhost"
if ! git log -1 --format=%s | grep -q '^KEX'; then
  python3 "$HERE/codemod.py" .
  git commit -qam "KEX codemod for $VERSION"
  # restore the blobs the patch's index lines point at, so am -3 can do a real 3-way merge
  (cd "$HERE/base" && find . -type f | sed 's#^\./##') | while read -r f; do git hash-object -w "$HERE/base/$f" >/dev/null; done
  if ! git am --keep-cr -3 "$HERE"/0001-*.patch; then
    echo "== CONFLICT applying the KEX patch to $VERSION:"; git diff --name-only --diff-filter=U
    exit 2
  fi
fi
# the app icon, made from the KEX logo (kex-icon.py needs Pillow)
ICO=PowerEditor/src/icons/npp.ico
python3 "$HERE/kex-icon.py" "$HERE/kex-logo.png" "$W/npp-kex.ico"
if ! cmp -s "$W/npp-kex.ico" "$ICO"; then cp "$W/npp-kex.ico" "$ICO"; touch PowerEditor/src/Notepad_plus.rc; fi
# the suite is our own code, not a patch: always take the current copy
SUITE="$HERE/../suite"
rm -rf PowerEditor/src/KexSuite; mkdir -p PowerEditor/src/KexSuite
cp "$SUITE"/*.cpp "$SUITE"/*.h "$SUITE"/*.rc PowerEditor/src/KexSuite/
grep -qx 'PowerEditor/src/KexSuite/' .git/info/exclude 2>/dev/null || echo 'PowerEditor/src/KexSuite/' >> .git/info/exclude
echo "== patched: $SRC"
if [ "${KEX_BUILD:-1}" = 1 ]; then
  export PATH="${KEX_XBIN:-$HOME/npp-work/xbin}:$PATH"
  sh_gen="$W/gen-libsversion.sh"
  cat > "$sh_gen" <<'G'
R="$1"
sci=$(grep '#define VERSION_SCINTILLA ' "$R/scintilla/win32/ScintRes.rc" | awk '{print $3}')
lex=$(grep '#define VERSION_LEXILLA ' "$R/lexilla/src/LexillaVersion.rc" | awk '{print $3}')
bst=$(grep '#define BOOST_LIB_VERSION ' "$R/boostregex/boost/version.hpp" | awk '{print $3}')
printf '// NppLibsVersion.h\n#define NPP_SCINTILLA_VERSION %s\n#define NPP_LEXILLA_VERSION %s\n#define NPP_BOOST_REGEX_VERSION %s\n' "${sci:-\"N/A\"}" "${lex:-\"N/A\"}" "${bst:-\"N/A\"}" > "$R/PowerEditor/src/NppLibsVersion.h"
G
  # a build folder left by an earlier build doesn't have the suite's object folder yet
  for b in PowerEditor/gcc/bin.*.build; do [ -d "$b" ] && mkdir -p "$b/KexSuite"; done
  make -C PowerEditor/gcc -j"$(nproc)" PREBUILD_EVENT_CMD="sh $sh_gen $SRC" \
    CPP_DEFINE="UNICODE _UNICODE OEMRESOURCE NOMINMAX _WIN32_WINNT=_WIN32_WINNT_WIN7 NTDDI_VERSION=NTDDI_WIN7 NDEBUG STRSAFE_NO_DEPRECATE" ||
    { echo "== BUILD FAILED for $VERSION"; exit 4; }
  echo "== built: $SRC/PowerEditor/gcc/bin.gcc.x86_64/notepad++.exe"
fi
