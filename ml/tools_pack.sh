#!/bin/sh
# usage: pack.sh N   -> /home/claude/out/ML_M50_110_testN_ALL.zip (autoexec + MATCHING M50_110.sym)
set -e
B=/home/claude/src/ml/platform/M50.110/build
grep -q "^\.bss            0x00120000" $B/location.map || { echo "BSS NOT AT 0x120000 - fix size first"; exit 1; }
rm -rf /tmp/zpack && cp -r /tmp/z54 /tmp/zpack
cp $B/autoexec.bin /tmp/zpack/ && cp $B/M50_110.sym /tmp/zpack/ML/modules/M50_110.sym
cd /tmp/zpack && rm -f /home/claude/out/ML_M50_110_test$1_ALL.zip && zip -qr /home/claude/out/ML_M50_110_test$1_ALL.zip .
echo "packed test$1"
