#!/bin/sh
# Host-side check of the ActionCam maths (WoW/Source/UIUtil/ActionCamMath.h, ported from wxl-camera).
set -e
R=$(cd "$(dirname "$0")/../.." && pwd)
O=${TMPDIR:-/tmp}/actioncam_check
mkdir -p "$O"
g++ -std=c++17 -O2 -Wall -Wextra -I"$R" "$R/Tools/ActionCamCheck/math_test.cpp" -o "$O/math_test"
"$O/math_test"
