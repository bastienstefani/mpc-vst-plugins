#!/bin/sh
# The curve editors' offline test (test_editors.c), under ASan/UBSan, with the host's gcc. From anywhere.
set -e
cd "$(dirname "$0")/.."
python3 ../../tools/gen_vst.py vst/vst.json --params-h >/dev/null
mkdir -p vst/build
gcc -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -std=gnu11 -Isrc -Ivst/build -I../../wrapper \
    test/test_editors.c src/philtre.c src/ph_wt.c src/ph_wav.c src/ph_fx.c -lm -lpthread -o vst/build/test_editors
vst/build/test_editors
