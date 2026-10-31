#!/bin/sh
# Publish the progress page (cli/libexec/progress_site.py) and the live stream (simstream.py) through a cloudflared
# quick tunnel. Serves only $X86SIM_SITE on 127.0.0.1:$X86SIM_SITE_PORT. The public URL is printed and kept in
# $X86SIM_SCRATCH/tunnel.log ("trycloudflare.com").
#   progress_tunnel.sh [start]   start whatever of site loop / stream / http server / tunnel is not running
#                                (a running tunnel is kept, so the public URL stays the same)
#   progress_tunnel.sh stop [all] stop the site loop and the stream; with "all" also the http server and tunnel
#   progress_tunnel.sh status
# Speed 13: each part runs as a transient systemd user service (x86sim-site-<name>), owned by the user manager. Started
# with setsid from a shell they stayed in that shell's cgroup (an agent's or a terminal's scope) and got SIGTERM with it
# when that scope was stopped: all four died together and the tunnel URL changed at every restart.
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
S=$X86SIM_SCRATCH
mkdir -p "$X86SIM_SITE"
alive() { systemctl --user is-active --quiet "x86sim-site-$1"; }
stop() { systemctl --user stop "x86sim-site-$1" 2>/dev/null; rm -f "$S/$1.pid"; }
# run NAME LOG CMD...: start CMD as the user service x86sim-site-NAME (restarted if it fails), output to LOG
run() {
	n=$1 log=$2; shift 2
	while IFS= read -r kv; do [ -n "$kv" ] && set -- -E "$kv" "$@"; done <<-EOF
	$(env | grep -E '^(X86SIM_|SIMFB_|X86CODE_)')
	EOF
	set -- -E PATH="$PATH" -E HOME="$HOME" -E X86SIM_ROOT="$X" "$@"
	systemctl --user reset-failed "x86sim-site-$n" 2>/dev/null
	systemd-run --user --quiet --collect --unit="x86sim-site-$n" -p Restart=on-failure -p RestartSec=5 \
		-p "StandardOutput=truncate:$log" -p "StandardError=inherit" --working-directory="$X" "$@"
	systemctl --user show -p MainPID --value "x86sim-site-$n" > "$S/$n.pid"
}
url() { grep -o 'https://[a-z0-9-]*\.trycloudflare\.com' "$S/tunnel.log" 2>/dev/null | head -1; }
case "${1:-start}" in
stop)
	stop stream; stop site-loop
	[ "$2" = all ] && { stop site-http; stop tunnel; }
	exit 0 ;;
status)
	for n in site-loop stream site-http tunnel; do alive $n && echo "$n: running (pid $(systemctl --user show -p MainPID --value x86sim-site-$n))" || echo "$n: stopped"; done
	alive tunnel && echo "url: $(url)"
	exit 0 ;;
esac
python3 "$X/cli/libexec/progress_site.py"
alive site-loop || run site-loop "$S/site-loop.log" python3 "$X/cli/libexec/progress_site.py" --loop
alive stream || run stream "$S/stream.log" python3 "$X/cli/libexec/simstream.py"
alive site-http || run site-http "$S/site-http.log" python3 -m http.server "$X86SIM_SITE_PORT" --bind 127.0.0.1 --directory "$X86SIM_SITE"
# the tunnel's log keeps the URL: a restarted tunnel (new URL) overwrites it
alive tunnel || run tunnel "$S/tunnel.log" cloudflared tunnel --no-autoupdate --url "http://127.0.0.1:$X86SIM_SITE_PORT"
for i in $(seq 1 30); do
	u=$(url)
	[ -n "$u" ] && { echo "$u"; exit 0; }
	sleep 1
done
echo "no tunnel URL yet; see $S/tunnel.log" >&2
