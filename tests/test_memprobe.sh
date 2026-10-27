#!/bin/sh
set -eu

# Client 12 has 1 MiB in system memory and 0.5 MiB in local memory;
# a second fd refers to that same client. Client 13 adds another 2 MiB.
actual=$(awk -f "$1/tools/drm-resident.awk" <<'EOF'
drm-client-id: 12
drm-resident-system0: 1048576
drm-resident-local0: 512 KiB
drm-client-id: 12
drm-resident-system0: 1 MiB
drm-resident-local0: 524288
drm-client-id: 13
drm-resident-system0: 2 MiB
EOF
)
set -- $actual
if [ "$1" != GPU ] || [ "$2" != 3.5 ] || [ "$3" != MB ]; then
    printf 'Expected GPU 3.5 MB, got %s\n' "$actual" >&2
    exit 1
fi
