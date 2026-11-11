#!/usr/bin/env bash
# test.sh — run HomeNAS in a throwaway sandbox under /tmp. No sudo, no install.
#
#   ./test.sh            Build, start, print the URL + login; Ctrl-C stops it
#                        and deletes the sandbox.
#   ./test.sh --smoke    Build, start, run the API checks, stop, report.
#   ./test.sh --keep     As the default, but leave the sandbox behind on exit.
#   ./test.sh --clean    Delete the build cache and any leftover sandboxes.
#
# Nothing outside /tmp is touched: no systemd unit, no NGINX config, no
# /etc/hosts entry, no certificate, no SELinux or firewall change, and the
# repo checkout itself is never modified (setup.sh rewrites tracked files;
# this works on a copy).
#
# Differences from a real install, so nobody is surprised:
#   - No NGINX and no TLS. Drogon serves the web UI itself over plain HTTP on
#     127.0.0.1, with webui/ as its document root and sync-portal/ under /sync/.
#   - The hardcoded install paths in the backend sources are rewritten to be
#     relative to the sandbox in the copy that gets compiled.
#   - If Drogon isn't installed system-wide it is built once into the cache
#     directory below (a few minutes, first run only).
#
# Environment:
#   PORT=9000 ./test.sh           Use a specific port (default: 8080 if free,
#                                 otherwise a random free one).
#   PRIVATE_DROGON=1 ./test.sh    Ignore a system Drogon; use the cached build.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# Per-user and persistent across runs: holds the Drogon build and the backend
# build tree, so only the first run is slow. The sandbox itself is random.
CACHE="/tmp/homenas-test-cache-$(id -u)"
SANDBOX_GLOB="/tmp/homenas-test.*"

MODE=run
KEEP=0
for arg in "$@"; do
    case "$arg" in
        --smoke)   MODE=smoke ;;
        --keep)    KEEP=1 ;;
        --clean)   MODE=clean ;;
        --help|-h) sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $arg (try --help)" >&2; exit 1 ;;
    esac
done

say() { printf '\033[1m==> %s\033[0m\n' "$*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

if [ "$MODE" = clean ]; then
    for d in "$CACHE" $SANDBOX_GLOB; do
        if [ -d "$d" ] && [ -O "$d" ]; then echo "removing $d"; rm -rf "$d"; fi
    done
    exit 0
fi

# ── Preconditions ────────────────────────────────────────────────────────────
[ "$(id -u)" -ne 0 ] || die "don't run this as root — it needs no privileges."

missing=()
for tool in cmake ninja g++ git curl python3 rsync; do
    command -v "$tool" >/dev/null || missing+=("$tool")
done
for hdr in json/json.h uuid/uuid.h openssl/evp.h zlib.h sqlite3.h; do
    [ -e "/usr/include/$hdr" ] || [ -e "/usr/include/jsoncpp/$hdr" ] || missing+=("$hdr")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "Missing build prerequisites: ${missing[*]}" >&2
    echo "This script installs nothing. On Fedora:" >&2
    echo "  sudo dnf install cmake ninja-build gcc-c++ git curl python3 rsync \\" >&2
    echo "      jsoncpp-devel libuuid-devel openssl-devel zlib-devel sqlite-devel" >&2
    exit 1
fi

mkdir -p -m 700 "$CACHE"
[ -O "$CACHE" ] || die "$CACHE exists but belongs to someone else."

# ── Drogon ───────────────────────────────────────────────────────────────────
CMAKE_PREFIX=()
if [ -z "${PRIVATE_DROGON:-}" ] && { [ -f /usr/local/lib64/cmake/Drogon/DrogonConfig.cmake ] ||
                                     [ -f /usr/local/lib/cmake/Drogon/DrogonConfig.cmake ] ||
                                     [ -f /usr/lib64/cmake/Drogon/DrogonConfig.cmake ]; }; then
    say "Using the system Drogon"
else
    DROGON_PREFIX="$CACHE/drogon-prefix"
    if [ ! -f "$DROGON_PREFIX/.complete" ]; then
        say "Building Drogon into $DROGON_PREFIX (first run only)"
        if [ ! -d "$CACHE/drogon-src/.git" ]; then
            rm -rf "$CACHE/drogon-src"
            git clone --depth=1 --recurse-submodules \
                https://github.com/drogonframework/drogon "$CACHE/drogon-src"
        fi
        # SQLite stays on: the backend gets its sqlite3 symbols through Drogon
        # (see backend/CMakeLists.txt). Everything else is unused here.
        cmake -S "$CACHE/drogon-src" -B "$CACHE/drogon-build" -GNinja \
            -DCMAKE_BUILD_TYPE=Release \
            -DCMAKE_INSTALL_PREFIX="$DROGON_PREFIX" \
            -DBUILD_TESTING=OFF -DBUILD_EXAMPLES=OFF -DBUILD_CTL=OFF \
            -DBUILD_POSTGRESQL=OFF -DBUILD_MYSQL=OFF -DBUILD_REDIS=OFF \
            -DBUILD_SQLITE=ON
        cmake --build "$CACHE/drogon-build" -j"$(nproc)"
        cmake --install "$CACHE/drogon-build" >/dev/null
        touch "$DROGON_PREFIX/.complete"
    else
        say "Using the cached Drogon in $DROGON_PREFIX"
    fi
    CMAKE_PREFIX=(-DCMAKE_PREFIX_PATH="$DROGON_PREFIX")
fi

# ── Sandbox ──────────────────────────────────────────────────────────────────
SB="$(mktemp -d /tmp/homenas-test.XXXXXX)"
BACKEND_PID=""
cleanup() {
    local rc=$?
    trap - EXIT INT TERM
    if [ -n "$BACKEND_PID" ] && kill -0 "$BACKEND_PID" 2>/dev/null; then
        kill -TERM "$BACKEND_PID" 2>/dev/null || true
        # The backend joins its writer threads on SIGTERM; give it a moment
        # before insisting.
        for _ in $(seq 30); do kill -0 "$BACKEND_PID" 2>/dev/null || break; sleep 0.1; done
        kill -KILL "$BACKEND_PID" 2>/dev/null || true
    fi
    if [ "$KEEP" = 1 ]; then
        echo; echo "Sandbox kept: $SB   (remove with: rm -rf $SB)"
    else
        rm -rf "$SB"
    fi
    exit "$rc"
}
trap cleanup EXIT INT TERM

NAS_MAIN="$SB/nas/nas_main"
NAS_ROOT="$SB/nas/nas_storage"
BACKUP_ROOT="$SB/nas/nas_backup"
mkdir -p "$NAS_MAIN"/{webui,logs,tmp_uploads} "$NAS_ROOT" "$BACKUP_ROOT"

# ── Build ────────────────────────────────────────────────────────────────────
say "Building the backend"
# The sources carry absolute install paths — either the home_path placeholder
# or whatever setup.sh last substituted for it. Rewriting them to "./nas/..."
# (the backend is started with the sandbox as its working directory) keeps the
# compiled sources byte-identical from run to run, so the cached build tree
# stays incremental even though the sandbox path is random.
STAGE="$SB/src-stage"
rsync -a --exclude build "$REPO/backend/" "$STAGE/"
find "$STAGE" -type f \( -name '*.cpp' -o -name '*.h' \) -exec \
    sed -i -E 's#(home_path|/home/[^/"]+)/nas/#./nas/#g' {} +
# --checksum: only files whose content changed get a new mtime, so ninja
# rebuilds exactly what you edited.
rsync -a --checksum --delete "$STAGE/" "$CACHE/backend-src/"
rm -rf "$STAGE"

cmake -S "$CACHE/backend-src" -B "$CACHE/backend-build" -GNinja \
    -DCMAKE_BUILD_TYPE=Release "${CMAKE_PREFIX[@]}" >/dev/null
cmake --build "$CACHE/backend-build" -j"$(nproc)"
cp "$CACHE/backend-build/nas_backend" "$NAS_MAIN/nas_backend"

# ── Deploy into the sandbox ──────────────────────────────────────────────────
cp -r "$REPO/webui/." "$NAS_MAIN/webui/"
# NGINX aliases /sync/ to sync-portal/; nesting it under the document root is
# the equivalent when Drogon is the one serving static files.
mkdir -p "$NAS_MAIN/webui/sync"
cp -r "$REPO/sync-portal/." "$NAS_MAIN/webui/sync/"

if [ -n "${PORT:-}" ]; then
    :
elif ! (exec 3<>/dev/tcp/127.0.0.1/8080) 2>/dev/null; then
    PORT=8080
else
    PORT="$(python3 -I -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')"
fi

ADMIN_USER=admin
ADMIN_PASS="test-$(head -c 6 /dev/urandom | od -An -tx1 | tr -d ' \n')"

python3 -I - "$REPO/config.json" "$NAS_MAIN/config.json" \
    "$SB" "$PORT" "$ADMIN_PASS" <<'PY'
import json, re, secrets, sys
src, dst, sb, port, password = sys.argv[1:6]

with open(src) as f:
    text = f.read()
# Same rewrite as the sources, but to the absolute sandbox path: SyncManager
# compares nas_root and backup_root, and absolute paths keep that unambiguous.
text = re.sub(r'(home_path|/home/[^/"]+)/nas/', sb + '/nas/', text)
cfg = json.loads(text)

cfg['listeners'] = [{'address': '127.0.0.1', 'port': int(port), 'https': False}]
app = cfg['app']
app['document_root'] = sb + '/nas/nas_main/webui'   # stands in for NGINX
app['threads_num'] = 2
cc = cfg['custom_config']
cc['jwt_secret'] = secrets.token_hex(32)
cc['admin_password'] = password
cc['events_db_path'] = sb + '/nas/nas_storage/.nas-meta/events.db'
cc['alerts_db_path'] = sb + '/nas/nas_storage/.nas-meta/alerts.db'
cc['sync']['schedule_cron'] = ''   # no surprise 3am run in a test sandbox

with open(dst, 'w') as f:
    json.dump(cfg, f, indent=2)
PY

# A little content so the file manager isn't empty on first load.
mkdir -p "$NAS_ROOT/Documents" "$NAS_ROOT/Photos"
echo "Hello from the HomeNAS test sandbox." > "$NAS_ROOT/Documents/readme.txt"
head -c 262144 /dev/urandom > "$NAS_ROOT/Photos/sample.bin"

# ── Start ────────────────────────────────────────────────────────────────────
BASE="http://127.0.0.1:$PORT"
say "Starting the backend on $BASE"
( cd "$SB" && exec ./nas/nas_main/nas_backend ) > "$SB/backend.log" 2>&1 &
BACKEND_PID=$!

for _ in $(seq 50); do
    kill -0 "$BACKEND_PID" 2>/dev/null || { cat "$SB/backend.log" >&2; die "backend exited during startup."; }
    curl -fsS -o /dev/null "$BASE/" 2>/dev/null && break
    sleep 0.2
done
curl -fsS -o /dev/null "$BASE/" || { cat "$SB/backend.log" >&2; die "backend never answered on $BASE."; }

# ── Smoke tests ──────────────────────────────────────────────────────────────
run_smoke() {
    local pass=0 fail=0 token out code
    ok()  { pass=$((pass+1)); printf '  \033[32mPASS\033[0m  %s\n' "$1"; }
    bad() { fail=$((fail+1)); printf '  \033[31mFAIL\033[0m  %s\n        got: %s\n' "$1" "${2:0:300}"; }
    # check NAME EXPECTED_SUBSTRING ACTUAL
    check() { if [[ "$3" == *"$2"* ]]; then ok "$1"; else bad "$1" "$3"; fi; }
    api() {   # api METHOD PATH [curl args...] — prints "<body> <http-code>"
        local m="$1" p="$2"; shift 2
        curl -sS -X "$m" "$BASE$p" -H "Authorization: Bearer $token" -w ' %{http_code}' "$@"
    }
    json() { curl -sS -X "$1" "$BASE$2" -H "Authorization: Bearer $token" \
                 -H 'Content-Type: application/json' -d "$3" -w ' %{http_code}'; }

    say "Static files"
    check "web UI index served"        "<html"  "$(curl -sS "$BASE/")"
    check "web UI app.js served"       "200"    "$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/app.js")"
    check "sync portal served at /sync/" "200"  "$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/sync/")"

    say "Auth"
    token=""
    check "API rejects a missing token" "401" "$(api GET '/api/ls?path=/')"
    check "wrong password is rejected"  "401" \
        "$(json POST /api/auth/login '{"username":"admin","password":"nope"}')"
    out="$(curl -sS -X POST "$BASE/api/auth/login" -H 'Content-Type: application/json' \
            -d "{\"username\":\"$ADMIN_USER\",\"password\":\"$ADMIN_PASS\"}")"
    token="$(python3 -I -c 'import json,sys; print(json.load(sys.stdin).get("token",""))' <<<"$out" 2>/dev/null || true)"
    if [ -n "$token" ]; then ok "login returns a JWT"; else bad "login returns a JWT" "$out"; fi
    check "forged token is rejected" "401" \
        "$(curl -sS -o /dev/null -w '%{http_code}' "$BASE/api/ls?path=/" -H "Authorization: Bearer ${token}x")"

    say "Filesystem"
    check "ls lists seeded content"  '"Documents"' "$(api GET '/api/ls?path=/')"
    check "mkdir"                    "200" "$(json POST /api/mkdir '{"path":"smoke_a"}')"
    check "mkdir (second)"           "200" "$(json POST /api/mkdir '{"path":"smoke_b"}')"
    echo "hello nas" > "$SB/upload.txt"
    check "upload"                   '"saved"' "$(api POST '/api/upload?path=smoke_a' -F "file=@$SB/upload.txt")"
    check "rename within a folder"   "200" \
        "$(json POST /api/rename '{"from":"smoke_a/upload.txt","to":"smoke_a/renamed.txt"}')"
    check "move across folders"      "200" \
        "$(json POST /api/rename '{"from":"smoke_a/renamed.txt","to":"smoke_b/renamed.txt"}')"
    check "download returns the uploaded bytes" "hello nas" "$(api GET '/api/file?path=smoke_b/renamed.txt')"
    [ -f "$NAS_ROOT/smoke_b/renamed.txt" ] && ok "file is on disk in the sandbox" \
        || bad "file is on disk in the sandbox" "$(ls -R "$NAS_ROOT")"
    # 2 MB: catches a regression of the 1 MB Drogon body cap (client_max_body_size).
    head -c 2097152 /dev/urandom > "$SB/big.bin"
    check "upload larger than 1 MB"  '"saved"' "$(api POST '/api/upload?path=smoke_a' -F "file=@$SB/big.bin")"
    code="$(curl -sS -o /dev/null -w '%{http_code}' -H "Authorization: Bearer $token" \
            --path-as-is "$BASE/api/file?path=../../../../etc/passwd")"
    if [ "$code" != 200 ]; then ok "path traversal is refused ($code)"; else bad "path traversal is refused" "$code"; fi
    check "delete file"              "200" "$(api DELETE '/api/file?path=smoke_b/renamed.txt')"
    check "delete directory"         "200" "$(api DELETE '/api/file?path=smoke_a')"

    say "Audit log and detection"
    sleep 1.5   # EventWriter flushes every 250ms; the analyzer runs after each flush
    out="$(api GET '/api/events?limit=100')"
    for ev in auth.login.failure auth.login.success dir.create file.upload file.rename file.move file.download file.delete dir.delete; do
        check "event recorded: $ev" "\"$ev\"" "$out"
    done
    check "events stats summary"       "200" "$(api GET '/api/events/stats/summary')"
    check "events CSV export"          "200" "$(api GET '/api/events/export')"
    check "MD-006 alert fired for the directory delete" '"MD-006"' "$(api GET '/api/alerts?limit=50')"
    check "alerts stats summary"       '"total_open"' "$(api GET '/api/alerts/stats/summary')"

    say "Sync"
    check "sync status"                '"state"' "$(api GET /api/sync/status)"
    check "sync start"                 "200"     "$(api POST /api/sync/start)"
    for _ in $(seq 40); do
        [ -f "$BACKUP_ROOT/Documents/readme.txt" ] && break; sleep 0.25
    done
    [ -f "$BACKUP_ROOT/Documents/readme.txt" ] && ok "backup run copied nas_storage to nas_backup" \
        || bad "backup run copied nas_storage to nas_backup" "$(api GET '/api/sync/logs?limit=10')"
    check "sync run history"           '"items"' "$(api GET '/api/sync/runs?limit=5')"

    echo
    if [ "$fail" -eq 0 ]; then
        printf '\033[32mAll %d checks passed.\033[0m\n' "$pass"
    else
        printf '\033[31m%d of %d checks failed.\033[0m  Backend log: %s\n' "$fail" $((pass+fail)) "$SB/backend.log"
        [ "$KEEP" = 1 ] || { echo "--- last backend log lines"; tail -n 20 "$SB/backend.log"; }
        return 1
    fi
}

if [ "$MODE" = smoke ]; then
    run_smoke
    exit $?
fi

cat <<INFO

======================================================
 HomeNAS test sandbox is up
======================================================
   URL:       $BASE
   Username:  $ADMIN_USER
   Password:  $ADMIN_PASS

   Sandbox:   $SB
   Storage:   $NAS_ROOT
   Backup:    $BACKUP_ROOT
   Config:    $NAS_MAIN/config.json
   Log:       $SB/backend.log

 Ctrl-C stops the backend$([ "$KEEP" = 1 ] && echo " (sandbox is kept)." || echo " and deletes the sandbox.")
======================================================

INFO

wait "$BACKEND_PID" || true
BACKEND_PID=""
echo "Backend exited. Last log lines:"; tail -n 15 "$SB/backend.log" 2>/dev/null || true
