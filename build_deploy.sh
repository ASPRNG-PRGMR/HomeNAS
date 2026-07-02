#!/usr/bin/env bash
# build_deploy.sh — rebuild backend and redeploy webui in one shot
# Run as root: sudo bash build_deploy.sh

set -euo pipefail

_USER="${SUDO_USER:-$USER}"
_HOME="$(eval echo "~$_USER")"
INSTALL_DIR="$_HOME/nas/nas_main"

echo "==> Stopping nas-backend"
systemctl stop nas-backend

echo "==> Building backend"
cmake -S backend -B backend/build \
    -DCMAKE_BUILD_TYPE=Release \
    -GNinja
cmake --build backend/build -j"$(nproc)"

echo "==> Deploying backend binary"
if [ -f "$INSTALL_DIR/nas_backend" ]; then
    echo "    Removing old binary..."
    rm -f "$INSTALL_DIR/nas_backend"
fi
cp backend/build/nas_backend "$INSTALL_DIR/nas_backend"
chown "$_USER":"$_USER" "$INSTALL_DIR/nas_backend"

echo "==> Deploying Web UI"
cp -r webui/* "$INSTALL_DIR/webui/"
chown -R "$_USER":"$_USER" "$INSTALL_DIR/webui"
cp -r sync-portal/* "$INSTALL_DIR/sync-portal/"
chown -R "$_USER":"$_USER" "$INSTALL_DIR/sync-portal"
chmod -R o+rX "$INSTALL_DIR/sync-portal"

echo "==> Starting nas-backend"
systemctl start nas-backend

echo "==> Waiting for backend to come up..."
sleep 1
if systemctl is-active --quiet nas-backend; then
    echo "    nas-backend is running."
else
    echo "    nas-backend failed to start. Check logs:"
    echo "    sudo journalctl -u nas-backend -n 40 --no-pager"
    exit 1
fi

echo "==> Opening nas.local"
sudo -u "$_USER" \
    DISPLAY=:0 \
    DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$_USER")/bus" \
    xdg-open "https://nas.local" 2>/dev/null || \
sudo -u "$_USER" \
    DISPLAY=:0 \
    DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$_USER")/bus" \
    firefox "https://nas.local" 2>/dev/null || \
    echo "    Could not launch browser automatically. Open https://nas.local manually."

echo ""
echo "Done."
