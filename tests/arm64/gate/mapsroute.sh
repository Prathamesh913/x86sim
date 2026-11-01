#!/bin/bash
# tests/arm64/gate/mapsroute.sh [LABEL] (Speed 27): Maps beyond its first screen, as on a Mac's simulator: search a place
# (Eiffel Tower), open its place card, Plan (directions), choose an origin by search (Louvre Museum), the route.
# Logs first: Maps' liveness (process list + its JIT log) is checked between steps, screens are read only while it lives.
# Taps are for an iPhone 17 (1206x2622). Run on a booted phone (lib.sh env: A64_DEVICE, O). Exit 0: route shown.
. "$(dirname "$0")/lib.sh"
L=${1:-mapsroute}; O=${O:-$SCR/gate/$L}; mkdir -p "$O"; mark_logs
alive() { local s; s=$(proc_state Maps); [ "$s" = alive ] || { say "FAIL $L: Maps $s at $1"; exit 1; }; }
kill -9 $(pid_of Maps) 2>/dev/null; sleep 2
app com.apple.Maps; wait_proc "$L" Maps 60 || exit 1; sleep 25; alive open; touch_ dismiss
touch_ tap 500 1575; sleep 5; touch_ text "Eiffel Tower"; sleep 6; touch_ raw "key return"; sleep 15; alive search
touch_ tap 500 520; sleep 20; alive place; touch_ dismiss; sleep 3
for i in 1 2 3 4 5; do tap_word Plan && break; touch_ dismiss; sleep 3; done; sleep 15; alive plan
touch_ tap 400 2130; sleep 6; touch_ text "Louvre Museum"; sleep 12; alive origin
touch_ tap 500 670; sleep 30; alive route
o=$(ocr); cp "$O/now.png" "$O/$L.png"
if echo "$o" | grep -Eqi 'min|km|Go|Louvre'; then say "HIT $L route [${o:0:110}]"; exit 0; fi
say "FAIL $L: no route on screen [${o:0:110}]"; exit 4
