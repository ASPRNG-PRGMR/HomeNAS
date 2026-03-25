# HomeNAS

A lightweight self-hosted NAS (Network Attached Storage) running on an old laptop over Tailscale. Browse, upload, download, and manage files from any device through a clean web UI — no cloud, no subscriptions, no port forwarding.

---

## Features

- Remote file access over Tailscale
- Upload and download files from browser
- Folder creation, deletion, and file management
- JWT-based authentication
- NGINX reverse proxy with HTTPS
- systemd service integration for persistence
- Lightweight frontend with no framework dependency

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
# 1. Update your system
sudo dnf update && sudo dnf upgrade -y

# 2. Clone the repo
git clone https://github.com/ASPRNG-PRGMR/HomeNAS.git
cd HomeNAS

# 3. Run setup
./setup.sh

# 4. Start services
./startup.sh

# 5. Authenticate Tailscale when prompted
# Then get your Tailscale IP
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
# Run the below commands - change custom_user with whatever username you desire and custom_path with the path where you wish to move the installation
sed -i "s|/home/\${USER}|<custom_path>|g" setup.sh
sed -i "s/\${USER}/{custom_user>/g" setup.sh

# Now run the setup.sh file again, it will install the files in the desired path with the desired user
```

Then set `ProtectHome=true` in the service file and update all paths in `config.json` accordingly. This isolates the backend process so it has no visibility into any user's home directory.

Either way works — the tradeoff is convenience vs isolation.

### SELinux and the webui

Fedora's SELinux will block NGINX from serving files that aren't labeled `httpd_sys_content_t`. Files under `/home` on some Fedora setups are on a filesystem that doesn't support xattr labels, which means `chcon` will fail with "can't apply partial context to unlabeled file".

The fix is to serve the webui from `/{installation path}/nas/nas_main/webui/` instead, which is on the root filesystem and supports SELinux labels properly. The backend can still run from anywhere.

### Self-signed certificate warning

Your browser will warn about the certificate the first time. This is expected — the cert is self-signed, not issued by a CA. Since you're accessing this over Tailscale (a private encrypted network you control), this is fine for personal use. Click "Advanced" → "Accept the risk and continue" (Firefox) or "Proceed anyway" (Chrome).

If you want to get rid of the warning, use [mkcert](https://github.com/FiloSottile/mkcert) to generate a locally trusted cert instead.

### Large file transfers

The HTTP upload works but can be slow for large files. For bulk transfers, SCP over Tailscale is much faster since it goes directly over the WireGuard tunnel without touching the backend:

```bash
# From any client on your Tailscale network
scp largefile.mkv yourusername@100.x.x.x:/{installation path}/nas/nas_storage/
```

### Passwords

Passwords are currently stored as plaintext in `config.json`. This is fine for personal single-user use on a private Tailscale network, but if you extend this to multiple users, implement bcrypt hashing in `AuthController.cpp` before storing any passwords.

---

## Project structure

```
HomeNAS-main/
│
├── backend/
│   ├── controllers/
│   │   ├── AuthController.h/cpp         — login, logout, JWT generation
│   │   ├── FilesystemController.h/cpp   — list, download, delete, mkdir, rename
│   │   └── UploadController.h/cpp       — multipart file upload
│   ├── filters/  
│   │    └── JwtFilter.h/cpp              — JWT validation middleware
│   ├── CMakeLists.txt   
│   ├── main.cpp   
│   └── config.json                          — runtime configuration
│
├── webui/   
│   ├── index.html                       — page structure
│   ├── style.css                        — all styling
│   └── app.js                           — all UI logic
|
│                       
├── nginx/   
│   └── nas.conf                         — reverse proxy + TLS
│
├── systemd
│   └── nas-backend.service              — service definition
│
└── README.md   
```

---
