#!/bin/sh
# Philtre's own offline tests, under ASan/UBSan, with the host's gcc. From anywhere.
#   test_editors.c   the curve editors
#   test_import.py   tools/serum2_import.py on synthetic Serum 2 presets (skipped without Zstandard for Python)
#   test_states.c    the imported states loaded into the engine and played
set -e
cd "$(dirname "$0")/.."
python3 ../../tools/gen_vst.py vst/vst.json --params-h >/dev/null
mkdir -p vst/build
CC="gcc -Wall -Wextra -fsanitize=address,undefined -fno-omit-frame-pointer -g -O1 -std=gnu11 -Isrc -Ivst/build -I../../wrapper"
SRC="src/philtre.c src/ph_wt.c src/ph_wav.c src/ph_fx.c -lm -lpthread"
$CC test/test_editors.c $SRC -o vst/build/test_editors
vst/build/test_editors
st=0
python3 test/test_import.py vst/build/import_states.txt || st=$?
if [ "$st" = 77 ]; then
    echo "test_import: skipped (no Zstandard: Python 3.14+ or pip install zstandard)"
elif [ "$st" != 0 ]; then
    exit "$st"
else
    $CC test/test_states.c $SRC -o vst/build/test_states
    vst/build/test_states vst/build/import_states.txt
fi
