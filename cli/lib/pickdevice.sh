# x86Sim: pick_device ARG — simctl-style <device> argument: a device dir, var/<folder>, an arm64 phone's
# suffix (var/device-a64-<ARG>) or its name; "booted" = the configured one. Sets X86SIM_DEVICE; returns 1 otherwise.
pick_device() {  # pick_device ARG: sets X86SIM_DEVICE when ARG names a device
	local a=$1 c
	[ -n "$a" ] || return 1
	[ "$a" = booted ] && return 0
	for c in "$a" "$X86SIM_ROOT/var/$a" "$X86SIM_ROOT/var/device-a64-$a"; do
		[ -f "$c/sim.env" ] && [ -d "$c/data" ] && { X86SIM_DEVICE=$(cd "$c" && pwd); return 0; }
	done
	for c in "$X86SIM_ROOT"/var/device-a64*; do   # the name given at creation
		[ "$(cat "$c/name" 2>/dev/null)" = "$a" ] && { X86SIM_DEVICE=$c; return 0; }
	done
	return 1
}
