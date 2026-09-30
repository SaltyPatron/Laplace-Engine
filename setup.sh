#!/bin/bash
# The machine Laplace runs on, declared once and converged: the one step that needs root (Operations: Setup, Database).
#
#   sudo ./setup.sh            everything below, in order; each part looks at the state and makes only what is missing
#   sudo ./setup.sh access     one part, or several: volumes | packages | kernel | deps | cluster | settings | access | agent | report
#
# Nothing here is a sequence of patches. Each part is a set: the volumes there are to be, the packages, the settings,
# the access rules; the part compares the set with the machine and closes the difference. Running it again on a
# finished machine changes nothing and reports. What it did is kept under $LAPLACE_WORK/logs/root. Then, as the
# operator: ./deploy.sh. Every name of this machine's (paths, users, volumes, the service, the network, the agent)
# is a variable of laplace.env, so another machine declares its own.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); . "$here/laplace.env"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
op=${SUDO_USER:-$(id -un)}; ophome=$(getent passwd "$op" | cut -d: -f6)
PGDATA=$LAPLACE_PGDATA; PGWAL=$LAPLACE_PGWAL; PGTEMP=$LAPLACE_PGTEMP; PGUSER=$LAPLACE_PGUSER; PGBIN=$LAPLACE_PG_DIR/bin; PREFIX=$LAPLACE_PREFIX; SERVICE=$LAPLACE_PGSERVICE
as_pg(){ su "$PGUSER" -c "$*" </dev/null; }                                     # as the server's user; never reads our stdin
q(){ as_pg "$PGBIN/psql -X -h /tmp -d postgres -Atc \"$1\""; }                  # one statement, its value
run(){ as_pg "$PGBIN/psql -X -h /tmp -d postgres -qc \"$1\"" >/dev/null; }        # one statement, done
mkdir -p "$LAPLACE_WORK/logs/root"; chown "$op" "$LAPLACE_WORK/logs/root"
log=$LAPLACE_WORK/logs/root/setup-$(date -u +%Y%m%dT%H%M%SZ).log; exec > >(tee "$log") 2>&1
say(){ printf '  %-46s %s\n' "$1" "$2"; }
part(){ echo; echo "=== $1   $(date -u +%H:%M:%S)"; }

# ------------------------------------------------------------------------------------------------------- the sets
# Volumes: $LAPLACE_VOLUMES (laplace.env): vg lv size fs mount. An existing volume is never resized or reformatted; a
# missing one is made. Anything not Laplace's is not there.
VOLUMES=$LAPLACE_VOLUMES
# Packages from the distribution: the build tools and the libraries PostgreSQL, PostGIS, PROJ and GDAL are built on.
PACKAGES="build-essential cmake ninja-build meson pkg-config flex bison liburing-dev libssl-dev liblz4-dev libzstd-dev
libreadline-dev zlib1g-dev libxml2-dev libsqlite3-dev libtiff-dev libcurl4-openssl-dev libprotobuf-c-dev
protobuf-c-compiler libjson-c-dev libeigen3-dev xfsprogs lvm2 openssl"
# Dependencies built from source under $LAPLACE_SRC, in this order; each is known by the file it installs, and is
# cloned when its source is not there. name  proof  how  origin  branch
DEPS="
geos        $PREFIX/bin/geos-config             cmake      https://github.com/libgeos/geos.git              main
proj        $PREFIX/bin/projinfo                cmake      https://github.com/OSGeo/PROJ.git                master
gdal        $PREFIX/bin/gdal-config             cmake      https://github.com/OSGeo/gdal.git                master
spectra     $PREFIX/include/Spectra/SymEigsSolver.h   headers   https://github.com/yixuan/spectra.git      master
postgresql  $PGBIN/postgres                     configure  https://git.postgresql.org/git/postgresql.git    REL_18_STABLE
postgis     $LAPLACE_PG_DIR/share/extension/postgis.control   postgis   https://git.osgeo.org/gitea/postgis/postgis.git   master
"
# The server's settings (Database.md), as ALTER SYSTEM makes them. Memory follows the machine.
ram_kb=$(awk '/MemTotal/{print $2}' /proc/meminfo); cores=$(nproc)
SETTINGS="
shared_buffers                   $((ram_kb / 4 / 1024 / 1024))GB
effective_cache_size             $((ram_kb * 7 / 10 / 1024 / 1024))GB
work_mem                         256MB
maintenance_work_mem             8GB
huge_pages                       try
io_method                        worker
io_workers                       $((cores * 2 / 3))
effective_io_concurrency         256
maintenance_io_concurrency       256
random_page_cost                 1.1
default_toast_compression        lz4
max_wal_size                     32GB
min_wal_size                     4GB
wal_buffers                      64MB
wal_compression                  zstd
checkpoint_timeout               30min
checkpoint_completion_target     0.9
max_worker_processes             $((cores + 4))
max_parallel_workers             $cores
max_parallel_workers_per_gather  $((cores / 2))
max_parallel_maintenance_workers $((cores / 2))
jit                              off
shared_preload_libraries         pg_stat_statements,auto_explain
pg_stat_statements.track         all
auto_explain.log_min_duration    5s
track_io_timing                  on
track_wal_io_timing              on
temp_tablespaces                 pgtemp
listen_addresses                 *
ssl                              on
port                             5432
max_connections                  100
"
# Who may connect, and how: the whole file, not a line added to whatever is there.
HBA="# Laplace (setup.sh). Local: the server's own user and the operator, over the socket. The network: laplace, scram over TLS.
local     all   $PGUSER                           peer
local     all   $LAPLACE_ROLE                     peer map=laplace
host      all   $LAPLACE_ROLE  127.0.0.1/32       scram-sha-256
host      all   $LAPLACE_ROLE  ::1/128            scram-sha-256
hostssl   all   $LAPLACE_ROLE  $LAPLACE_LAN       scram-sha-256
hostnossl all   all            0.0.0.0/0          reject
"
IDENT="# who connects over the socket as $LAPLACE_ROLE: the operator, the server's user, the agent
laplace  $op                  $LAPLACE_ROLE
laplace  $PGUSER              $LAPLACE_ROLE
laplace  $LAPLACE_AGENT_USER  $LAPLACE_ROLE
"

# ------------------------------------------------------------------------------------------------------- the parts
volumes(){ part volumes
  echo "$VOLUMES" | while read -r vg lv size fs mnt _; do [ -n "$vg" ] || continue
    dev=/dev/$vg/$lv
    if ! vgs "$vg" >/dev/null 2>&1; then say "$mnt" "volume group $vg is not on this machine: skipped"; continue; fi
    if [ ! -e "$dev" ]; then
      if [ "$size" = ALL ]; then lvcreate -l 100%FREE -n "$lv" "$vg" >/dev/null; else lvcreate -L "$size" -n "$lv" "$vg" >/dev/null; fi
      mkfs."$fs" -q "$dev"; say "$mnt" "made $vg/$lv $size $fs"
    fi
    uuid=$(blkid -s UUID -o value "$dev"); mkdir -p "$mnt"
    grep -q "UUID=$uuid" /etc/fstab || { printf 'UUID=%s  %s  %s  defaults,nofail  0  2\n' "$uuid" "$mnt" "$fs" >> /etc/fstab; say "$mnt" "added to fstab"; }
    findmnt -n "$mnt" >/dev/null || { mount "$mnt"; say "$mnt" "mounted"; }
    say "$mnt" "$(findmnt -n -o SOURCE,SIZE,AVAIL "$mnt")"
  done
  shared
}
# What the operator and the agent both read and write is the shared group's, set-group-id and group-writable, so
# neither ever meets a permission: sources, builds, work, dependencies, data; and the server's extension directories.
shared(){
  getent group "$LAPLACE_GROUP" >/dev/null || return 0
  mkdir -p "$LAPLACE_SRC" "$LAPLACE_BUILD" "$LAPLACE_WORK" "$LAPLACE_DEPS" "$LAPLACE_DATA"
  for d in "$LAPLACE_SRC" "$LAPLACE_BUILD" "$LAPLACE_WORK" "$LAPLACE_DEPS" "$LAPLACE_DATA"; do
    chgrp -R "$LAPLACE_GROUP" "$d" 2>/dev/null || true; chmod -R g+rwX "$d" 2>/dev/null || true; find "$d" -type d -exec chmod g+s {} + 2>/dev/null || true
  done
  install -d "$LAPLACE_PG_DIR/lib" "$LAPLACE_PG_DIR/share/extension"; chgrp "$LAPLACE_PG_GROUP" "$LAPLACE_PG_DIR/lib" "$LAPLACE_PG_DIR/share/extension"; chmod 2775 "$LAPLACE_PG_DIR/lib" "$LAPLACE_PG_DIR/share/extension"
  say "shared trees" "group $LAPLACE_GROUP, set-group-id, group-writable"; say "$LAPLACE_PG_DIR/{lib,share/extension}" "group $LAPLACE_PG_GROUP"
}
packages(){ part packages
  missing=$(for p in $PACKAGES; do dpkg-query -W -f '${Status}\n' "$p" 2>/dev/null | grep -q "install ok installed" || echo "$p"; done)
  if [ -n "$missing" ]; then DEBIAN_FRONTEND=noninteractive apt-get install -y -q $missing; say "installed" "$(echo $missing)"; else say "packages" "all $(echo $PACKAGES | wc -w) present"; fi
  [ -e /opt/intel/oneapi/setvars.sh ] && say "Intel oneAPI" "present" || say "Intel oneAPI" "MISSING: install it from Intel (icx, MKL) and run setup.sh again"
  id "$PGUSER" >/dev/null 2>&1 || { useradd -r -m -d "/var/lib/$PGUSER" -s /bin/bash "$PGUSER"; say "user $PGUSER" "made"; }
  # the users and groups: the server's user; the group the operator and the agent share; the group that installs extensions
  getent group "$LAPLACE_GROUP" >/dev/null || groupadd -r "$LAPLACE_GROUP"
  getent group "$LAPLACE_PG_GROUP" >/dev/null || groupadd -r "$LAPLACE_PG_GROUP"
  id "$LAPLACE_AGENT_USER" >/dev/null 2>&1 || useradd -r -d "$LAPLACE_AGENT_HOME" -s /usr/sbin/nologin -g "$LAPLACE_GROUP" "$LAPLACE_AGENT_USER"
  for u in "$op" "$LAPLACE_AGENT_USER"; do id -nG "$u" | tr ' ' '\n' | grep -qx "$LAPLACE_GROUP" || usermod -aG "$LAPLACE_GROUP" "$u"; id -nG "$u" | tr ' ' '\n' | grep -qx "$LAPLACE_PG_GROUP" || usermod -aG "$LAPLACE_PG_GROUP" "$u"; done
  say "group $LAPLACE_GROUP" "$(getent group "$LAPLACE_GROUP" | cut -d: -f4)"; say "group $LAPLACE_PG_GROUP" "$(getent group "$LAPLACE_PG_GROUP" | cut -d: -f4)"
}
kernel(){ part kernel
  # huge pages: what the server says it needs, 3% over, from the shared_buffers declared above; takes effect at boot
  # and now, as far as free memory allows
  sb=$(echo "$SETTINGS" | awk '$1=="shared_buffers"{print $2}' | tr -d GB); declared=$((sb * 512 * 103 / 100 + 64))   # 2 MB pages for the buffers declared, 3% and the rest of shared memory over
  need=$(q "SHOW shared_memory_size_in_huge_pages" 2>/dev/null || echo 0); [ "${need:-0}" -gt "$declared" ] || need=$declared
  want=$((need + need * 3 / 100))
  printf 'vm.nr_hugepages = %s\n' "$want" > /etc/sysctl.d/60-laplace.conf
  sysctl -q -p /etc/sysctl.d/60-laplace.conf; say "vm.nr_hugepages" "$want (the server needs $need)   reserved now: $(awk '/HugePages_Total/{print $2}' /proc/meminfo)"
}
deps(){ part dependencies
  set +u; . /opt/intel/oneapi/setvars.sh >/dev/null 2>&1 || true; set -u
  [ -d "$LAPLACE_ICU_DIR/lib" ] && say "ICU 78 (Unicode 17)" "$LAPLACE_ICU_DIR" || say "ICU 78" "MISSING: unpack the release into $LAPLACE_ICU_DIR (lib, include); tier 0 is Unicode 17"
  echo "$DEPS" | while read -r name proof how origin branch; do [ -n "$name" ] || continue
    src=$LAPLACE_SRC/$name; b=$LAPLACE_WORK/$name
    if [ -e "$proof" ]; then say "$name" "installed"; continue; fi
    [ -d "$src" ] || { su "$op" -c "git clone -q --depth 1 -b $branch $origin $src" || { say "$name" "could not clone $origin"; continue; }; }
    case $how in
      cmake)     cmake -S "$src" -B "$b" -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PREFIX -DBUILD_TESTING=OFF >/dev/null; cmake --build "$b" >/dev/null; cmake --install "$b" >/dev/null; ldconfig ;;
      headers)   install -d "$PREFIX/include/Spectra"; cp -a "$src/include/Spectra/." "$PREFIX/include/Spectra/" ;;
      configure) mkdir -p "$b"; ( cd "$b"; [ -f config.status ] || CC=icx CXX=icpx "$src/configure" --prefix="$LAPLACE_PG_DIR" --with-icu --with-ssl=openssl --with-lz4 --with-zstd --with-liburing \
                   ICU_CFLAGS="-I$LAPLACE_ICU_DIR/include" ICU_LIBS="-L$LAPLACE_ICU_DIR/lib -licui18n -licuuc -licudata" >/dev/null
                   make -j"$cores" >/dev/null; make install >/dev/null; make -C contrib/pg_stat_statements install >/dev/null; make -C contrib/pg_buffercache install >/dev/null )
                 ln -sfn "$PGBIN/psql" "$PREFIX/bin/psql"; ln -sfn "$PGBIN/pg_config" "$PREFIX/bin/pg_config" ;;
      postgis)   [ -x "$src/configure" ] || su "$op" -c "cd $src && ./autogen.sh" >/dev/null
                 mkdir -p "$b"; ( cd "$b"; [ -f config.status ] || "$src/configure" --with-pgconfig="$PGBIN/pg_config" --with-geosconfig=$PREFIX/bin/geos-config --with-projdir=$PREFIX --with-gdalconfig=$PREFIX/bin/gdal-config >/dev/null
                   make -j"$cores" >/dev/null; make install >/dev/null ); ldconfig ;;
    esac
    [ -e "$proof" ] && say "$name" "built and installed" || say "$name" "DID NOT INSTALL: $b"
  done
  shared
}
cluster(){ part cluster
  install -d -o "$PGUSER" -g "$PGUSER" -m 700 "$PGDATA" "$PGWAL" "$PGTEMP"
  if [ ! -f "$PGDATA/PG_VERSION" ]; then
    # initdb wants an empty directory and the WAL directory is a mount: initialise beside it, then move the cluster in
    stage=$(mktemp -d "$(dirname "$PGDATA")/pgdata-init.XXXX"); chown "$PGUSER" "$stage"
    as_pg "$PGBIN/initdb -D $stage --locale=C.UTF-8 --encoding=UTF8 --data-checksums" >/dev/null
    cp -a "$stage/pg_wal/." "$PGWAL/"; rm -rf "$stage/pg_wal"; cp -a "$stage/." "$PGDATA/"; rm -rf "$stage"
    [ "$PGWAL" = "$PGDATA/pg_wal" ] || { rm -rf "$PGDATA/pg_wal"; ln -s "$PGWAL" "$PGDATA/pg_wal"; }
    chown -R "$PGUSER:$PGUSER" "$PGDATA" "$PGWAL" "$PGTEMP"; say "cluster" "initialised in $PGDATA, WAL in $PGWAL"
  else say "cluster" "$(cat "$PGDATA/PG_VERSION") in $PGDATA"; fi
  chown "$PGUSER:$PGUSER" "$PGWAL" "$PGTEMP"; chmod 700 "$PGWAL" "$PGTEMP"
  cat > "/etc/systemd/system/$SERVICE.service" <<EOF
[Unit]
Description=PostgreSQL on $PGDATA
After=network.target
RequiresMountsFor=$PGDATA $PGWAL $PGTEMP

[Service]
Type=exec
User=$PGUSER
Group=$PGUSER
Environment=PGDATA=$PGDATA
ExecStart=$PGBIN/postgres -D $PGDATA
ExecReload=/bin/kill -HUP \$MAINPID
KillMode=mixed
KillSignal=SIGINT
TimeoutStartSec=60
TimeoutStopSec=infinity

[Install]
WantedBy=multi-user.target
EOF
  systemctl daemon-reload; systemctl enable -q "$SERVICE.service"; systemctl is-active -q "$SERVICE.service" || systemctl start "$SERVICE.service"
  for _ in $(seq 30); do as_pg "$PGBIN/pg_isready -q -h /tmp" && break; sleep 1; done
  say "$SERVICE.service" "$(systemctl is-active "$SERVICE.service")"
  [ "$(q "SELECT count(*) FROM pg_tablespace WHERE spcname = 'pgtemp'")" = 1 ] || run "CREATE TABLESPACE pgtemp LOCATION '$PGTEMP'"
  say "tablespace pgtemp" "$PGTEMP"
  # the server's certificate, before ssl = on is declared: a server told to use TLS without one does not start
  [ -s "$PGDATA/server.crt" ] || { as_pg "openssl req -new -x509 -days 3650 -nodes -subj '/CN=$(hostname)' -keyout $PGDATA/server.key -out $PGDATA/server.crt 2>/dev/null && chmod 600 $PGDATA/server.key"; say "certificate" "made, 10 years"; }
}
settings(){ part settings
  # every setting is stated; the server itself says which of them changed and which need a restart
  while read -r name value; do [ -n "$name" ] || continue
    case $name in shared_preload_libraries) v="$value" ;; *) v="'$value'" ;; esac
    run "ALTER SYSTEM SET $name = $v" 2>/dev/null || say "$name" "REFUSED $value"
  done <<SET
$SETTINGS
SET
  q "SELECT pg_reload_conf()" >/dev/null
  pending=$(q "SELECT string_agg(name, ', ') FROM pg_settings WHERE pending_restart")
  say "settings" "$(echo "$SETTINGS" | grep -c .) declared, reloaded"
  [ -z "$pending" ] || { systemctl restart "$SERVICE.service"; for _ in $(seq 30); do as_pg "$PGBIN/pg_isready -q -h /tmp" && break; sleep 1; done; say "restarted for" "$pending"; }
  [ "$(q "SHOW huge_pages_status")" = on ] || say "huge pages" "NOT IN USE: the server needs $(q "SHOW shared_memory_size_in_huge_pages") pages; sudo ./setup.sh kernel settings"
  q "SELECT '  '||rpad(name, 46)||current_setting(name) FROM pg_settings WHERE name IN ('shared_buffers','huge_pages_status','effective_cache_size','work_mem','maintenance_work_mem','io_method','max_wal_size','wal_buffers','listen_addresses','ssl') ORDER BY name"
}
access(){ part access
  # the operator's password for laplace: made once, kept in the operator's ~/.pgpass only, never printed
  pgpass=$ophome/.pgpass; touch "$pgpass"; chown "$op" "$pgpass"; chmod 600 "$pgpass"
  [ "$(q "SELECT count(*) FROM pg_roles WHERE rolname = '$LAPLACE_ROLE'")" = 1 ] || run "CREATE ROLE $LAPLACE_ROLE LOGIN SUPERUSER"
  if ! grep -q ":$LAPLACE_ROLE:" "$pgpass"; then
    pw=$(openssl rand -base64 24 | tr -d '/+=' | cut -c1-32)
    run "ALTER ROLE $LAPLACE_ROLE PASSWORD '$pw'"
    printf '*:5432:*:%s:%s\n' "$LAPLACE_ROLE" "$pw" >> "$pgpass"; unset pw; say "role $LAPLACE_ROLE" "password made, in $pgpass"
  else say "role $LAPLACE_ROLE" "password in $pgpass"; fi
  printf '%s' "$HBA" > "$PGDATA/pg_hba.conf.setup"; printf '%s' "$IDENT" > "$PGDATA/pg_ident.conf.setup"
  cmp -s "$PGDATA/pg_hba.conf.setup" "$PGDATA/pg_hba.conf" || { cp -p "$PGDATA/pg_hba.conf" "$PGDATA/pg_hba.conf.before" 2>/dev/null || true; mv "$PGDATA/pg_hba.conf.setup" "$PGDATA/pg_hba.conf"; say "pg_hba.conf" "written"; }
  cmp -s "$PGDATA/pg_ident.conf.setup" "$PGDATA/pg_ident.conf" || { mv "$PGDATA/pg_ident.conf.setup" "$PGDATA/pg_ident.conf"; say "pg_ident.conf" "written"; }
  rm -f "$PGDATA/pg_hba.conf.setup" "$PGDATA/pg_ident.conf.setup"; chown "$PGUSER:$PGUSER" "$PGDATA/pg_hba.conf" "$PGDATA/pg_ident.conf"
  q "SELECT pg_reload_conf()" >/dev/null
  if command -v ufw >/dev/null && ufw status | grep -q "^Status: active"; then ufw status | grep -q "5432/tcp.*$LAPLACE_LAN" || { ufw allow from "$LAPLACE_LAN" to any port 5432 proto tcp >/dev/null; say "ufw" "5432/tcp from $LAPLACE_LAN"; }; fi
  q "SELECT '  '||rpad(type, 10)||rpad(array_to_string(user_name, ','), 10)||rpad(coalesce(address, 'socket'), 18)||auth_method||coalesce('   '||error, '') FROM pg_hba_file_rules ORDER BY rule_number"
}
agent(){ part agent
  # A GitHub Actions runner for $LAPLACE_GITHUB/$LAPLACE_AGENT_REPO, as $LAPLACE_AGENT_USER under $LAPLACE_AGENT_HOME:
  # its own sources, builds and work (laplace.env's roots, under its home), the database shared. Registration needs a
  # token from GitHub: gh (logged in as someone who administers the repository) fetches one; or LAPLACE_AGENT_TOKEN.
  home=$LAPLACE_AGENT_HOME; user=$LAPLACE_AGENT_USER; as_agent(){ su -s /bin/bash "$user" -c "$*" </dev/null; }
  install -d -o "$user" -g "$LAPLACE_GROUP" -m 2775 "$home" "$home/build" "$home/work"
  cat > "$home/.laplace.env" <<EOF
# the agent's roots: its checkouts are the workflow's workspace; its builds and work are its own; the database,
# dependencies and data are the machine's
export LAPLACE_BUILD=$home/build LAPLACE_WORK=$home/work
EOF
  chown "$user:$LAPLACE_GROUP" "$home/.laplace.env"
  # the one right it needs: setup.sh of the checkout a workflow made, as root; and to restart the server
  cat > /etc/sudoers.d/laplace-agent <<EOF
$user ALL=(root) NOPASSWD:SETENV: $home/work/runner/*/*/*/setup.sh, /bin/systemctl restart $SERVICE.service
EOF
  chmod 440 /etc/sudoers.d/laplace-agent; visudo -cq -f /etc/sudoers.d/laplace-agent || { rm -f /etc/sudoers.d/laplace-agent; say "sudoers" "REFUSED"; }
  say "sudo" "$user: setup.sh of a checkout under $home/work/runner, systemctl restart $SERVICE.service"
  runner=$home/runner
  if [ ! -x "$runner/config.sh" ]; then
    ver=$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p')
    install -d -o "$user" -g "$LAPLACE_GROUP" "$runner"
    curl -fsSL "https://github.com/actions/runner/releases/download/v$ver/actions-runner-linux-x64-$ver.tar.gz" | as_agent "tar xz -C $runner"
    say "runner" "$ver unpacked in $runner"
  fi
  if [ ! -f "$runner/.runner" ]; then
    token=${LAPLACE_AGENT_TOKEN:-$(su "$op" -c "gh api -X POST repos/$LAPLACE_GITHUB/$LAPLACE_AGENT_REPO/actions/runners/registration-token --jq .token" 2>/dev/null || true)}
    if [ -z "$token" ]; then say "registration" "NO TOKEN: gh auth login as an administrator of $LAPLACE_GITHUB/$LAPLACE_AGENT_REPO, or LAPLACE_AGENT_TOKEN=..., and run setup.sh agent again"; return; fi
    as_agent "cd $runner && ./config.sh --unattended --url https://github.com/$LAPLACE_GITHUB/$LAPLACE_AGENT_REPO --token $token --name $(hostname) --labels $LAPLACE_AGENT_LABELS --work $home/work/runner --replace" >/dev/null
    ( cd "$runner" && ./svc.sh install "$user" >/dev/null && ./svc.sh start >/dev/null ); say "registration" "$(hostname) on $LAPLACE_GITHUB/$LAPLACE_AGENT_REPO, labels $LAPLACE_AGENT_LABELS"
  else say "registration" "$(sed -n 's/.*"agentName": *"\([^"]*\)".*/\1/p' "$runner/.runner") on $LAPLACE_GITHUB/$LAPLACE_AGENT_REPO"; fi
  say "service" "$(cd "$runner" && ./svc.sh status 2>/dev/null | grep -o 'active ([a-z]*)' | head -1)"
}
report(){ part report
  say "server" "$($PGBIN/postgres --version)   $(systemctl is-active "$SERVICE.service")"
  say "PostGIS" "$(q "SELECT default_version FROM pg_available_extensions WHERE name = 'postgis'")"
  say "listening" "$(ss -ltn | awk '$4 ~ /:5432$/ {print $4}' | tr '\n' ' ')"
  addr=$(ip -4 -o addr | awk -v lan="$LAPLACE_LAN" '{split(lan, a, "."); if ($4 ~ "^"a[1]"\\."a[2]"\\."a[3]"\\.") {print $4; exit}}' | cut -d/ -f1)
  say "from the network, as $op" "$(su "$op" -c "$PGBIN/psql -X 'host=$addr port=5432 user=$LAPLACE_ROLE dbname=postgres sslmode=require' -Atc \"SELECT 'connected, TLS '||coalesce((SELECT version FROM pg_stat_ssl WHERE pid = pg_backend_pid()), 'off')\"" 2>&1 | tail -1)"
  say "over the socket, as $op" "$(su "$op" -c "$PGBIN/psql -X 'host=/tmp port=5432 user=$LAPLACE_ROLE dbname=postgres' -Atc \"SELECT 'connected as '||current_user\"" 2>&1 | tail -1)"
  echo; echo "next, as $op: ./deploy.sh"
}

case ${1:-all} in
  all) volumes; packages; kernel; deps; cluster; settings; access; agent; report ;;
  volumes|packages|kernel|deps|cluster|settings|access|agent|report) for p in "$@"; do "$p"; done ;;
  *) echo "setup.sh [volumes|packages|kernel|deps|cluster|settings|access|agent|report]"; exit 2 ;;
esac
echo; echo "=== done   $(date -u +%H:%M:%S)   $log"
