#!/usr/bin/env bash
# setup.sh — NAS stack installer for Fedora 43
# Run as root: sudo bash setup.sh

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

    cmake -S /tmp/drogon -B /tmp/drogon/build \
        -DCMAKE_BUILD_TYPE=Release \
        -GNinja \
        -DBUILD_TESTING=OFF

    cmake --build /tmp/drogon/build -j"$(nproc)"
    cmake --install /tmp/drogon/build
    ldconfig
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
sudo systemctl reload nginx
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
