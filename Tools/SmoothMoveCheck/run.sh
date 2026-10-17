#!/bin/sh
# Builds the real spline sources + smooth_check.cpp for Windows x64 (clang-cl + xwin) and runs it under Wine.
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${TMPDIR:-/tmp}/smooth_check
mkdir -p "$O"
X=/opt/xwin
/usr/lib/llvm-20/bin/clang-cl --target=x86_64-pc-windows-msvc /nologo /O2 /EHsc /MT -w /std:c++17 \
    /imsvc$X/crt/include /imsvc$X/sdk/include/ucrt /imsvc$X/sdk/include/um /imsvc$X/sdk/include/shared \
    -I"$R" /Fo"$O/" /Fe"$O/smooth_check.exe" \
    "$R/Tools/SmoothMoveCheck/smooth_check.cpp" "$R/Tools/SmoothMoveCheck/stubs.cpp" \
    "$R/Engine/Source/Tempest/C3Spline.cpp" "$R/Engine/Source/Tempest/C3Spline_CatmullRom.cpp" \
    "$R/Engine/Source/Tempest/C44Matrix.cpp" "$R/Engine/Source/Tempest/C3Vector.cpp" \
    -fuse-ld=lld /link /libpath:$X/crt/lib/x86_64 /libpath:$X/sdk/lib/um/x86_64 /libpath:$X/sdk/lib/ucrt/x86_64
WINEDEBUG=-all wine "$O/smooth_check.exe"
