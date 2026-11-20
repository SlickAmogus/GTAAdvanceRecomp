#!/bin/bash
# gta_install.sh - install or update the GTA Advance room server on this Linux
# machine: an sh_master (the Silent Hill Online server program) run as its own
# systemd service, gta-rooms, on its own ports and in its own folder, so its
# rooms never mix with a Silent Hill server on the same machine.
#
#   sudo bash gta_install.sh [--args "..."] [--yes] [--no-warn]
#
# Builds sh_master from the sources uploaded beside this script (gcc needed).
# --args is used when the service is first created; later the command line
# lives in ExecStart= of /etc/systemd/system/gta-rooms.service.
#
# With players online it lists them and asks:
#   c  continue: warn them, restart in 30 seconds
#   w  wait: check every 15 seconds and restart as soon as nobody is on
#   s  stop: change nothing
# From gta_redeploy.bat (no terminal) the question goes to the Windows side
# as a line "@@ASK <n>" and the answer comes back on standard input.
set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
UNIT=gta-rooms
UNIT_FILE=/etc/systemd/system/$UNIT.service
DIR=/root/gta-rooms
BIN=$DIR/sh_master
ARGS='-p 27889 --admin 27892 -n "GTA Advance rooms" --no-ghosts --no-memos --no-deaths --no-update-check'
HELPER=/usr/local/bin/gta-admin
WARN=1
ASK=1
while [ $# -gt 0 ]; do
    case "$1" in
        --args)    [ -n "$2" ] && ARGS="$2"; shift 2 ;;
        --no-warn) WARN=0; ASK=0; shift ;;
        --yes)     ASK=0; shift ;;
        *) echo "unknown option: $1"; exit 2 ;;
    esac
done

[ "$(id -u)" = 0 ] || { echo "run as root (or with sudo)"; exit 1; }
command -v systemctl >/dev/null 2>&1 || { echo "this needs systemd"; exit 1; }
command -v gcc >/dev/null 2>&1 || { echo "gcc is needed to build the server: apt install gcc"; exit 1; }
mkdir -p "$DIR"

echo ">> building the room server ..."
gcc -O2 -I"$HERE" -o "$BIN.new" "$HERE/sh_master.c"
"$BIN.new" --help >/dev/null

admin_port() {
    if [ -f "$UNIT_FILE" ]; then
        P="$(grep '^ExecStart=' "$UNIT_FILE" | sed -n 's/.* --admin \([0-9]*\).*/\1/p')"
        [ -n "$P" ] && { echo "$P"; return; }
    fi
    echo "$ARGS" | sed -n 's/.*--admin \([0-9]*\).*/\1/p'
}
ADMIN_PORT="$(admin_port)"
players_online() {
    set +e
    LIST="$("$BIN" --admin "$ADMIN_PORT" --cmd players 2>/dev/null)"
    N="$(printf '%s\n' "$LIST" | tail -1 | grep -o '^[0-9]*')"
    set -e
    [ -n "$N" ] || N=0
}

ANS=c
if [ "$ASK" = 1 ] && [ -x "$BIN" ] && systemctl is-active --quiet $UNIT; then
    players_online
    if [ "$N" -gt 0 ]; then
        printf '%s\n' "$LIST"
        if [ -t 0 ]; then
            printf '%s player(s) online. Continue (warn, restart in 30 s), wait until nobody is on, or stop? [c/w/s] ' "$N"
        else
            echo "@@ASK $N"
        fi
        read -r ANS || ANS=s
        case "$ANS" in
            c|C) ANS=c ;;
            w|W) ANS=w ;;
            *)
                rm -f "$BIN.new"
                echo ">> stopped: nothing was changed, the server keeps running the version it has"
                exit 3 ;;
        esac
    else
        [ -t 0 ] || echo "@@GO"
    fi
else
    [ -t 0 ] || echo "@@GO"
fi

if [ "$ANS" = w ]; then
    echo ">> waiting for the server to empty (checking every 15 seconds; close the window to give up) ..."
    while true; do
        players_online
        [ "$N" -gt 0 ] || break
        echo "   $(date +%H:%M:%S)  $N player(s) still on"
        sleep 15
    done
    echo ">> nobody on now"
    WARN=0
fi

mv -f "$BIN.new" "$BIN"

if [ ! -f "$UNIT_FILE" ]; then
    cat > "$UNIT_FILE" <<EOF
[Unit]
Description=GTA Advance room server (sh_master)
After=network-online.target
Wants=network-online.target
StartLimitIntervalSec=0

[Service]
WorkingDirectory=$DIR
ExecStart=$BIN $ARGS
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
EOF
    echo ">> created $UNIT_FILE"
    echo "   command line: $BIN $ARGS"
fi
# Updates come from gta_redeploy.bat. sh_master's own updater looks for new
# Silent Hill Online server releases, which would only confuse this instance.
if ! grep -q '^ExecStart=.*--no-update-check' "$UNIT_FILE"; then
    sed -i 's/^\(ExecStart=.*\)$/\1 --no-update-check/' "$UNIT_FILE"
    echo ">> added --no-update-check to $UNIT_FILE"
fi

# gta-admin: sh_master's admin client aimed at this instance's admin port
# (read from the unit each time, so a changed --admin is followed).
#   gta-admin --console      the admin console (players, kick, ban, say, watch...)
#   gta-admin --watch [N]    the log live
#   gta-admin --cmd "..."    one command
#   gta-admin log            print the whole log file
cat > "$HELPER" <<EOF
#!/bin/sh
P="\$(sed -n 's/^ExecStart=.* --admin \([0-9]*\).*/\1/p' $UNIT_FILE)"
if [ "\$1" = log ]; then
    L="\$(sed -n 's/^ExecStart=.* --log \([^ ]*\).*/\1/p' $UNIT_FILE)"
    exec cat "\${L:-$DIR/sh_master.log}"
fi
exec $BIN --admin "\${P:-27892}" "\$@"
EOF
chmod 755 "$HELPER"
UDP_PORT="$(grep '^ExecStart=' "$UNIT_FILE" | sed -n 's/.* -p \([0-9]*\).*/\1/p')"
[ -n "$UDP_PORT" ] || UDP_PORT=27889
systemctl daemon-reload
systemctl enable $UNIT >/dev/null 2>&1
if command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -q 'Status: active'; then
    ufw allow "$UDP_PORT/udp" >/dev/null 2>&1 || true
fi

if [ "$WARN" = 1 ] && systemctl is-active --quiet $UNIT; then
    players_online
    if [ "$N" -gt 0 ] && "$BIN" --admin "$ADMIN_PORT" --cmd "say The room server restarts in 30 seconds." >/dev/null 2>&1; then
        echo ">> $N player(s) warned: restarting in 30 seconds"
        for s in 30 20 10; do echo "   $s ..."; sleep 10; done
    fi
fi

echo ">> starting ..."
systemctl restart $UNIT
sleep 1
printf '>> service %s is now: ' "$UNIT"; systemctl is-active $UNIT || true
printf '>> admin console answers: '
if "$BIN" --admin "$ADMIN_PORT" --cmd players >/dev/null 2>&1; then echo yes; else echo "no (journalctl -u $UNIT -n 50)"; fi
echo ">> done. The game connects to UDP $UDP_PORT on this machine."
echo "   admin console: gta-admin --console (gta_console.bat on Windows), live log: gta-admin --watch"
