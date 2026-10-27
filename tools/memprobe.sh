#!/bin/sh
# Memory of a running lyren: RSS breakdown, DRM-resident GPU memory and the largest mappings.
# usage: tools/memprobe.sh [pid] [mappings]
pid=${1:-$(pgrep -x lyren | head -n1)}
n=${2:-12}
[ -n "$pid" ] || { echo "lyren is not running" >&2; exit 1; }

awk '/^(VmRSS|RssAnon|RssFile|RssShmem):/ { printf "%-9s %7.1f MB\n", $1, $2 / 1024 }' /proc/$pid/status

# Several fds can share one DRM client; count each client once.
for fd in /proc/$pid/fd/*; do
    case $(readlink "$fd") in /dev/dri/*) cat /proc/$pid/fdinfo/${fd##*/} ;; esac
done | awk -f "$(dirname "$0")/drm-resident.awk"

echo "largest mappings:"
awk '/^[0-9a-f]+-[0-9a-f]+ / { name = $6 == "" ? "[anon]" : $6; sub(/.*\//, "", name) }
     /^Rss:/ { rss[name] += $2 }
     END { for (k in rss) printf "%7.2f MB  %s\n", rss[k] / 1024, k }' /proc/$pid/smaps | sort -rn | head -n "$n"
