#!/bin/bash
# The repositories' agents on this machine: one GitHub Actions runner per repository, all as $LAPLACE_AGENT_USER,
# separate from anyone's development environment. Declared in laplace.env; converged here; needs root for the user's
# directories, the services and the one right the Engine's runner has.
#
#   sudo ./agents.sh                    every repository of $LAPLACE_REPOS: directory, environment, runner, registration, service, rights
#   sudo ./agents.sh Laplace-postgres   one of them
#   sudo ./agents.sh status             each runner as this machine and as GitHub see it
#   sudo ./agents.sh remove [REPO]      unregister and remove one, or all
#
# Each runner: $LAPLACE_AGENT_HOME/<repo>/{runner,work,build}. Its checkouts are its workflow's workspace under work/;
# its builds are under build/; the database, the dependencies and the data are the machine's, through the shared
# group setup.sh gives them. Registration needs a token from GitHub for each repository: gh, logged in as someone who
# administers them, fetches it; or LAPLACE_AGENT_TOKEN_<REPO with - as _>. Laplace-postgres and Laplace-Engine check
# out Laplace-Native (and Laplace-postgres) beside themselves; a workflow's own token reads only its repository, so
# the operator's gh credential (or LAPLACE_CHECKOUT_TOKEN) is put in those two as the secret LAPLACE_CHECKOUT. What was
# done is kept under $LAPLACE_WORK/logs/root.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd); . "$here/laplace.env"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
op=${SUDO_USER:-$(id -un)}; user=$LAPLACE_AGENT_USER; group=$LAPLACE_GROUP; home=$LAPLACE_AGENT_HOME; owner=$LAPLACE_GITHUB
mkdir -p "$LAPLACE_WORK/logs/root"; log=$LAPLACE_WORK/logs/root/agents-$(date -u +%Y%m%dT%H%M%SZ).log; exec > >(tee "$log") 2>&1
say(){ printf '  %-46s %s\n' "$1" "$2"; }
part(){ echo; echo "=== $1   $(date -u +%H:%M:%S)"; }
as_agent(){ su -s /bin/bash "$user" -c "$*" </dev/null; }
as_op(){ su -s /bin/bash "$op" -c "$*" </dev/null; }
gh_op(){ as_op "gh $*"; }

# the runner's release, once per run
runner_version(){ [ -n "${RUNNER_VERSION:-}" ] || RUNNER_VERSION=$(curl -fsSL https://api.github.com/repos/actions/runner/releases/latest | sed -n 's/.*"tag_name": *"v\([^"]*\)".*/\1/p'); echo "$RUNNER_VERSION"; }

# ---------------------------------------------------------------------------------------------- one repository
agent(){ repo=$1; part "$repo"
  id "$user" >/dev/null 2>&1 || { echo "no user $user: sudo ./setup.sh packages first"; exit 1; }
  d=$home/$repo; r=$d/runner; ws=$d/work/$repo/$repo                             # the workflow's workspace, as the runner lays it out
  # the directories: the agent's, the shared group's, group-writable, so the operator reads its logs and builds
  install -d -o "$user" -g "$group" -m 2775 "$home" "$d" "$r" "$d/work" "$d/build"
  # the environment every job on this runner gets (the runner reads .env at its root): its own builds and work, the
  # machine's dependencies, data and database; its checkouts are the workspace, which the workflow names as LAPLACE_SRC
  cat > "$r/.env" <<EOF
LAPLACE_BUILD=$d/build
LAPLACE_WORK=$d/work
LAPLACE_DEPSRC=$LAPLACE_DEPSRC
LAPLACE_BLAKE3_DIR=$LAPLACE_BLAKE3_DIR
LAPLACE_COREMATH=$LAPLACE_COREMATH
LAPLACE_TREESITTER=$LAPLACE_TREESITTER
LAPLACE_DEPS=$LAPLACE_DEPS
LAPLACE_ICU_DIR=$LAPLACE_ICU_DIR
LAPLACE_DATA=$LAPLACE_DATA
LAPLACE_PG_DIR=$LAPLACE_PG_DIR
LAPLACE_CONNINFO=$LAPLACE_CONNINFO
LAPLACE_TIER0=$LAPLACE_WORK/tier0/tier0.bin
LAPLACE_GRAMMARS=$LAPLACE_BUILD/grammars
EOF
  chown "$user:$group" "$r/.env"; say "environment" "$r/.env"
  # the runner's release
  if [ ! -x "$r/config.sh" ]; then
    v=$(runner_version)
    curl -fsSL "https://github.com/actions/runner/releases/download/v$v/actions-runner-linux-x64-$v.tar.gz" | as_agent "tar xz -C $r"
    "$r/bin/installdependencies.sh" >/dev/null 2>&1 || true
    say "runner" "$v unpacked"
  else say "runner" "$(cat "$r/bin/Runner.Listener.deps.json" 2>/dev/null | sed -n 's/.*"Runner.Listener\/\([0-9.]*\)".*/\1/p' | head -1) present"; fi
  # registered with its repository, named for this machine, labelled with the repository
  name=$(hostname)-$repo
  if [ ! -f "$r/.runner" ]; then
    var=LAPLACE_AGENT_TOKEN_$(echo "$repo" | tr - _); token=${!var:-}
    [ -n "$token" ] || token=$(gh_op "api -X POST repos/$owner/$repo/actions/runners/registration-token --jq .token" 2>/dev/null || true)
    if [ -z "$token" ]; then say "registration" "NO TOKEN: gh auth login as an administrator of $owner/$repo, or $var=..., and run again"; return; fi
    as_agent "cd $r && ./config.sh --unattended --url https://github.com/$owner/$repo --token $token --name $name --labels $LAPLACE_AGENT_LABELS,$repo --work $d/work --replace" >/dev/null
    say "registration" "$name on $owner/$repo, labels $LAPLACE_AGENT_LABELS,$repo"
  else say "registration" "$(sed -n 's/.*"agentName": *"\([^"]*\)".*/\1/p' "$r/.runner") on $owner/$repo"; fi
  # its service
  ( cd "$r"; [ -f .service ] || ./svc.sh install "$user" >/dev/null; ./svc.sh status 2>/dev/null | grep -q "active (running)" || ./svc.sh start >/dev/null )
  say "service" "$(cat "$r/.service" 2>/dev/null)   $(cd "$r" && ./svc.sh status 2>/dev/null | grep -o 'active ([a-z]*)' | head -1)"
  # its rights: the Engine's runner runs setup.sh of its checkout as root, and restarts the server; the others need none
  if [ "$repo" = Laplace-Engine ]; then
    printf '%s ALL=(root) NOPASSWD:SETENV: %s/setup.sh, /bin/systemctl restart %s.service\n' "$user" "$ws/Laplace-Engine" "$LAPLACE_PGSERVICE" > /etc/sudoers.d/laplace-agent
    chmod 440 /etc/sudoers.d/laplace-agent; visudo -cq -f /etc/sudoers.d/laplace-agent || { rm -f /etc/sudoers.d/laplace-agent; say "rights" "REFUSED by visudo"; }
    say "rights" "setup.sh of its checkout as root; restart $LAPLACE_PGSERVICE"
  else say "rights" "none beyond the shared group"; fi
  # the secret that reads the private repositories it checks out beside itself
  case $repo in Laplace-postgres|Laplace-Engine)
    if gh_op "secret list -R $owner/$repo" 2>/dev/null | grep -q "^LAPLACE_CHECKOUT"; then say "secret LAPLACE_CHECKOUT" "set"
    else tok=${LAPLACE_CHECKOUT_TOKEN:-$(gh_op "auth token" 2>/dev/null || true)}              # the operator's own gh credential reads them
      if [ -n "$tok" ]; then as_op "gh secret set LAPLACE_CHECKOUT -R $owner/$repo -b '$tok'" >/dev/null && say "secret LAPLACE_CHECKOUT" "set from the operator's gh login"
      else say "secret LAPLACE_CHECKOUT" "NOT SET: gh auth login, and run again"; fi; unset tok; fi ;;
  esac
}
status(){ part status
  for repo in $LAPLACE_REPOS; do r=$home/$repo/runner
    local_state=$( [ -f "$r/.runner" ] && (cd "$r" && ./svc.sh status 2>/dev/null | grep -o 'active ([a-z]*)' | head -1) || echo "not registered")
    remote=$(gh_op "api repos/$owner/$repo/actions/runners --jq '.runners[] | select(.name == \"$(hostname)-$repo\") | .status + \", busy \" + (.busy|tostring)'" 2>/dev/null || echo "not seen")
    say "$repo" "here: ${local_state:-stopped}   GitHub: ${remote:-not registered}"
  done
}
remove(){ repo=$1; part "remove $repo"; r=$home/$repo/runner
  [ -d "$r" ] || { say "$repo" "nothing here"; return; }
  ( cd "$r"; [ -f .service ] && { ./svc.sh stop >/dev/null 2>&1 || true; ./svc.sh uninstall >/dev/null 2>&1 || true; } )
  if [ -f "$r/.runner" ]; then
    token=$(gh_op "api -X POST repos/$owner/$repo/actions/runners/remove-token --jq .token" 2>/dev/null || true)
    [ -n "$token" ] && as_agent "cd $r && ./config.sh remove --token $token" >/dev/null && say "registration" "removed from $owner/$repo" || say "registration" "could not unregister: remove $(hostname)-$repo on GitHub"
  fi
  rm -rf "$home/$repo"; [ "$repo" = Laplace-Engine ] && rm -f /etc/sudoers.d/laplace-agent; say "$repo" "removed"
}

case ${1:-all} in
  all)     for repo in $LAPLACE_REPOS; do agent "$repo"; done; status ;;
  status)  status ;;
  remove)  shift; for repo in ${*:-$LAPLACE_REPOS}; do remove "$repo"; done ;;
  *)       for repo in "$@"; do echo " $LAPLACE_REPOS " | grep -q " $repo " || { echo "$repo is not in LAPLACE_REPOS"; exit 2; }; agent "$repo"; done; status ;;
esac
echo; echo "=== done   $(date -u +%H:%M:%S)   $log"
