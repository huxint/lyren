# DRM fdinfo sizes are bytes unless they carry KiB or MiB.
/^drm-client-id:/ { count = !seen[$2]++ }
/^drm-resident-/ && count {
    v = $2
    if ($3 == "MiB") v *= 1024
    else if ($3 != "KiB") v /= 1024
    total += v
}
END {
    printf "%-9s %7.1f MB\n", "GPU", total / 1024
}
