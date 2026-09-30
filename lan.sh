#!/bin/bash
# Laplace on the local network: the server reachable from 192.168.1.0/24 as role laplace, password (scram) over TLS.
# Needs root, because pg_hba.conf and the certificate live in postgres's data directory (Operations: Setup).
#
#   sudo ./lan.sh          logs to $LAPLACE_WORK/logs/root/lan-<time>.log
#
# What it does, once each:
#   1. host all laplace 192.168.1.0/24 scram-sha-256   appended to pg_hba.conf (with hostssl preferred first)
#   2. a self-signed server certificate (10 years) as postgres, ssl = on
#   3. restarts the server, so listen_addresses = '*' (already set with ALTER SYSTEM) takes effect
#   4. shows what listens and the rules in force
# The role's password is in the operator's ~/.pgpass (mode 600); it is never printed.
set -e
here=$(cd "$(dirname "$0")" && pwd); . "$here/laplace.env"
PGDATA=${PGDATA:-/data/pgdata}; BIN=${LAPLACE_PGBIN:-/usr/local/pgsql/bin}; LAN=${LAPLACE_LAN:-192.168.1.0/24}; owner=${SUDO_USER:-$(id -un)}
log=$LAPLACE_WORK/logs/root/lan-$(date -u +%Y%m%dT%H%M%SZ).log; mkdir -p "$LAPLACE_WORK/logs/root"; chown "$owner" "$LAPLACE_WORK/logs/root"
exec 3>exec > >(tee "$log") 2>&11; exec > "$log" 2>&1; tail -f "$log" >&3 & tailpid=$!; trap 'sleep 0.2; kill $tailpid 2>/dev/null' EXIT
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
echo "=== pg_hba.conf"
if grep -q "^hostssl all laplace $LAN" "$PGDATA/pg_hba.conf"; then echo "rule present"; else
  cp -p "$PGDATA/pg_hba.conf" "$PGDATA/pg_hba.conf.before-lan"
  printf '\n# Laplace on the local network (lan.sh)\nhostssl all laplace %s scram-sha-256\nhostnossl all laplace %s reject\n' "$LAN" "$LAN" >> "$PGDATA/pg_hba.conf"
  echo "rule added"; fi
echo "=== certificate"
if [ -s "$PGDATA/server.crt" ]; then echo "present: $(openssl x509 -in "$PGDATA/server.crt" -noout -subject -enddate | tr '\n' ' ')"; else
  su postgres -c "openssl req -new -x509 -days 3650 -nodes -subj '/CN=$(hostname)' -keyout '$PGDATA/server.key' -out '$PGDATA/server.crt' 2>/dev/null && chmod 600 '$PGDATA/server.key'"
  echo "made"; fi
su postgres -c "$BIN/psql -X -h /tmp -d postgres -c \"ALTER SYSTEM SET ssl = on\""
echo "=== restart"
systemctl restart pgdata.service
for i in 1 2 3 4 5 6 7 8 9 10; do su postgres -c "$BIN/pg_isready -h /tmp -q" && break; sleep 1; done
echo "=== in force"
ss -ltn | grep 5432
su postgres -c "$BIN/psql -X -h /tmp -d postgres -Atc \"SELECT name||' = '||setting FROM pg_settings WHERE name IN ('listen_addresses','ssl')\""
su postgres -c "$BIN/psql -X -h /tmp -d postgres -c \"SELECT rule_number, type, database, user_name, address, netmask, auth_method, error FROM pg_hba_file_rules ORDER BY rule_number\""
echo "=== from this host, over TCP with the password, as a client on the network would"
addr=$(ip -4 -o addr | awk -v lan="$LAN" '$4 ~ "^"substr(lan,1,index(lan,".0/")) {print $4; exit}' | cut -d/ -f1)
su "$owner" -c "$BIN/psql -X 'host=$addr port=5432 user=laplace dbname=laplace_engine sslmode=require' -Atc \"SELECT 'connected over '||coalesce((SELECT version FROM pg_stat_ssl WHERE pid = pg_backend_pid()), 'no tls')\""
echo "=== done; a firewall, if one is on, must pass 5432/tcp from $LAN"
