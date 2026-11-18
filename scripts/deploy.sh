#!/bin/sh
#
# deploy.sh — встановити security_daemon на плату як сервіс (BusyBox init).
#
#   sh scripts/deploy.sh [--run] root@<ip> <security_daemon> [libgpiod.so.X.Y.Z]
#
# Що робить:
#   1. зупиняє сервіс / старий процес;
#   2. кладе бінарник у /usr/bin, init-скрипт у /etc/init.d (автозапуск);
#   3. (опційно) кладе libgpiod з sysroot Buildroot у /usr/lib — без LD_LIBRARY_PATH;
#   4. виставляє на платі час із хоста (на Pi без мережі/батарейки RTC інакше 1970-01-01)
#      і пише його в RTC (hwclock), щоб пережив перезавантаження;
#   5. готує /etc/security_daemon/cards.db (0600) і /var/lib/security_daemon;
#   6. стартує сервіс  (або з --run: запускає демон у цьому терміналі).
#
# Викликається CMake-таргетами deploy / run, але працює й вручну.
#
set -eu

RUN=0
if [ "${1:-}" = "--run" ]; then RUN=1; shift; fi
if [ $# -lt 2 ]; then
	echo "usage: $0 [--run] user@host <binary> [libgpiod.so.X.Y.Z]" >&2
	exit 2
fi

TARGET=$1
BIN=$2
LIB=${3:-}
HERE=$(cd "$(dirname "$0")/.." && pwd)
INIT="$HERE/init/S99security-daemon"
SSH_OPTS="-o BatchMode=yes -o ConnectTimeout=20"

say() { printf '==> %s\n' "$*"; }
rsh() { ssh $SSH_OPTS "$TARGET" "$@"; }
put() { scp -O -q $SSH_OPTS "$1" "$TARGET:$2"; }

say "stop service on $TARGET"
rsh '/etc/init.d/S99security-daemon stop >/dev/null 2>&1; killall security_daemon >/dev/null 2>&1; mkdir -p /etc/security_daemon /var/lib/security_daemon; true'

say "install /usr/bin/security_daemon, /etc/init.d/S99security-daemon"
put "$BIN"  /usr/bin/security_daemon
put "$INIT" /etc/init.d/S99security-daemon

if [ -n "$LIB" ] && [ -f "$LIB" ]; then
	name=$(basename "$LIB")
	soname=$(printf '%s' "$name" | sed -E 's/^(lib[^.]+\.so\.[0-9]+).*/\1/')
	say "install /usr/lib/$name (+ $soname)"
	put "$LIB" "/usr/lib/$name"
	rsh "ln -sf $name /usr/lib/$soname"
fi

say "sync clock from host ($(date -u '+%Y-%m-%d %H:%M:%S') UTC)"
rsh "{ date -u -s @$(date +%s) || date -u -s '$(date -u '+%Y-%m-%d %H:%M:%S')'; } >/dev/null 2>&1 \
	&& { hwclock -w -u >/dev/null 2>&1 || true; } || echo '   WARNING: clock not set'"

say "prepare cards.db / log dir"
rsh 'chmod 0755 /usr/bin/security_daemon /etc/init.d/S99security-daemon
db=/etc/security_daemon/cards.db
if [ ! -e "$db" ]; then
	if [ -s /root/cards.db ]; then cp /root/cards.db "$db"; echo "   migrated /root/cards.db"; else : > "$db"; fi
fi
chown root:root "$db"; chmod 0600 "$db"; chmod 0700 /etc/security_daemon
ldd /usr/bin/security_daemon 2>/dev/null | grep -q "not found" && echo "   WARNING: missing shared libs:" && ldd /usr/bin/security_daemon | grep "not found"
true'

if [ "$RUN" = 1 ]; then
	say "run in foreground (Ctrl+C to stop)"
	exec ssh -tt $SSH_OPTS "$TARGET" 'SECURITY_CARDS_DB=/etc/security_daemon/cards.db SECURITY_LOG=/var/lib/security_daemon/security.log /usr/bin/security_daemon'
fi

say "start service"
rsh '/etc/init.d/S99security-daemon start && sleep 1 && /etc/init.d/S99security-daemon status'
