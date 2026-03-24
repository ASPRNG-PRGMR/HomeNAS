# HomeNAS

A lightweight self-hosted NAS (Network Attached Storage) running on an old laptop over Tailscale. Browse, upload, download, and manage files from any device through a clean web UI — no cloud, no subscriptions, no port forwarding.

---

## What's inside

- **Drogon (C++)** — high performance backend handling auth, file listing, uploads and downloads
- **NGINX** — reverse proxy, serves the web UI, handles HTTPS
- **Tailscale** — encrypted WireGuard tunnel so you can access the NAS from anywhere without opening router ports
- **JWT auth** — stateless token-based login, no database needed
- **Vanilla JS frontend** — no build step, no framework, just HTML/CSS/JS

---

## Deployment

### Requirements

- Fedora 38+ (tested on Fedora 43)
- `sudo` access
- Internet connection (to install packages and authenticate Tailscale)

### Steps

```bash
# 1. Clone the repo
git clone https://github.com/yourname/nas-stack
cd nas-stack

# 2. Edit config before anything else
nano nas_main/config.json
# Set: jwt_secret, admin_username, admin_password, nas_root

# 3. Build the backend
cd nas_main/build
cmake .. -GNinja
sudo ninja

# 4. Create required directories
mkdir -p ~/nas/nas_main/logs
mkdir -p ~/nas/nas_main/tmp_uploads
mkdir -p ~/nas/nas_storage

# 5. Install NGINX config
sudo cp nginx/nas.conf /etc/nginx/conf.d/nas.conf

# 6. Copy webui to a SELinux-friendly location
sudo mkdir -p /opt/nas/webui
sudo cp -r nas_main/webui/* /opt/nas/webui/
sudo chcon -R -t httpd_sys_content_t /opt/nas/webui/

# 7. Generate TLS cert
sudo mkdir -p /etc/ssl/nas
sudo openssl req -x509 -nodes -newkey rsa:4096 -days 3650 \
    -keyout /etc/ssl/nas/key.pem \
    -out /etc/ssl/nas/cert.pem \
    -subj "/CN=nas.local"

# 8. Open firewall ports
sudo firewall-cmd --permanent --add-service=https
sudo firewall-cmd --permanent --add-service=http
sudo firewall-cmd --reload

# 9. SELinux — allow NGINX to proxy to backend
sudo setsebool -P httpd_can_network_relay 1
sudo setsebool -P httpd_can_network_connect 1

# 10. Start NGINX
sudo systemctl enable --now nginx

# 11. Install and start the backend service
sudo cp systemd/nas-backend.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now nas-backend

# 12. Connect Tailscale
sudo systemctl enable --now tailscaled
sudo tailscale up
# Follow the URL it prints to authenticate

# 13. Get your Tailscale IP
tailscale ip -4
```

Open `https://<tailscale-ip>` in any browser on your Tailscale network. Accept the self-signed certificate warning and log in.

### Useful commands

```bash
# Reload NGINX after config changes
# nginx -t tests the config for errors first, then reload applies it
sudo nginx -t && sudo systemctl reload nginx

# Backend logs
sudo journalctl -u nas-backend -f

# Restart backend
sudo systemctl restart nas-backend

# SELinux denials (if something isn't loading)
sudo ausearch -m avc -ts recent | audit2why
```

---

## Notes

### ProtectHome and running from your home directory

The systemd service file includes `ProtectHome=false`. This is intentional — by default systemd's `ProtectHome=true` makes `/home` invisible to the service process, which would prevent it from accessing anything under `~`.

**If you are running this for personal use on your own machine with everything under `~/nas/`, leave it as `ProtectHome=false`.** It actually makes things easier — you can browse your NAS storage directly in your file manager, edit configs without sudo, and check logs without switching users.

**If you are deploying this more seriously** (shared machine, multiple users, or exposing to more people than just yourself), the better practice is to move everything out of your home directory:

```bash
sudo mkdir -p /opt/nas/{webui,backend/logs,backend/tmp_uploads}
sudo mkdir -p /data/nas
sudo useradd -r -s /sbin/nologin -d /opt/nas nas
sudo chown -R nas:nas /opt/nas /data/nas
```

Then set `ProtectHome=true` in the service file and update all paths in `config.json` accordingly. This isolates the backend process so it has no visibility into any user's home directory.

Either way works — the tradeoff is convenience vs isolation.

### SELinux and the webui

Fedora's SELinux will block NGINX from serving files that aren't labeled `httpd_sys_content_t`. Files under `/home` on some Fedora setups are on a filesystem that doesn't support xattr labels, which means `chcon` will fail with "can't apply partial context to unlabeled file".

The fix is to serve the webui from `/opt/nas/webui/` instead, which is on the root filesystem and supports SELinux labels properly. The backend can still run from anywhere.

### Self-signed certificate warning

Your browser will warn about the certificate the first time. This is expected — the cert is self-signed, not issued by a CA. Since you're accessing this over Tailscale (a private encrypted network you control), this is fine for personal use. Click "Advanced" → "Accept the risk and continue" (Firefox) or "Proceed anyway" (Chrome).

If you want to get rid of the warning, use [mkcert](https://github.com/FiloSottile/mkcert) to generate a locally trusted cert instead.

### Large file transfers

The HTTP upload works but can be slow for large files. For bulk transfers, SCP over Tailscale is much faster since it goes directly over the WireGuard tunnel without touching the backend:

```bash
# From any client on your Tailscale network
scp largefile.mkv yourusername@100.x.x.x:/home/noobiegg/nas/nas_storage/
```

### Passwords

Passwords are currently stored as plaintext in `config.json`. This is fine for personal single-user use on a private Tailscale network, but if you extend this to multiple users, implement bcrypt hashing in `AuthController.cpp` before storing any passwords.

---

## Project structure

```
nas-stack/
├── nas_main/
│   ├── backend/
│   │   ├── controllers/
│   │   │   ├── AuthController.h/cpp      — login, logout, JWT generation
│   │   │   ├── FilesystemController.h/cpp — list, download, delete, mkdir, rename
│   │   │   └── UploadController.h/cpp    — multipart file upload
│   │   └── filters/
│   │       └── JwtFilter.h/cpp           — JWT validation middleware
│   ├── webui/
│   │   ├── index.html                    — page structure
│   │   ├── style.css                     — all styling
│   │   └── app.js                        — all UI logic
│   ├── CMakeLists.txt
│   ├── main.cpp
│   └── config.json                       — runtime configuration
├── nginx/
│   └── nas.conf                          — reverse proxy + TLS
├── systemd/
│   └── nas-backend.service               — service definition
└── README.md
```

---

## License

MIT
