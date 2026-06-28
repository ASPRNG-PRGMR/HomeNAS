sudo systemctl stop nas-backend
INSTALL_DIR="/home/$SUDO_USER/nas/nas_main"

echo "==> Building backend"
cmake -S backend -B backend/build \
    -DCMAKE_BUILD_TYPE=Release \
    -GNinja
cmake --build backend/build -j"$(nproc)"

if [ -f "$INSTALL_DIR/nas_backend" ]; then
      echo "nas_backend found in nas_main - removing it..."
      rm -f "$INSTALL_DIR/nas_backend"
fi
sudo cp backend/build/nas_backend $INSTALL_DIR/nas_backend

sudo systemctl start nas-backend
sudo -u "$SUDO_USER" DISPLAY=:0 DBUS_SESSION_BUS_ADDRESS="unix:path=/run/user/$(id -u "$SUDO_USER")/bus" firefox nas.local &

