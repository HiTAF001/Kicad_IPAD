#!/bin/bash
# Real ASan/UBSan pass over the PCB corpus (load, pick sweep, DRC, fabrication output, Gerber read-back)
S=${NETLOOM_WORK:-work}
K=$S/build-asan/kpcli
OUT=$S/asan_all.txt
: > $OUT
export UBSAN_OPTIONS=print_stacktrace=1 ASAN_OPTIONS=detect_leaks=1
n=0
while IFS= read -r f; do
  n=$((n+1))
  for cmd in "check" "picksweep" "drc" "fab"; do
    if [ $cmd = picksweep ]; then args="$f 5"; elif [ $cmd = fab ]; then rm -rf $S/asan_fab; args="$f $S/asan_fab"; else args="$f"; fi
    o=$(timeout 600 $K $cmd $args 2>&1); rc=$?
    if echo "$o" | grep -q "ERROR: AddressSanitizer\|runtime error\|LeakSanitizer"; then
      echo "== $cmd $f" >> $OUT; echo "$o" | grep -m6 "ERROR\|runtime error\|SUMMARY\|#[0-3] " >> $OUT
    elif [ $rc -ge 124 ]; then echo "== rc=$rc $cmd $f" >> $OUT; fi
  done
  if [ -d $S/asan_fab ]; then
    o=$(timeout 600 $K render $S/asan_fab $S/asan_fab.png 800 2>&1)
    if echo "$o" | grep -q "ERROR: AddressSanitizer\|runtime error"; then echo "== gerber $f" >> $OUT; echo "$o" | grep -m6 "ERROR\|runtime error\|#[0-3] " >> $OUT; fi
  fi
done < $S/corpus.txt
while IFS= read -r f; do
  o=$(timeout 300 $K render "$f" $S/asan_g.png 800 2>&1)
  if echo "$o" | grep -q "ERROR: AddressSanitizer\|runtime error"; then echo "== gfile $f" >> $OUT; echo "$o" | grep -m6 "ERROR\|runtime error\|#[0-3] " >> $OUT; fi
done < $S/gfiles.txt
echo "DONE boards=$n" >> $OUT
