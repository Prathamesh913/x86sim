#!/bin/bash
# tests/arm64/gate/mapsroute.sh [LABEL] (Speed 27): Maps beyond its first screen, as on a Mac's simulator: search a place
# (Eiffel Tower), open its place card, Plan (directions), choose an origin by search (Louvre Museum), the route.
# Logs first: Maps' liveness (process list + its JIT log) is checked between steps, screens are read only while it lives.
# Taps are for an iPhone 17 (1206x2622). Run on a booted phone (lib.sh env: A64_DEVICE, O). Exit 0: route shown.
. "$(dirname "$0")/lib.sh"
L=${1:-mapsroute}; O=${O:-$SCR/gate/$L}; mkdir -p "$O"; mark_logs
alive() { local s; s=$(proc_state Maps); [ "$s" = alive ] || { say "FAIL $L: Maps $s at $1"; exit 1; }; }
# RAM 3: Maps in front before any input. In-gate (r3h1, r3ctl1) Maps was alive but the home screen stayed up
# (the open came 2 s after the kill, while SpringBoard still handled the death): the taps and typed text went to
# SpringBoard (typing on the home screen opens Spotlight, Return opens its top hit), Plan was never found, and Settings
# was left on Camera > Preserve Settings for the swipe-up / switcher checks. Wait for the old Maps to be gone, then
# reopen until the frame is not the home screen.
front() { local o; o=$(ocr); ! echo "$o" | grep -Eq "$HOMERE" && [ -n "$(pid_of Maps)" ]; }
old=$(pid_of Maps); [ -n "$old" ] && kill -9 $old 2>/dev/null
for i in $(seq 1 20); do [ -z "$(pid_of Maps)" ] && break; sleep 1; done; sleep 3
for i in 1 2 3; do
	home 4; app com.apple.Maps; wait_proc "$L" Maps 60 || exit 1; sleep 25; alive open
	front && break; say "$L: Maps running but the home screen is in front (try $i), opening again"
done
front || { cp "$O/now.png" "$O/$L-notfront.png"; say "FAIL $L: Maps never came to the front"; exit 2; }
touch_ dismiss
touch_ tap 500 1575; sleep 5; touch_ text "Eiffel Tower"; sleep 6; touch_ key return; sleep 15; alive search
# fileproviderd-vnode round: Return now opens the Eiffel Tower's place card directly (no result list); the old tap on
# the first result row then hit the map, closed the card, Plan was never found, and the origin tap below landed on
# Places > Work "Add" (the Louvre was saved as Work). Tap the result row only when no place card is up.
if ! ocr | grep -q Plan; then touch_ tap 500 520; sleep 20; fi; alive place
# Speed 36: confirm the Directions sheet (a Plan tap once landed on the place card's "Add a Note"); else close and retry
for i in 1 2 3 4 5; do
	front || { cp "$O/now.png" "$O/$L-notfront.png"; say "FAIL $L: home screen in front at Plan (try $i)"; exit 2; }
	# no Plan (the card closed or never opened): search again rather than only dismissing, which left a bare sheet
	tap_word Plan || { touch_ dismiss; sleep 3; touch_ tap 500 1575; sleep 5; touch_ text "Eiffel Tower"; sleep 6
		touch_ key return; sleep 15; continue; }
	sleep 15; ocr | grep -Eqi 'Directions|My Location|From' && break
	say "$L: no Directions sheet after Plan (try $i), closing it"; touch_ tap 113 301; sleep 2; touch_ dismiss; sleep 3
done; alive plan
ocr | grep -Eqi 'Directions|My Location|From' || { say "FAIL $L: no Directions sheet (Plan not found)"; exit 3; }
front || { say "FAIL $L: home screen in front at origin"; exit 2; }
touch_ tap 400 2130; sleep 6; touch_ text "Louvre Museum"; sleep 12; alive origin
touch_ tap 500 670; sleep 30; alive route
o=$(ocr); cp "$O/now.png" "$O/$L.png"
# RAM 3: a route card (its time and ETA, or the origin's name with Directions), not any 'go' in the map labels
if ! echo "$o" | grep -Eq "$HOMERE" && echo "$o" | grep -Eqi '[0-9]+ ?min|ETA|Louvre.*Directions|Directions.*Louvre'; then say "HIT $L route [${o:0:110}]"; exit 0; fi
say "FAIL $L: no route on screen [${o:0:110}]"; exit 4
