#!/bin/sh
set -eu

LYREN_GFX_TEST_DIR=$(mktemp -d)
LYREN_GFX_TEST_BIN=$(realpath "$1")
export LYREN_GFX_TEST_DIR LYREN_GFX_TEST_BIN
trap 'rm -rf "$LYREN_GFX_TEST_DIR"' EXIT
mkdir -p "$LYREN_GFX_TEST_DIR/config" "$LYREN_GFX_TEST_DIR/cache" "$LYREN_GFX_TEST_DIR/state"

cat > "$LYREN_GFX_TEST_DIR/session.sh" <<'SESSION'
#!/bin/sh
env LYREN_VALIDATION=1 VK_LAYER_VALIDATE_SYNC=1 NO_PROXY=127.0.0.1 no_proxy=127.0.0.1 "$LYREN_GFX_TEST_BIN" > "$LYREN_GFX_TEST_DIR/tests.log" 2>&1
printf '%s\n' "$?" > "$LYREN_GFX_TEST_DIR/status"
SESSION
chmod +x "$LYREN_GFX_TEST_DIR/session.sh"

if ! dbus-run-session -- env XDG_CONFIG_HOME="$LYREN_GFX_TEST_DIR/config" XDG_CACHE_HOME="$LYREN_GFX_TEST_DIR/cache" \
    XDG_STATE_HOME="$LYREN_GFX_TEST_DIR/state" "$2" --virtual --socket "lyren-gfx-$$" --width 1600 --height 1000 \
    --no-lockscreen --no-global-shortcuts --no-kactivities --exit-with-session "$LYREN_GFX_TEST_DIR/session.sh" \
    > "$LYREN_GFX_TEST_DIR/kwin.log" 2>&1; then
    cat "$LYREN_GFX_TEST_DIR/kwin.log"
    exit 1
fi
cat "$LYREN_GFX_TEST_DIR/tests.log"
exit "$(cat "$LYREN_GFX_TEST_DIR/status")"
