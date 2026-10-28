#!/bin/sh
# Publish the progress page (cli/libexec/progress_site.py) and the live stream (simstream.py) through a cloudflared
# quick tunnel. Serves only $X86SIM_SITE on 127.0.0.1:$X86SIM_SITE_PORT. The public URL is printed and kept in
# $X86SIM_SCRATCH/tunnel.log ("trycloudflare.com").
#   progress_tunnel.sh [start]   start whatever of site loop / stream / http server / tunnel is not running
#                                (a running tunnel is kept, so the public URL stays the same)
#   progress_tunnel.sh stop [all] stop the site loop and the stream; with "all" also the http server and tunnel
#   progress_tunnel.sh status
X86SIM_ROOT=${X86SIM_ROOT:-$(cd "$(dirname "$0")/../.." && pwd)}; . "$X86SIM_ROOT/cli/lib/config.sh"
X=$X86SIM_ROOT
S=$X86SIM_SCRATCH
mkdir -p "$X86SIM_SITE"
alive() { [ -f "$S/$1.pid" ] && kill -0 "$(cat "$S/$1.pid")" 2>/dev/null; }
stop() { alive "$1" && kill "$(cat "$S/$1.pid")" 2>/dev/null; rm -f "$S/$1.pid"; }
url() { grep -o 'https://[a-z0-9-]*\.trycloudflare\.com' "$S/tunnel.log" 2>/dev/null | head -1; }
case "${1:-start}" in
stop)
	stop stream; stop site-loop
	[ "$2" = all ] && { stop site-http; stop tunnel; }
	exit 0 ;;
status)
	for n in site-loop stream site-http tunnel; do alive $n && echo "$n: running (pid $(cat "$S/$n.pid"))" || echo "$n: stopped"; done
	alive tunnel && echo "url: $(url)"
	exit 0 ;;
esac
python3 "$X/cli/libexec/progress_site.py"
alive site-loop || { setsid python3 "$X/cli/libexec/progress_site.py" --loop > "$S/site-loop.log" 2>&1 & echo $! > "$S/site-loop.pid"; }
alive stream || { setsid python3 "$X/cli/libexec/simstream.py" > "$S/stream.log" 2>&1 & echo $! > "$S/stream.pid"; }
alive site-http || { setsid python3 -m http.server "$X86SIM_SITE_PORT" --bind 127.0.0.1 --directory "$X86SIM_SITE" > "$S/site-http.log" 2>&1 & echo $! > "$S/site-http.pid"; }
if ! alive tunnel; then
	setsid cloudflared tunnel --no-autoupdate --url "http://127.0.0.1:$X86SIM_SITE_PORT" > "$S/tunnel.log" 2>&1 & echo $! > "$S/tunnel.pid"
fi
for i in $(seq 1 30); do
	u=$(url)
	[ -n "$u" ] && { echo "$u"; exit 0; }
	sleep 1
done
echo "no tunnel URL yet; see $S/tunnel.log" >&2
