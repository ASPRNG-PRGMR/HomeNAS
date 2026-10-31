#!/usr/bin/env bash
# setup.sh — NAS stack installer/uninstaller for Fedora 43
#
#   sudo bash setup.sh --install      (default if no flag is given)
#   sudo bash setup.sh --uninstall
#   bash setup.sh --help
#
# The uninstaller removes everything the installer puts on the system and
# leaves your stored files alone — see do_uninstall() for exactly what is
# kept and why.

set -euo pipefail

_USER="${SUDO_USER:-$USER}"
_HOME="$(eval echo "~$_USER")"
NAS_ROOT="$_HOME/nas/nas_storage"
# One-way backup target for the sync engine. Must live outside NAS_ROOT —
# SyncManager refuses to run if it is nested inside, since it would copy its
# own output back into itself on every pass.
BACKUP_ROOT="$_HOME/nas/nas_backup"
INSTALL_DIR="$_HOME/nas/nas_main"
NAS_USER="$_USER"

# Small state directory outside the install tree. It holds the record of what
# we installed into /usr/local (Drogon's install manifest), which the
# uninstaller needs *after* it has deleted INSTALL_DIR — and which would
# otherwise only exist in /tmp, i.e. gone after a reboot.
STATE_DIR="/usr/local/share/homenas"
DROGON_MANIFEST="$STATE_DIR/drogon-install-manifest.txt"

# Best-effort step. The uninstaller must keep going when a given piece was
# never installed, or was already removed by hand — a half-finished install
# still has to uninstall cleanly, and `set -e` would abort on the first
# missing item.
try() { "$@" 2>/dev/null || true; }

require_root() {
    if [ "$(id -u)" -ne 0 ]; then
        echo "This must run as root:  sudo bash setup.sh ${1:-}" >&2
        exit 1
    fi
}

usage() {
    cat <<USAGE
setup.sh — HomeNAS installer/uninstaller

  sudo bash setup.sh --install     Build and install the stack (default)
  sudo bash setup.sh --uninstall   Remove everything this installer created
  bash setup.sh --help             Show this message

--uninstall deliberately keeps your stored files:
  $NAS_ROOT
  $BACKUP_ROOT
Remove those by hand if you also want the data gone.
USAGE
}

# ─────────────────────────────────────────────────────────────────────────────
#  INSTALL
# ─────────────────────────────────────────────────────────────────────────────
do_install() {
require_root --install

echo "==> Installing dependencies"
dnf install -y \
    libuuid-devel\
    cmake ninja-build gcc-c++ \
    openssl-devel zlib-devel jsoncpp-devel \
    brotli-devel \
    sqlite-devel \
    git \
    nginx \
    tailscale \
    python3

echo "==> Installing Drogon"
# Check lib64 as well as lib — on Fedora (and any multilib distro) CMake
# installs to /usr/local/lib64, so a lib-only check never sees an existing
# install and we'd needlessly rebuild Drogon on every run.
if [ -f /usr/local/lib/cmake/Drogon/DrogonConfig.cmake ] || \
   [ -f /usr/local/lib64/cmake/Drogon/DrogonConfig.cmake ]; then
    echo "Drogon already installed. Skipping..."
else
    # Only clone if we don't already have a tree. A leftover /tmp/drogon used
    # to mean "skip entirely", which silently left Drogon uninstalled and the
    # backend build failing further down — resume the build/install instead.
    if [ -d /tmp/drogon/.git ]; then
        echo "Reusing existing /tmp/drogon clone..."
    else
        rm -rf /tmp/drogon
        git clone --depth=1 --recurse-submodules \
            https://github.com/drogonframework/drogon /tmp/drogon
    fi

    # BUILD_EXAMPLES defaults to ON upstream, and `cmake --install` then drops
    # a dozen demo binaries with very generic names (helloworld, client,
    # benchmark, cors, file_upload, ...) straight into /usr/local/bin. We have
    # no use for them and they're indistinguishable from someone else's tools
    # once they're there.
    cmake -S /tmp/drogon -B /tmp/drogon/build \
        -DCMAKE_BUILD_TYPE=Release \
        -GNinja \
        -DBUILD_TESTING=OFF \
        -DBUILD_EXAMPLES=OFF

    cmake --build /tmp/drogon/build -j"$(nproc)"
    cmake --install /tmp/drogon/build
    ldconfig

    # Keep the manifest so --uninstall can remove exactly the files we added
    # to /usr/local, rather than guessing at paths. Its presence also records
    # that *we* installed Drogon: without it, --uninstall leaves a
    # pre-existing Drogon alone.
    mkdir -p "$STATE_DIR"
    if [ -f /tmp/drogon/build/install_manifest.txt ]; then
        cp /tmp/drogon/build/install_manifest.txt "$DROGON_MANIFEST"
    fi
fi


echo "==> Creating NAS system user"
id -u "$NAS_USER" &>/dev/null || \
    useradd -r -s /sbin/nologin -d "$INSTALL_DIR" "$NAS_USER"

echo "==> Creating directories"
mkdir -p "$NAS_ROOT"
mkdir -p "$BACKUP_ROOT"
mkdir -p "$INSTALL_DIR"/{webui,sync-portal,logs,tmp_uploads}


# TO PUT HOME PATHS IN RESPECTIVE FILES
# NOTE: EventsController.cpp added here — it carries its own home_path
# fallback default (events_db_path), same as Filesystem/UploadController's
# nas_root fallback. Without this, that one fallback string would stay
# literally "home_path/..." forever (harmless as long as config.json's
# events_db_path key is always present, but a landmine if it's ever missing).

sed -i "s|home_path|$_HOME|g"\
        backend/controllers/FilesystemController.cpp \
        backend/controllers/UploadController.cpp \
        backend/controllers/EventsController.cpp \
        backend/controllers/AlertsController.cpp \
        backend/main.cpp\
        config.json\
        nginx/nas.conf

echo "==> Building backend"
cmake -S backend -B backend/build \
    -DCMAKE_BUILD_TYPE=Release \
    -GNinja
cmake --build backend/build -j"$(nproc)"
if [ -f "$INSTALL_DIR/nas_backend" ]; then
    rm -f "$INSTALL_DIR/nas_backend"
fi
cp backend/build/nas_backend "$INSTALL_DIR"
cp config.json "$INSTALL_DIR"

echo "==> Deploying Web UI"
cp -r webui/* "$INSTALL_DIR/webui/"

echo "==> Deploying sync portal (served by NGINX at /sync/)"
cp -r sync-portal/* "$INSTALL_DIR/sync-portal/"

echo "==> Setting ownership"
chown -R "$NAS_USER":"$NAS_USER" "$NAS_ROOT"
chown -R "$NAS_USER":"$NAS_USER" "$BACKUP_ROOT"
chown -R "$NAS_USER":"$NAS_USER" "$INSTALL_DIR"

echo "==> Setting SELinux contexts"
# Allow NGINX to proxy to localhost:8080
setsebool -P httpd_can_network_relay 1
setsebool -P httpd_can_network_connect 1

# Label the webui directory so NGINX can read it
semanage fcontext -a -t httpd_sys_content_t "$INSTALL_DIR/webui(/.*)?" || \
semanage fcontext -m -t httpd_sys_content_t "$INSTALL_DIR/webui(/.*)?"
restorecon -Rv "$INSTALL_DIR/webui"

# Label the sync-portal directory the same way — NGINX serves it at /sync/
# on the main domain, so it needs the same read access as webui/.
semanage fcontext -a -t httpd_sys_content_t "$INSTALL_DIR/sync-portal(/.*)?" || \
semanage fcontext -m -t httpd_sys_content_t "$INSTALL_DIR/sync-portal(/.*)?"
restorecon -Rv "$INSTALL_DIR/sync-portal"

# Label the backend binary bin_t so systemd can exec it. The binary lives
# under $HOME, so it otherwise inherits user_home_t, and init_t is denied
# `execute` on that type — the unit then dies at step EXEC with
# status=203/EXEC ("Permission denied") even though the file is mode 0755
# and runs fine when launched by hand from a user shell. Done via semanage
# (not chcon) so the label survives a filesystem relabel.
semanage fcontext -a -t bin_t "$INSTALL_DIR/nas_backend" || \
semanage fcontext -m -t bin_t "$INSTALL_DIR/nas_backend"
restorecon -v "$INSTALL_DIR/nas_backend"

# Label nas_root and the backup target so the backend can read/write them
semanage fcontext -a -t var_t "$NAS_ROOT(/.*)?" || true
restorecon -Rv "$NAS_ROOT"
semanage fcontext -a -t var_t "$BACKUP_ROOT(/.*)?" || true
restorecon -Rv "$BACKUP_ROOT"

echo "==> Generating self-signed TLS certificate"
mkdir -p /etc/ssl/nas
# subjectAltName is required, not optional: Chrome/Brave (and every current
# browser) ignore the CN entirely and validate the hostname against SAN only.
# A CN-only cert gets ERR_CERT_COMMON_NAME_INVALID even when the CN matches
# exactly. Both nas.local and the loopback IPs are listed so connecting by
# either name or address gives a plain "self-signed" warning rather than an
# additional name-mismatch one.
openssl req -x509 -nodes -newkey rsa:4096 -days 3650 \
    -keyout /etc/ssl/nas/key.pem \
    -out    /etc/ssl/nas/cert.pem \
    -subj   "/CN=nas.local" \
    -addext "subjectAltName=DNS:nas.local,IP:127.0.0.1,IP:::1"
chmod 600 /etc/ssl/nas/key.pem

echo "==> Registering nas.local in /etc/hosts"
# Makes https://nas.local work on this machine with no Tailscale and no DNS.
# Matched on a word-boundary hostname rather than a plain substring so an
# unrelated entry (e.g. "my-nas.local.example") can't be mistaken for ours,
# and so re-running setup.sh doesn't append a duplicate line each time.
if grep -qE '^[^#]*[[:space:]]nas\.local([[:space:]]|$)' /etc/hosts; then
    echo "nas.local already present in /etc/hosts. Skipping..."
else
    printf '\n# HomeNAS dashboard\n127.0.0.1\tnas.local\n::1\t\tnas.local\n' >> /etc/hosts
    echo "Added nas.local -> 127.0.0.1 / ::1"
fi

echo "==> Installing NGINX config"
cp nginx/nas.conf /etc/nginx/conf.d/nas.conf
rm -f /etc/nginx/conf.d/default.conf
nginx -t
systemctl enable --now nginx

echo "==> Opening firewall ports"
firewall-cmd --permanent --add-service=https
firewall-cmd --permanent --add-service=http
# No extra ports: the sync portal is served by NGINX at /sync/ over 443,
# replacing the stopgap listener's daily-rotating port (devlog Phase 3.2).
firewall-cmd --reload

echo "==> Enabling Tailscale"
systemctl enable --now tailscaled

echo "==> Installing and running systemd service"
cp systemd/nas-backend.service /etc/systemd/system/
sed -i "s|_home_|$_HOME|g" /etc/systemd/system/nas-backend.service
sed -i "s|_user_|$_USER|g" /etc/systemd/system/nas-backend.service
chmod o+x $_HOME
chmod o+rx $_HOME/nas
chmod o+rx $_HOME/nas/nas_main
chmod -R o+rX $_HOME/nas/nas_main/webui
systemctl daemon-reload
systemctl reload nginx
systemctl restart nas-backend

echo ""
echo "======================================================"
echo " Done! The dashboard is now at:  https://nas.local"
echo "======================================================"
echo ""
echo " Accept the self-signed certificate warning on first visit."
echo " nas.local is already mapped to this machine in /etc/hosts,"
echo " so no Tailscale and no DNS setup is needed for local access."
echo ""
echo " Required manual steps:"
echo ""
echo " 1. Edit $INSTALL_DIR/config.json"
echo "    - Set jwt_secret  → a long random string"
echo "    - Set admin_password → your chosen password"
echo "    (Edit the deployed copy above, not the one in the repo —"
echo "     the repo copy is version-controlled.)"
echo ""
echo " 2. Restart the backend:"
echo "    sudo systemctl restart nas-backend"
echo ""
echo " Optional — access from other machines, via Tailscale:"
echo ""
echo " 3. Connect to Tailscale:"
echo "    sudo tailscale up"
echo ""
echo " 4. On each *client* machine, add to its /etc/hosts:"
echo "    <tailscale-ip>  nas.local"
echo "    (or just use the Tailscale IP directly)"
echo ""
echo " To remove all of this again:  sudo bash setup.sh --uninstall"
echo ""
}

# ─────────────────────────────────────────────────────────────────────────────
#  UNINSTALL
# ─────────────────────────────────────────────────────────────────────────────
# Reverses do_install() in roughly the opposite order it ran, so nothing is
# in use while it's being removed. Every step is best-effort (see try()):
# uninstalling a partial or already-partly-removed install has to work.
do_uninstall() {
require_root --uninstall

echo "==> Stopping and removing the systemd service"
try systemctl stop nas-backend
try systemctl disable nas-backend
try rm -f /etc/systemd/system/nas-backend.service
try systemctl daemon-reload
try systemctl reset-failed nas-backend

# Older installs forked a detached `python3 -m http.server` for the sync
# portal that deliberately outlived the backend (devlog Phase 3.2). Nothing
# spawns it any more, but an old one can still be running right now, and
# stopping the unit would not have taken it with it.
try pkill -f "http.server.*sync-portal"

echo "==> Removing NGINX config"
try rm -f /etc/nginx/conf.d/nas.conf
# Only reload if nginx is actually up and still has a valid config — on a
# machine where nginx serves other sites we must not leave it down, and on
# one where it doesn't, there's nothing to reload.
if systemctl is-active --quiet nginx; then
    if nginx -t >/dev/null 2>&1; then
        try systemctl reload nginx
    else
        echo "    NGINX config is invalid after removal — leaving it running;"
        echo "    check 'nginx -t' yourself."
    fi
fi

echo "==> Closing firewall ports"
try firewall-cmd --permanent --remove-service=https
try firewall-cmd --permanent --remove-service=http
# Installs from before Phase 3.3 opened the whole dynamic range for the
# rotating sync-portal port. Harmless if it was never added.
try firewall-cmd --permanent --remove-port=49152-65535/tcp
try firewall-cmd --reload

echo "==> Removing nas.local from /etc/hosts"
# Drops our marker comment and any nas.local mapping. Same word-boundary
# match the installer uses, so an unrelated host like "my-nas.local.example"
# is left alone.
if [ -f /etc/hosts ]; then
    try sed -i -e '/^# HomeNAS dashboard$/d' \
               -e '/^[^#]*[[:space:]]nas\.local\([[:space:]]\|$\)/d' /etc/hosts
    # The installer prepends a blank line as a separator; without this the
    # file keeps growing a stray empty line at the end on every
    # install/uninstall cycle. Only touches blank lines at EOF.
    try sed -i -e :a -e '/^\n*$/{$d;N;};/\n$/ba' /etc/hosts
fi

echo "==> Removing TLS certificate"
try rm -rf /etc/ssl/nas

echo "==> Reverting SELinux configuration"
try semanage fcontext -d "$INSTALL_DIR/webui(/.*)?"
try semanage fcontext -d "$INSTALL_DIR/sync-portal(/.*)?"
try semanage fcontext -d "$INSTALL_DIR/nas_backend"
try semanage fcontext -d "$NAS_ROOT(/.*)?"
try semanage fcontext -d "$BACKUP_ROOT(/.*)?"
# These booleans are system-wide, not ours alone. Turning them back off is
# the clean thing to do, but if you run another web app that proxies or makes
# outbound connections, re-enable them with:
#   sudo setsebool -P httpd_can_network_relay 1
try setsebool -P httpd_can_network_relay 0
try setsebool -P httpd_can_network_connect 0

echo "==> Removing the install directory"
try rm -rf "$INSTALL_DIR"

# Kept directories were labelled var_t by the installer; now that the rules
# are gone, put them back to whatever the default policy says they should be.
if [ -d "$NAS_ROOT" ];    then try restorecon -R "$NAS_ROOT";    fi
if [ -d "$BACKUP_ROOT" ]; then try restorecon -R "$BACKUP_ROOT"; fi

echo "==> Reverting home directory permissions"
# The installer widened these so NGINX could traverse down to webui/. Undo
# that — it only ever loosened them, so tightening back is the safe
# direction even if they happened to be set beforehand.
try chmod o-x "$_HOME"
if [ -d "$_HOME/nas" ]; then try chmod o-rx "$_HOME/nas"; fi

echo "==> Removing Drogon from /usr/local"
if [ -f "$DROGON_MANIFEST" ]; then
    # Remove exactly what we installed, by manifest — never by glob.
    while IFS= read -r f; do
        if [ -n "$f" ]; then try rm -f "$f"; fi
    done < "$DROGON_MANIFEST"
    # Then the directories, which the manifest doesn't list. rmdir (not rm
    # -rf) so anything else living in them survives.
    for d in /usr/local/include/drogon /usr/local/include/trantor \
             /usr/local/lib64/cmake/Drogon /usr/local/lib64/cmake/Trantor \
             /usr/local/lib/cmake/Drogon /usr/local/lib/cmake/Trantor; do
        try find "$d" -depth -type d -empty -delete
    done
    try ldconfig
    echo "    Removed per install manifest."
else
    # No manifest means either Drogon predates this installer or it was
    # installed by an older setup.sh. Leave it: deleting shared libraries we
    # can't prove we own is how you break someone else's build.
    echo "    No install manifest at $DROGON_MANIFEST — leaving Drogon in"
    echo "    place. If this installer did put it there, remove by hand:"
    echo "      /usr/local/{include/drogon,include/trantor}"
    echo "      /usr/local/lib64/{libdrogon.a,libtrantor.a,cmake/Drogon,cmake/Trantor}"
    echo "      /usr/local/bin/drogon_ctl"
fi

echo "==> Removing build trees and state"
try rm -rf /tmp/drogon
# Build output inside the repo checkout (gitignored, but still residue).
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
try rm -rf "$SCRIPT_DIR/backend/build"
try rm -rf "$STATE_DIR"

echo ""
echo "======================================================"
echo " Uninstall complete."
echo "======================================================"
echo ""
echo " Kept on purpose — your stored files:"
echo "   $NAS_ROOT"
echo "   $BACKUP_ROOT"
echo "   (nas_storage/.nas-meta also holds events.db, alerts.db and"
echo "    sync.db. Delete these two directories by hand if you want"
echo "    the data gone too.)"
echo ""
echo " Not touched, because they are shared system components this"
echo " installer cannot prove it owns:"
echo "   - Packages from dnf (nginx, tailscale, git, cmake, python3, ...)"
echo "   - The nginx and tailscaled services themselves (only our config"
echo "     was removed; nginx was reloaded if it was running)"
echo ""
echo " Note: the installer deleted /etc/nginx/conf.d/default.conf and"
echo " cannot restore it. Fedora ships no such file by default, so this"
echo " only matters if you had created one."
echo ""
echo " Your git checkout still has the installer's path substitutions"
echo " (home_path -> $_HOME) in config.json and nginx/nas.conf."
echo " Run 'git checkout -- config.json nginx/nas.conf' to revert them."
echo ""
}

# ─────────────────────────────────────────────────────────────────────────────
#  ENTRY POINT
# ─────────────────────────────────────────────────────────────────────────────
# No flag behaves as --install, so the documented `sudo bash setup.sh` from
# earlier versions keeps working unchanged.
case "${1:---install}" in
    --install)   do_install ;;
    --uninstall) do_uninstall ;;
    --help|-h)   usage ;;
    *)
        echo "Unknown option: $1" >&2
        echo >&2
        usage >&2
        exit 1
        ;;
esac
