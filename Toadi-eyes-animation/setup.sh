#!/bin/sh
# Deploy the Toadi eyes animation plug-in to a robot (persistent, crash-loop-safe).
#
# See SETUP.md and docs/101-persistence-and-ota.md §6.4.1 for what the
# installed wrapper does and why each line is there.
set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PLUGIN="$SCRIPT_DIR/plugins/libEyesAnimationPlugin.so"
KEY=""
SSHOPT=""
PORT=""
PRIORITY=""
INERT=0
FORCE=0
MODE=deploy
ROBOT=""

usage() {
    cat >&2 <<'EOF'
Usage: ./setup.sh [options] root@<robot>

Modes:
  (default)     trial deploy: copy the .so, install the crash-loop-safe
                wrapper, arm a one-shot trial, restart the app
  --approve     same, but approve immediately (plugin-ok instead of trial)
  --uninstall   remove wrapper, flags and .so; restart the stock app

Options:
  --inert            deploy without TOADI_EXAMPLE_HEARTS=1 (inert example)
  --port N           set TOADI_EXAMPLE_HEARTS_PORT (default: 8081)
  --priority N       set TOADI_EXAMPLE_HEARTS_PRIORITY (default: 2000)
  --plugin PATH      .so to deploy (default: plugins/libEyesAnimationPlugin.so
                     next to this script)
  --key PATH         SSH private key (-i)
  --force            skip the SHA256SUMS check
  -h, --help         this text
EOF
    exit 1
}

while [ $# -gt 0 ]; do
    case "$1" in
        --approve)   MODE=approve ;;
        --uninstall) MODE=uninstall ;;
        --inert)     INERT=1 ;;
        --port)      PORT="${2:?--port needs a value}"; shift ;;
        --priority)  PRIORITY="${2:?--priority needs a value}"; shift ;;
        --plugin)    PLUGIN="$2"; shift ;;
        --key)       KEY="$2"; shift ;;
        --force)     FORCE=1 ;;
        -h|--help)   usage ;;
        --*)         echo "unknown option: $1" >&2; usage ;;
        *)           ROBOT="$1" ;;
    esac
    shift
done

if [ -z "$ROBOT" ]; then
    echo "error: no robot given" >&2
    usage
fi
case "$ROBOT" in
    *@*) ;;
    *)   ROBOT="root@$ROBOT" ;;
esac
HOST="${ROBOT#*@}"

if [ -n "$KEY" ]; then
    SSHOPT="-i $KEY"
fi

case "$PORT" in
    '') ;;
    *[!0-9]*|0*|*[!0-9]) echo "error: --port must be a number" >&2; exit 1 ;;
esac
case "$PRIORITY" in
    '') ;;
    *[!0-9]*) echo "error: --priority must be a non-negative integer" >&2; exit 1 ;;
esac

check_plugin() {
    [ -f "$PLUGIN" ] || {
        echo "error: $PLUGIN not found (build it or pass --plugin)" >&2
        exit 1
    }
    if [ "$FORCE" = 1 ]; then
        return
    fi
    if ! command -v sha256sum >/dev/null 2>&1; then
        echo "warning: sha256sum not available; skipping hash check" >&2
        return
    fi
    EXPECTED=$(awk '$2 == "libEyesAnimationPlugin.so" { print $1 }' \
        "$SCRIPT_DIR/SHA256SUMS")
    if [ -z "$EXPECTED" ]; then
        echo "warning: libEyesAnimationPlugin.so not listed in SHA256SUMS; skipping hash check" >&2
        return
    fi
    ACTUAL=$(sha256sum "$PLUGIN" | awk '{ print $1 }')
    if [ "$ACTUAL" != "$EXPECTED" ]; then
        echo "error: $PLUGIN" >&2
        echo "       sha256 $ACTUAL" >&2
        echo "       does not match SHA256SUMS ($EXPECTED)" >&2
        echo "       rebuild, update SHA256SUMS, or pass --force" >&2
        exit 1
    fi
    echo "ok: $PLUGIN sha256 matches SHA256SUMS"
}

# Generate the dev-workdir wrapper (spec 101 §6.4.1) with the eyes gate
# (spec 102 §4). Output goes to $1.
build_wrapper() {
    OUT="$1"
    {
        cat <<'EOF'
#!/bin/sh
# Installed by the Toadi-eyes-animation setup.sh.
# Loads the persistent eyes plug-in only after trial/approval gating, and
# disarms itself before starting so a bad plug-in cannot cause a boot loop.
cd /home/toadi || exit 1
export LD_LIBRARY_PATH=/usr/local/cuda-10.2/lib:/home/toadi/lib
# Mandatory: the dev workdir exists, so the default would be
# /home/toadi/data/toadi/plugins and no stock plug-ins would load.
export PLUGIN_PATH=/home/toadi/plugins
# The dev workdir also moves the stock web document root; keep it usable.
WEBROOT=/home/toadi/data/toadi/web
[ -e "$WEBROOT" ] || ln -sfn /home/toadi/web "$WEBROOT"
EOF
        if [ "$INERT" = 0 ]; then
            echo "# Eyes-animation controller gate. Without it the plug-in"
            echo "# remains the inert VisionAlgo example (spec 102 section 4)."
            echo "export TOADI_EXAMPLE_HEARTS=1"
        else
            echo "# TOADI_EXAMPLE_HEARTS deliberately unset: inert VisionAlgo example."
        fi
        [ -n "$PORT" ] && echo "export TOADI_EXAMPLE_HEARTS_PORT=$PORT"
        [ -n "$PRIORITY" ] && echo "export TOADI_EXAMPLE_HEARTS_PRIORITY=$PRIORITY"
        cat <<'EOF'
ENABLE=/home/toadi/data/plugin-enable   # operator intent
OK=/home/toadi/data/plugin-ok           # proven stable
TRIAL=/home/toadi/data/plugin-trial     # one-shot request
REAL=/home/toadi/data/plugins/libEyesAnimationPlugin.so
LINK=/home/toadi/plugins/libEyesAnimationPlugin.so

if [ -f "$ENABLE" ] && { [ -f "$OK" ] || [ -f "$TRIAL" ]; } && [ -f "$REAL" ]; then
    # Consume the trial BEFORE the plug-in is loaded: if the app dies, the
    # next boot has ENABLE but neither OK nor TRIAL and starts the stock app.
    [ -f "$TRIAL" ] && rm -f "$TRIAL"
    ln -sfn "$REAL" "$LINK"
    [ -f "$OK" ] || ( sleep 600; pidof ToadiApp >/dev/null 2>&1 && touch "$OK" ) &
else
    rm -f "$LINK"                        # no stale link on app restart
fi
exec /home/toadi/ToadiApp "$@"
EOF
    } > "$OUT"
}

deploy() {
    check_plugin
    WRAP=$(mktemp)
    trap 'rm -f "$WRAP"' EXIT
    build_wrapper "$WRAP"

    echo "==> $HOST: preparing /home/toadi/data"
    ssh $SSHOPT "$ROBOT" 'mkdir -p /home/toadi/data/plugins /home/toadi/data/toadi'

    echo "==> $HOST: copying $PLUGIN -> /home/toadi/data/plugins/libEyesAnimationPlugin.so"
    scp $SSHOPT "$PLUGIN" "$ROBOT:/home/toadi/data/plugins/libEyesAnimationPlugin.so"

    echo "==> $HOST: removing any pre-split plug-in (libExampleVisionPlugin.so)"
    ssh $SSHOPT "$ROBOT" \
        'rm -f /home/toadi/data/plugins/libExampleVisionPlugin.so \
               /home/toadi/plugins/libExampleVisionPlugin.so'

    echo "==> $HOST: installing dev-workdir wrapper /home/toadi/data/toadi/ToadiApp"
    cat "$WRAP" | ssh $SSHOPT "$ROBOT" \
        'cat > /home/toadi/data/toadi/ToadiApp && chmod 0755 /home/toadi/data/toadi/ToadiApp'

    echo "==> $HOST: keeping the stock web UI reachable (web symlink)"
    ssh $SSHOPT "$ROBOT" \
        '[ -e /home/toadi/data/toadi/web ] || ln -sfn /home/toadi/web /home/toadi/data/toadi/web'

    if [ "$MODE" = approve ]; then
        echo "==> $HOST: arming approval (plugin-enable + plugin-ok)"
        ssh $SSHOPT "$ROBOT" \
            'touch /home/toadi/data/plugin-enable /home/toadi/data/plugin-ok && rm -f /home/toadi/data/plugin-trial'
    else
        echo "==> $HOST: arming trial (plugin-enable + plugin-trial)"
        ssh $SSHOPT "$ROBOT" \
            'touch /home/toadi/data/plugin-enable /home/toadi/data/plugin-trial && rm -f /home/toadi/data/plugin-ok'
    fi

    echo "==> $HOST: restarting the stock app"
    ssh $SSHOPT "$ROBOT" '/etc/init.d/toadi restart'

    echo "==> waiting for ToadiApp (up to 90 s)"
    i=0
    while [ "$i" -lt 90 ]; do
        if ssh $SSHOPT "$ROBOT" 'pidof ToadiApp >/dev/null 2>&1'; then
            echo "==> ToadiApp is up after ~${i} s"
            break
        fi
        sleep 2
        i=$((i + 2))
    done
    if [ "$i" -ge 90 ]; then
        echo "warning: ToadiApp not up after 90 s; inspect the robot" >&2
    fi

    cat <<EOF

Done (${MODE}). Verify:
  curl -s http://$HOST:8081/status
  curl -s http://$HOST:8081/animations
  curl -s -X POST "http://$HOST:8081/hearts?loops=3"
  ssh $ROBOT 'grep libEyesAnimationPlugin /proc/\$(pidof ToadiApp)/maps | head'
EOF
    if [ "$MODE" != approve ]; then
        cat <<EOF

Trial mode: if ToadiApp survives 10 minutes the wrapper creates plugin-ok.
Approve now with:  ./setup.sh $ROBOT --approve
EOF
    fi
}

uninstall() {
    echo "==> $HOST: removing wrapper (and dev-workdir dir), gating flags and plug-in"
    # The dev-workdir DIRECTORY must go, not just the wrapper: getPluginPath()
    # selects /home/toadi/data/toadi/plugins while it exists (spec 101 §6.6).
    ssh $SSHOPT "$ROBOT" \
        'rm -rf /home/toadi/data/toadi \
              /home/toadi/data/plugin-enable /home/toadi/data/plugin-ok /home/toadi/data/plugin-trial \
              /home/toadi/data/plugins/libEyesAnimationPlugin.so \
              /home/toadi/plugins/libEyesAnimationPlugin.so \
              /home/toadi/data/plugins/libExampleVisionPlugin.so \
              /home/toadi/plugins/libExampleVisionPlugin.so'
    echo "==> $HOST: restarting the stock app"
    ssh $SSHOPT "$ROBOT" '/etc/init.d/toadi restart'
    cat <<EOF

Uninstalled; the stock app is back. /home/toadi/data/plugin_backups and
anything outside /home/toadi/data/toadi are untouched. A reboot additionally
clears any leftover overlay entries.
EOF
}

case "$MODE" in
    deploy|approve) deploy ;;
    uninstall)      uninstall ;;
    *)              echo "unknown mode: $MODE" >&2; exit 1 ;;
esac
