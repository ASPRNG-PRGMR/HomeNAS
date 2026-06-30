# HomeNAS

A lightweight self-hosted NAS running on an old laptop over Tailscale, with a built-in security platform that records every filesystem action, detects attacks in real time, and gives you a browser-based SOC console to investigate and respond.

No cloud. No subscription. No port forwarding. No blind spots.

---

## The problem this solves

Most self-hosted NAS solutions give you file access. What they don't give you is any answer to:

- *Did someone try to brute-force my login last night?*
- *Why are 200 files missing — did I delete them, or did something else?*
- *What exactly happened in the 10 minutes before that directory disappeared?*

Commercial NAS devices (Synology, QNAP) have audit logs, but they're locked behind paywalls, opaque, and you can't query them directly. Open-source alternatives like TrueNAS or Nextcloud have activity logs, but no real-time detection, no alert layer, and no investigation console.

HomeNAS is built on an old laptop with commodity software, but it records a tamper-evident append-only log of every action, runs detection rules against that log in real time, fires structured alerts on brute-force and mass-delete patterns, and surfaces everything through a browser dashboard you can actually use to investigate an incident.

The USP is that it's not just a NAS with logging bolted on — the security platform is first-class, built into the same process, with zero extra services to run.

---

## What it is

- **NAS:** browse, upload, download, rename, move, and drag-to-move files and folders from any browser on your Tailscale network
- **Folder upload:** upload entire directory trees from the browser — via picker or drag-and-drop — with directory structure preserved
- **Audit log:** every auth event and filesystem action recorded to a local SQLite database, append-only, never mutated
- **Real-time detection:** 17 rules covering brute-force login (BF-001–006), mass-delete (MD-001–006), path traversal (PT-001–002), and data exfiltration (DX-001–003), running within 250ms of each action
- **SOC dashboard:** Overview, Alert management, Event explorer, and investigation timelines — all in the same browser tab as the file manager

---

## Stack

| Component | Role |
|---|---|
| **Drogon (C++)** | Backend — file API, auth, event recording, detection engine |
| **SQLite (WAL mode)** | `events.db` (append-only) and `alerts.db` — embedded, no server |
| **NGINX** | Reverse proxy, TLS termination, static webui serving |
| **Tailscale** | Encrypted WireGuard tunnel — remote access without open ports |
| **Vanilla JS** | Frontend — no framework, no build step, no dependencies |
| **systemd** | Service supervision and auto-restart |

---

## Project structure

```
HomeNAS/
│
├── backend/
│   ├── controllers/
│   │   ├── AuthController.h/cpp        — login, logout, JWT generation/validation
│   │   ├── FilesystemController.h/cpp  — list, download, delete, mkdir, rename/move
│   │   ├── UploadController.h/cpp      — multipart file upload
│   │   ├── EventsController.h/cpp      — GET /api/events (audit log query API)
│   │   ├── AlertsController.h/cpp      — GET /api/alerts, PATCH /api/alerts/:id/status
│   │   └── SyncController.h/cpp        — GET /api/sync/status, /api/sync/logs, POST start/pause/resume
│   ├── filters/
│   │   └── JwtFilter.h/cpp             — JWT validation middleware
│   ├── services/
│   │   ├── EventTypes.h                — NasEvent struct and enums
│   │   ├── EventRecorder.h/cpp         — non-blocking emit() called from controllers
│   │   ├── EventWriter.h/cpp           — background thread, batched SQLite writes
│   │   ├── AlertTypes.h                — NasAlert struct, severity, rule ID constants
│   │   ├── AlertWriter.h/cpp           — alert persistence and DB migration management
│   │   ├── EventAnalyzer.h/cpp         — 17 detection rules, post-flush analysis
│   │   ├── SyncTypes.h                 — SyncState enum, SyncStatus, SyncLogEntry
│   │   └── SyncManager.h/cpp           — background hashing + port rotation + stopgap listener
│   ├── db/
│   │   ├── schema_events.sql           — events table reference schema
│   │   └── schema_alerts.sql           — alerts table and migration reference
│   ├── CMakeLists.txt
│   ├── main.cpp
│   └── config.json
│
├── webui/
│   ├── index.html
│   ├── style.css
│   └── app.js
│
├── sync-portal/                        — standalone sync landing page (preview/stopgap — see README "Storage sync")
│   ├── index.html
│   ├── style.css
│   └── app.js
│
├── nginx/
│   └── nas.conf
│
├── systemd/
│   └── nas-backend.service
│
├── build_deploy.sh                     — local build, deploy, and browser-launch script
└── setup.sh
```

---

## How the security platform works

### Event collection

Every action through HomeNAS is recorded. Controllers call `EventRecorder::emit()` at the point they know the outcome. The call is non-blocking — the event is timestamped and pushed to an in-memory queue, returning immediately so file I/O latency is unaffected.

A dedicated background thread (`EventWriter`) drains the queue in batches of up to 200 events per transaction, flushing at least every 250ms. A single writer thread means no SQLite write contention. Events are never deleted or modified.

Events recorded: `auth.login.success`, `auth.login.failure`, `auth.logout`, `file.upload`, `file.download`, `file.delete`, `file.rename`, `file.move`, `dir.create`, `dir.delete`.

### Detection

After each batch flush, `EventAnalyzer::analyze()` runs on the same background thread. It executes sliding-window COUNT queries, fires alerts when rules trigger, and deduplicates using per-rule in-memory cooldown timers.

**Brute force rules (IP-keyed):**

| Rule | Condition | Severity |
|---|---|---|
| BF-001 | ≥5 login failures / same IP / 60s | Low |
| BF-002 | ≥10 login failures / same IP / 60s | Medium |
| BF-003 | ≥20 login failures / same IP / 60s | High |
| BF-004 | ≥50 failures / 5min OR ≥100 / 15min | Critical |
| BF-005 | ≥10 failures then success / same IP / 10min | High |
| BF-006 | Same username targeted from ≥3 distinct IPs / 5min | Medium |

BF-005 is treated as the most dangerous scenario — the attacker may have successfully gained access. BF-006 detects distributed password spraying using `claimed_user` (unverified username from the request body), kept deliberately separate from `actor_user` (verified identity on success only).

**Mass delete rules (actor-keyed):**

| Rule | Condition | Severity |
|---|---|---|
| MD-001 | ≥20 deletes / same actor / 60s | Low |
| MD-002 | ≥50 deletes / same actor / 60s | Medium |
| MD-003 | ≥100 deletes / same actor / 5min | High |
| MD-004 | ≥500 deletes / same actor / 10min | Critical |
| MD-005 | Login followed by ≥50 deletes / 5min | High |
| MD-006 | Any directory deletion | Medium |

Directory deletes count toward all rules — deleting one top-level folder containing 500 files is equivalent to deleting 500 files individually for detection purposes.

**Path traversal rules (IP-keyed):**

| Rule | Condition | Severity |
|---|---|---|
| PT-001 | ≥5 path traversal attempts / same IP / 60s | Medium |
| PT-002 | ≥20 path traversal attempts / same IP / 5min | High |

Triggered when a request attempts to access a path outside the storage root (e.g. `../../etc/passwd`). One or two occurrences may be a misconfigured client; a burst indicates active probing.

**Data exfiltration rules (actor-keyed):**

| Rule | Condition | Severity |
|---|---|---|
| DX-001 | ≥50 downloads / same actor / 5min | Medium |
| DX-002 | ≥200 downloads / same actor / 10min | High |
| DX-003 | Login followed by ≥30 downloads / 5min | High |

Keyed on the authenticated actor rather than source IP, so an attacker rotating exit nodes mid-session doesn't evade detection.

### Dashboard

Four tabs, always accessible from the same browser session as the file manager:

- **Overview** — severity cards (open alert counts), events today, auth failure count, recent alerts, live activity feed, top source IPs and actors with proportional failure bars. Severity cards are clickable — click Critical to jump straight to the filtered alert list. The Alerts tab pulses with a glow matching the highest open severity: red for critical, orange for high, yellow for medium, green for low, faint green when clear.
- **Alerts** — full alert list with severity/rule/status/date filters, inline expand showing evidence JSON, status action buttons (Mark Investigating / Dismiss / Reopen), and a ±10 minute investigation timeline showing all related events before and after the alert fired.
- **Rules** — read-only reference view of every detection rule (BF, MD, PT, DX) grouped by family, with condition, time window, and severity for each — a quick way to check what triggers an alert without reading source.
- **Events** — full event log with type/result/user/date filters, inline expand showing every field including target path, secondary path, bytes transferred, and failure reason.
- **Files** — the NAS file manager. Drag the `⠿` handle on any row to move it into another folder in the current directory, or drag it onto a breadcrumb segment in the path pill to move it up to a parent directory. Moves are recorded as `file.move` events and trigger mass-delete detection rules the same as any other deletion-class action.

The dashboard polls every 15 seconds. The alert badge on the Alerts tab shows the live open alert count.

### Storage sync ⚠️ engine not yet built — scaffolding only

> **This feature is scaffolding, not a finished sync engine.** The icon, status API, and portal are real and wired against a real background thread, but the actual file-sync engine does not exist yet — progress/ETA are simulated, and the stopgap portal listener is scheduled for deletion. The sync architecture has been decided (see below and the devlog Phase 3.2 entry) and the real engine is next on the implementation list.

A sync status icon sits bottom-left, above the sign-out button. It reflects one of five states — idle, syncing, paused, error, or hash mismatch — shown as colour-coded fade animations matching the Alerts tab's severity glow language (idle fades green, paused fades amber, error fades red on a 2.5s cycle, hash-mismatch fades orange on a faster sharper cycle, syncing spins in accent blue). Hovering shows live status and last-verified hash age.

**Hash verification** is real: `SyncManager` periodically walks `nas_storage` and computes an aggregate integrity hash. If the hash changes between checks while no sync was running, the state flips to hash-mismatch — the one state the backend can enter on its own, without a sync engine reporting anything.

**What's mocked:** the sync engine itself. Progress/ETA are simulated by `SyncManager` advancing a percentage on a timer.

**What sync will actually do (decided in Phase 3.2):** one-way backup of `nas_storage` to a configured local path — typically a second drive. New and changed files are copied. Deletions in `nas_storage` are not automatically mirrored to the backup (`delete_from_backup: false` by default) — the backup is a safety net, not a mirror. Each run's outcome is persisted to `sync.db`. The sync portal moves from a separately-bound listener to NGINX-served static content under `/sync/` on the main domain, inheriting TLS and JWT auth. The stopgap `python3 -m http.server` listener and the daily port-rotation machinery are deleted in the next phase.

### Data storage

```
~/nas/nas_storage/.nas-meta/
├── events.db    — append-only event log (never deleted)
└── alerts.db    — alert log (status mutable, records never deleted)
```

Both databases are hidden from the NAS file browser. Back them up separately if the audit trail matters independently of the file data.

---

## REST API

All endpoints require `Authorization: Bearer <token>` except `/api/auth/login`.

### Auth
| Method | Endpoint | Description |
|---|---|---|
| POST | `/api/auth/login` | Returns JWT |
| POST | `/api/auth/logout` | Records logout event |

### Filesystem
| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/ls?path=` | List directory |
| GET | `/api/file?path=` | Download file |
| DELETE | `/api/file?path=` | Delete file or directory |
| POST | `/api/mkdir` | Create directory |
| POST | `/api/rename` | Rename or move |
| POST | `/api/upload?path=` | Upload files (multipart) |

### Events
| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/events` | Paginated list. Filters: `type`, `user`, `result`, `from`, `to`, `limit`, `offset` |
| GET | `/api/events/:id` | Single event |
| GET | `/api/events/export` | CSV export. Filters: `from`, `to` |
| GET | `/api/events/stats/summary` | Counts by type/result. Add `group_by=actor` or `group_by=source_ip` for top-N rankings |

### Alerts
| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/alerts` | Paginated list. Filters: `severity`, `rule`, `status`, `from`, `to` |
| GET | `/api/alerts/:id` | Single alert with full evidence JSON |
| GET | `/api/alerts/stats/summary` | Open counts by severity |
| PATCH | `/api/alerts/:id/status` | Set status: `open`, `investigating`, or `dismissed` |

### Sync (preview — see "Storage sync" above)
| Method | Endpoint | Description |
|---|---|---|
| GET | `/api/sync/status` | State, percent/ETA (mocked), last verified hash, current rotating port |
| GET | `/api/sync/logs?limit=` | Recent sync log entries (in-memory, not persisted to SQLite) |
| POST | `/api/sync/start` | Mock: begins a simulated sync run |
| POST | `/api/sync/pause` | Mock: pauses an in-progress simulated sync |
| POST | `/api/sync/resume` | Mock: resumes a paused simulated sync |

---

## Deployment

### Requirements

- Fedora 38+ (tested on Fedora 43)
- `sudo` access
- Internet connection

### Fresh install

```bash
git clone https://github.com/ASPRNG-PRGMR/HomeNAS.git
cd HomeNAS
sudo bash setup.sh
```

`setup.sh` installs all dependencies (including `sqlite-devel`), builds Drogon from source if not present, compiles the backend, deploys the web UI, configures NGINX, generates a self-signed TLS certificate, installs the systemd service, and starts everything.

### Required manual steps after setup

```bash
# Generate a real secret
openssl rand -hex 32

# Edit the deployed config
nano ~/nas/nas_main/config.json
```

Set `jwt_secret` to the generated value and `admin_password` to your chosen password. Then:

```bash
sudo systemctl restart nas-backend
sudo tailscale up

# On the NAS machine itself
echo "127.0.0.1 nas.local" | sudo tee -a /etc/hosts

# On each remote client
echo "<tailscale-ip> nas.local" | sudo tee -a /etc/hosts
```

Open `https://nas.local`. Accept the self-signed certificate warning.

### Iterative development (build and deploy)

After making changes to the backend, use `build_deploy.sh` to build, deploy, and relaunch in one step:

```bash
sudo bash build_deploy.sh
```

This stops the service, rebuilds with ninja in Release mode, replaces the deployed binary, restarts the service, and opens `nas.local` in Firefox. Web UI changes (`webui/`) take effect immediately after replacing the files in the NGINX serve directory — no rebuild needed.

### Useful commands

```bash
# Live backend logs
sudo journalctl -u nas-backend -f

# Reload NGINX after config changes
sudo nginx -t && sudo systemctl reload nginx

# Query the event log directly
sqlite3 ~/nas/nas_storage/.nas-meta/events.db \
  "SELECT id, timestamp_utc, event_type, actor_user, result \
   FROM nas_events ORDER BY id DESC LIMIT 20;"

# Query the alert log directly
sqlite3 ~/nas/nas_storage/.nas-meta/alerts.db \
  "SELECT id, timestamp_utc, rule_id, severity, title, status \
   FROM nas_alerts ORDER BY id DESC LIMIT 20;"

# SELinux denials
sudo ausearch -m avc -ts recent | audit2why
```

---

## Configuration reference

`~/nas/nas_main/config.json` after deployment:

```json
{
  "listeners": [{ "address": "127.0.0.1", "port": 8080, "https": false }],
  "app": {
    "threads_num": 4,
    "log_path": "~/nas/nas_main/logs",
    "upload_path": "~/nas/nas_main/tmp_uploads"
  },
  "custom_config": {
    "nas_root":           "~/nas/nas_storage",
    "jwt_secret":         "CHANGE THIS — use openssl rand -hex 32",
    "jwt_expiry_seconds": 86400,
    "admin_username":     "admin",
    "admin_password":     "CHANGE THIS",
    "events_db_path":     "~/nas/nas_storage/.nas-meta/events.db",
    "alerts_db_path":     "~/nas/nas_storage/.nas-meta/alerts.db",
    "trusted_ips": [
      "127.0.0.1",
      "::1"
    ],
    "sync": {
      "backup_root":        "/mnt/backup_drive/nas_backup",
      "schedule_cron":      "0 3 * * *",
      "delete_from_backup": false,
      "telegram_bot_token": "",
      "telegram_chat_id":   ""
    }
  }
}
```

`trusted_ips` — IPs exempt from all IP-keyed detection rules (BF and PT families). `127.0.0.1` and `::1` are always exempt regardless of this list. Add your Tailscale IP (`tailscale ip -4`) to avoid triggering brute-force alerts on your own login attempts.

`sync.backup_root` — local path to back up to. Must be writable by the `nas-backend` service user. Typically a second drive mounted under `/mnt/`.

`sync.schedule_cron` — when to run automatic backups. `0 3 * * *` = 3am daily.

`sync.delete_from_backup` — if `false` (default), files deleted from `nas_storage` are never removed from `backup_root`. Set `true` only if you want a strict mirror rather than a safety-net backup.

`sync.telegram_bot_token` / `sync.telegram_chat_id` — leave empty to disable notifications. To enable: create a bot via @BotFather, get your chat ID via @userinfobot, fill both fields and restart.

---

## Notes

### SELinux on Fedora

SELinux can block the backend from traversing `/home` even with correct Unix permissions. Check denials with `sudo ausearch -m avc -ts recent | audit2why`. For personal use, the simplest fix is `sudo setenforce 0` (temporary) or setting `SELINUX=disabled` in `/etc/selinux/config` and rebooting. For hardened deployments, move storage to `/srv` or `/opt` and configure proper SELinux contexts.

### ProtectHome

The systemd service uses `ProtectHome=false` — intentional for personal use where everything lives under `~/nas/`. For shared deployments, move everything out of `/home` and set `ProtectHome=true`.

### Self-signed certificate

Accept the browser warning on first visit. To eliminate it, use [mkcert](https://github.com/FiloSottile/mkcert) to issue a locally trusted certificate.

### Large file transfers

For bulk transfers SCP is faster than HTTP upload:
```bash
scp largefile.mkv user@<tailscale-ip>:~/nas/nas_storage/
```

### Folder upload — browser support

Folder upload via picker (`webkitdirectory`) and drag-and-drop directory traversal (`webkitGetAsEntry`) are supported on all modern desktop browsers (Chrome, Firefox, Safari, Edge). iOS Safari does not support `webkitdirectory` — the folder picker option will be non-functional on mobile. Flat file upload and drag-and-drop of individual files are unaffected.

### Trusted IPs and self-triggering alerts

Detection rules BF-001 through BF-006 and PT-001/002 are IP-keyed. If you mistype your password from localhost or your Tailscale IP, you will trigger BF alerts against yourself. Add those IPs to `trusted_ips` in `config.json` and restart to exempt them permanently.

### JWT and session management

JWTs are stateless — logout is client-side only. To invalidate all active sessions immediately, rotate `jwt_secret` and restart the backend. Passwords are stored as plaintext in `config.json`, acceptable for personal single-user use on a private Tailscale network.

---

## Roadmap

- **Storage Sync — real engine (next):** architecture decided in Phase 3.2 — one-way backup of `nas_storage` to a configured local path. Deletes the stopgap listener and port rotation, adds `sync.db` for run persistence, wires Telegram notifications, moves the portal to NGINX. See devlog Phase 3.2 for full decision record.
- **Phase 2.55 — SOAR:** Manual response actions from the dashboard: block IP, invalidate sessions, disable user account.
- **Phase 2.56 — Notifications:** Telegram alerts on high/critical severity triggers and sync completion/failure. Bot token and chat ID configurable in `config.json`.
- **Phase 3 — Storage Intelligence:** Duplicate detection, stale file identification, storage usage trends.
