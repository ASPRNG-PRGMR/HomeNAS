#!/usr/bin/env bash
# setup.sh — NAS stack installer for Fedora 43
# Run as root: sudo bash setup.sh

set -euo pipefail

_USER="${SUDO_USER:-$USER}"
_HOME="$(eval echo "~$_USER")"
NAS_ROOT="$_HOME/nas/nas_storage"
INSTALL_DIR="$_HOME/nas/nas_main"
NAS_USER="$_USER"

echo "==> Installing dependencies"
dnf install -y \
    libuuid-devel\
    cmake ninja-build gcc-c++ \
    openssl-devel zlib-devel jsoncpp-devel \
    brotli-devel \
    git \
    nginx \
    tailscale

echo "==> Installing Drogon"
if [ -f /usr/local/lib/cmake/Drogon/DrogonConfig.cmake ]; then
    echo "Drogon already installed. Skipping..."
elif [ -d /tmp/drogon ] && [ "$(ls -A /tmp/drogon 2>/dev/null)" ]; then
    echo "/tmp/drogon already exists and is not empty. Skipping Drogon install..."
else
    git clone --depth=1 --recurse-submodules \
        https://github.com/drogonframework/drogon /tmp/drogon

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
mkdir -p "$INSTALL_DIR"/{webui,logs,tmp_uploads}


# TO PUT HOME PATHS IN RESPECTIVE FILES
sed -i "s|home_path|$_HOME|g"\
        backend/controllers/FilesystemController.cpp \
        backend/controllers/UploadController.cpp \
        backend/main.cpp\
	backend/config.json\
	nginx/nas.conf

echo "==> Building backend"
cmake -S backend -B backend/build \
    -DCMAKE_BUILD_TYPE=Release \
    -GNinja
cmake --build backend/build -j"$(nproc)"
cp backend/build/nas_backend "$INSTALL_DIR"
cp backend/config.json   "$INSTALL_DIR"

echo "==> Deploying Web UI"
cp -r webui/* "$INSTALL_DIR/webui/"

echo "==> Setting ownership"
chown -R "$NAS_USER":"$NAS_USER" "$NAS_ROOT"
chown -R "$NAS_USER":"$NAS_USER" "$INSTALL_DIR"

echo "==> Setting SELinux contexts"
# Allow NGINX to proxy to localhost:8080
setsebool -P httpd_can_network_relay 1
setsebool -P httpd_can_network_connect 1

# Label the webui directory so NGINX can read it
semanage fcontext -a -t httpd_sys_content_t "$INSTALL_DIR/webui(/.*)?" || \
semanage fcontext -m -t httpd_sys_content_t "$INSTALL_DIR/webui(/.*)?"
restorecon -Rv "$INSTALL_DIR/webui"

# Label nas_root so the backend can read/write it
semanage fcontext -a -t var_t "$NAS_ROOT(/.*)?" || true
restorecon -Rv "$NAS_ROOT"

echo "==> Generating self-signed TLS certificate"
mkdir -p /etc/ssl/nas
openssl req -x509 -nodes -newkey rsa:4096 -days 3650 \
    -keyout /etc/ssl/nas/key.pem \
    -out    /etc/ssl/nas/cert.pem \
    -subj   "/CN=nas.local"
chmod 600 /etc/ssl/nas/key.pem

echo "==> Installing NGINX config"
cp nginx/nas.conf /etc/nginx/conf.d/nas.conf
rm -f /etc/nginx/conf.d/default.conf
nginx -t
systemctl enable --now nginx

echo "==> Opening firewall ports"
firewall-cmd --permanent --add-service=https
firewall-cmd --permanent --add-service=http
firewall-cmd --reload

echo "==> Enabling Tailscale"
systemctl enable --now tailscaled

echo "==> Installing and running systemd service"
cp systemd/nas-backend.service /etc/systemd/system/
sed -i "s|_home_|$_HOME|g" /etc/systemd/system//nas-backend.service
sed -i "s|_user_|$_USER|g" /etc/systemd/system//nas-backend.service
chmod o+x $_HOME
chmod o+rx $_HOME/nas
chmod o+rx $_HOME/nas/nas_main
chmod -R o+rX $_HOME/nas/nas_main/webui
systemctl daemon-reload
sudo systemctl reload nginx
systemctl enable nas-backend
systemctl start nas-backend

echo ""
echo "======================================================"
echo " Done! Required manual steps:"
echo "======================================================"
echo ""
echo " 1. Edit $INSTALL_DIR/config.json"
echo "    - Set jwt_secret  → a long random string"
echo "    - Set admin_password → your chosen password"
echo ""
echo " 2. Restart the backend:"
echo "    sudo systemctl restart nas-backend"
echo ""
echo " 3. Connect to Tailscale:"
echo "    sudo tailscale up"
echo ""
echo " 4. On each client machine, add to /etc/hosts:"
echo "    <tailscale-ip>  nas.local"
echo "    (or just use the Tailscale IP directly)"
echo ""
echo " 5. Open https://nas.local in your browser."
echo "    Accept the self-signed certificate warning."
echo ""
