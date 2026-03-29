sudo systemctl stop nas-backend
cd /home/noobiegg/Documents/nas/backend/build
sudo ninja
sudo cp /home/noobiegg/Documents/nas/backend/build/nas_backend /home/noobiegg/nas/nas_main/nas_backend
sudo systemctl start nas-backend
