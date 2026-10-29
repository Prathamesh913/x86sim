#!/bin/bash
# Boot the standard device for SECS seconds and summarise: cli/libexec/rundev.sh [extra jobs...] (`x86sim boot` wraps it)
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT; cd $X
D=$X86SIM_DEVICE; ST=$X86SIM_STATE
rm -rf $D/data/var/log/*.log $D/data/Library/Preferences/com.x86code*.plist
# x86Code: systemstatusd owns com.apple.systemstatus.publisher (feature flag SystemStatus/EmbeddedDaemon is on);
# without it SpringBoard deadlocks in a sync NSXPC call made on the connection's own queue (also on macOS)
# x86Code: iconservicesagent (icon images), chronod (widgets), sleepd, pkd, distnoted, tccd, fontservicesd run on a
# real iOS 26.5 simulator (launchctl list); without them SpringBoard/Spotlight loop on failed lookups
# x86Code: com.apple.assetsd serves com.apple.photos.service (the photo library); without it Photos aborts at launch
# (PXPhotosDataSource: "index 0 beyond bounds for empty array", the library failed to open)
# x86Code: com.apple.geod serves Maps' (and GeoServices clients') network (tiles, search; without it Maps says "No Internet
# Connection"); nsurlsessiond runs background URL sessions (both run on a Mac's simulator)
# x86Code: kbd serves com.apple.TextInput (without it every text field's keyboard client retried in a loop and
# typed characters were lost); navd (com.apple.navigationService: the Maps widget looked it up ~2000 times a
# minute), profiled (ManagedConfiguration migration: Spotlight polled for it) and assistantd run on a Mac's
# simulator; the Safari bookmark sync agent, webbookmarksd and Safari.History are Safari's (without the sync
# agent Safari reconnected in a loop and crashed in -[WBTabGroupSyncAgentProxy addSyncObserver:])
# x86Code: com.apple.FileCoordination (filecoordinationd): without it NSFileCoordinator fails and Health traps at launch
# (DiskHealthExperienceStore.swift:307 fatal error); it runs on a Mac's simulator. com.apple.FileProvider (fileproviderd):
# Preview's and Files' document browsers look it up (it crashed at start until SIMULATOR_MEMORY_WARNINGS was set, FINDINGS 3.11)
# x86Code: the account, CloudKit, push, identity, location and notification daemons, and newsd, run on a Mac's simulator.
# News waited for ever "determining user ID" without accountsd/akd/cloudd. amsaccountsd: see below (it trapped in CloudKit
# at boot, FINDINGS 3.11).
# x86Code: Spotlight: searchd (the CoreSpotlight index: Files/Preview Recents, home-screen search) and
# corespotlightservice; with them the daemons a Mac's simulator runs that Spotlight, Files and others look up: dasd
# (background tasks), contactsd, assistant_cdmd (query understanding), mobileassetd, biomed, parsecd, pasted (the
# pasteboard), bird (com.apple.bird.token), swcd, intelligenceplatformd, textunderstandingd (FINDINGS 3.12).
# x86Code: suggestd (left out since 3.12: SIGSEGV at boot) runs since the Darling fixes of 3.13-3.16; no crash in 5
# boots of 3.16 (FINDINGS 3.16).
# x86Code: syncdefaultsd (com.apple.syncdefaultsd, the iCloud key-value store) and amsaccountsd (AppleMediaServices'
# accounts) trapped in CloudKit until Darling's proc_pidinfo(PROC_PIDPATHINFO) returned 0 like XNU: cloudd took every
# client for dead and answered container requests with (nil, nil) (FINDINGS 3.13). Both run on a Mac's simulator.
# x86Code: nanoregistryd (the paired-watch registry; runs on a Mac's simulator): without it locationd's start hung in
# -[NRPairedDeviceRegistry initWithBoost:disconnected:] (a synchronous wait for the registry) on a queue its other
# services wait behind, and its com.apple.locationd.simulation listener never took a connection: simctl location
# requests were lost (FINDINGS 3.17)
# x86Code: com.x86code.openurl (host/openurl/openurl) serves cli/libexec/simtouch.py openurl <url>
# x86Code: com.x86code.fpsim2host (host/fpsim2host) stands in for the Mac host's FileProvider service
# (com.apple.FileProvider.sim2host: open by id, openat/mkdirat/... for File Provider extensions; FINDINGS 3.13)
# x86Code: revisiond (GenerationalStorage: document id -> file id; FileProvider resolves every "did=" item through it,
# so Preview could open nothing Files handed over: "can't resolve did=... [70: Stale NFS file handle]", FINDINGS 3.14)
# x86Code: com.x86code.fseventsd (host/fseventsd) stands in for the Mac host's fseventsd (com.apple.FSEvents: volume
# UUIDs, live file-system events from Linux inotify; the On My iPhone extension needs it for item lookups and live
# updates in Files; FINDINGS 3.14)
# x86Code: com.apple.PosterBoard is a LaunchAngel (draws the wallpaper; SpringBoard's PRSService looks up its endpoint)
# x86Code: Metal (host/metalhostd) is on by default for every process (CoreAnimation's render server, the default
# wallpaper, apps): X86CODE_METAL unset = all; X86CODE_METAL=0 = software rendering only; a process list
# (e.g. backboardd,PosterBoard,MercuryPosterExtension) as documented in docs/METAL.md. It needs mtlvk (Linux, Vulkan): start
# it first and wait for its socket; if it doesn't come up, boot without Metal rather than with a dead device.
export X86CODE_METAL=${X86CODE_METAL-all}
if [ -n "$X86CODE_METAL" ] && [ "$X86CODE_METAL" != 0 ]; then
	MTLVK_BIN=${MTLVK:-$X86SIM_MTLVK}
	pkill -x mtlvk 2>/dev/null; sleep 0.2
	export X86CODE_MTLVK_SOCKET=$ST/mtlvk.sock   # mksimdevice.py hands it to metalhostd
	rm -f $ST/mtlvk.sock
	if [ -x "$MTLVK_BIN" ]; then
		(setsid "$MTLVK_BIN" $ST/mtlvk.sock > $ST/mtlvk.log 2>&1 &)
		for i in $(seq 1 50); do [ -S $ST/mtlvk.sock ] && break; sleep 0.1; done
	fi
	if [ -S $ST/mtlvk.sock ] && pgrep -x mtlvk >/dev/null; then
		echo "rundev: mtlvk up, Metal for: $X86CODE_METAL" >&2
	else
		echo "rundev: mtlvk did not start ($MTLVK_BIN, see $ST/mtlvk.log): booting with software rendering" >&2
		export X86CODE_METAL=0
	fi
fi
# x86Code: the host's mDNSResponder: on a Mac the simulator's libsystem_dnssd talks to the host daemon at
# /var/run/mDNSResponder. linux/mdnsd/mdnsresponder.py (Linux, real DNS lookups) listens on var/run/mDNSResponder
# and cli/libexec/run-launchd-sim.sh links the container's /var/run/mDNSResponder to it (the prefix's var/run is an overlay
# that Darling's startup recreates: a socket made there from Linux is invisible or a stale handle inside).
# X86CODE_MDNS=0 leaves it out (every lookup then fails with -1003).
if [ "${X86CODE_MDNS:-1}" != 0 ]; then
	[ -f $ST/mdns.pid ] && kill $(cat $ST/mdns.pid) 2>/dev/null
	setsid python3 $X/linux/mdnsd/mdnsresponder.py $ST/mDNSResponder > $ST/mdnsresponder.log 2>&1 &
	echo $! > $ST/mdns.pid
fi
X86CODE_JOBLOG=1 python3 cli/libexec/mksimdevice.py $D com.apple.cfprefsd.xpc.daemon com.apple.cfprefsd.xpc.daemon.system com.apple.notifyd com.apple.configd_sim \
	com.apple.runningboardd com.apple.lsd com.apple.containermanagerd com.apple.containermanagerd.system \
	com.apple.mobilegestalt.xpc com.apple.UserEventAgent-System com.apple.securityd com.apple.trustd com.apple.syslogd com.apple.mobile.installd \
	com.apple.systemstatusd \
	com.apple.iconservices.iconservicesagent com.apple.chronod com.apple.sleepd com.apple.pluginkit.pkd \
	com.apple.distnoted.xpc.daemon com.apple.tccd com.apple.fontservicesd \
	com.apple.assetsd com.apple.geod com.apple.nsurlsessiond \
	com.apple.TextInput.kbd com.apple.navd com.apple.managedconfiguration.profiled com.apple.assistantd \
	com.apple.SafariBookmarksSyncAgent com.apple.WebBookmarks.webbookmarksd com.apple.Safari.History \
	com.apple.PosterBoard com.apple.FileCoordination com.apple.FileProvider \
	com.apple.accountsd com.apple.akd com.apple.cloudd com.apple.apsd com.apple.appleaccountd com.apple.usernotificationsd \
	com.apple.locationd com.apple.followupd com.apple.newsd com.apple.identityservicesd com.apple.ids_simd com.apple.cdpd \
	com.apple.searchd com.apple.corespotlightservice com.apple.dasd com.apple.contactsd com.apple.assistant_cdmd \
	com.apple.mobileassetd com.apple.biomed com.apple.parsecd com.apple.pasteboard.pasted com.apple.bird com.apple.swcd \
	com.apple.intelligenceplatformd com.apple.textunderstandingd com.apple.syncdefaultsd com.apple.amsaccountsd \
	com.apple.revisiond com.apple.installcoordinationd com.apple.suggestd com.apple.nanoregistryd \
	com.x86code.prefs=tests/probes/prefs com.x86code.openurl=host/openurl/openurl \
	"com.x86code.fpsim2host=host/fpsim2host/fpsim2host|mach:com.apple.FileProvider.sim2host" \
	"com.x86code.fseventsd=host/fseventsd/fseventsd|mach:com.apple.FSEvents" "$@" >/dev/null
# x86Code: pre-answer the permission prompts (TCC, location, notifications) for the apps we test, as a test device is
# set up: an unanswered prompt covers the app (cli/libexec/seedperms.py). X86CODE_SEEDPERMS=0 leaves the device as it is.
[ "${X86CODE_SEEDPERMS:-1}" != 0 ] && python3 cli/libexec/seedperms.py $D
UDID=$(sed -n 's/^SIMULATOR_UDID=//p' $D/sim.env)
L=$X86SIM_DPREFIX/private/var/tmp/com.apple.CoreSimulator.SimDevice.$UDID/launchd.log
rm -f $L $L.1 $L.2
(TIMEOUT=$((${SECS:-55}+5)) cli/libexec/run-launchd-sim.sh $D > $ST/lsim.out 2>&1 &)
# x86Code: launchd.log rotates (2 x 7 MB) when something floods it; keep spawn/exit lines aside
# (and everything but failed lookups in $ST/rundev-launchd.log)
sleep 3; (timeout $((${SECS:-55}+2)) tail -n +1 -F $L 2>/dev/null | tee >(grep --line-buffered -v -E "failed lookup|Last log repeated" > $ST/rundev-launchd.log) | grep --line-buffered -E "Successfully spawned|exited due" > $ST/rundev-spawns.log &)
sleep ${SECS:-55}
grep -h synchronize $D/data/var/log/com.x86code.prefs.log || echo "prefs: STUCK"
cat $ST/rundev-spawns.log | sed "s/^[0-9-]* [0-9:.]* //; s/\[[0-9]*\]//g; s/ because.*//; s/, ran for.*//; s/<Notice>: //" | sort | uniq -c | cut -c1-120
