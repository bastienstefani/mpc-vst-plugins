#!/bin/sh
# Offline test, then the device builds (armv7 + aarch64) and the skin. From this directory.
cd "$(dirname "$0")"
ROOT=../..
"$ROOT/tools/test_port.sh" vst/vst.json && "$ROOT/tools/build_port.sh" vst/vst.json
