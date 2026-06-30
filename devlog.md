# HomeNAS — Development Log

A chronological record of architectural decisions, bugs caught, tradeoffs made, and implementation notes. Written as a technical reference for future development, not a tutorial.

---

## Pre-Phase 1 — Codebase Audit

Before designing the event collection layer, a full read of the existing source was done to establish ground truth.

**Architecture confirmed:**
- Drogon (C++) backend, controller-based: `AuthController`, `FilesystemController`, `UploadController`
- `JwtFilter` as Drogon middleware on all protected routes
- JWT is stateless — `validateJwt()` re-derives the HMAC signature rather than trusting the header's `alg` claim, so no `alg: none` attack surface
- No database of any kind — config from `config.json`, no persistence layer
- NGINX terminates TLS and reverse-proxies `/api/*` to Drogon on `127.0.0.1:8080`

**Security issues found in existing code (pre-Phase 1):**

1. **Critical: plaintext password logging.** `AuthController.cpp` logged both the attempted password and the real config password to `journalctl` on every login attempt via `LOG_INFO`. Anyone with log access could read the admin password. Fixed immediately as part of Phase 1 (two lines deleted).

2. **No rate limiting on `/api/auth/login`.** Nothing prevents brute-force attempts. Noted as a future detection target (addressed in Phase 2 via `EventAnalyzer`), not auto-blocked (per design decision: Phase 2 is detection-only).

3. **Non-constant-time password comparison.** `username != cfgUser || password != cfgPass` is a timing side-channel in theory. Low severity at this scale, noted but not fixed.

---

## Phase 1 — Event Collection Layer

### Design decisions

**SQLite over Postgres/MySQL:** Zero extra service, zero extra systemd unit, trivial backup (one file). For 1–3 users this is strictly better. Would revisit if multi-node or high-concurrency use emerged.

**WAL mode mandatory:** `PRAGMA journal_mode=WAL` enables concurrent reads (EventsController queries) without blocking the single writer (EventWriter), and vice versa. Non-negotiable.

**Single writer thread:** `EventWriter` owns the only write connection to `events.db`. This avoids SQLite multi-thread handle-sharing complexity entirely, rather than relying on `SQLITE_OPEN_FULLMUTEX`. Producers push onto a thread-safe queue and return immediately.

**Batched writes:** One `BEGIN; INSERT×N; COMMIT;` per flush rather than one transaction per event. SQLite's per-transaction fsync cost dominates at small batch sizes — batching is what makes "log every action" affordable without affecting upload/download latency.

**Queue bound:** In-memory queue capped at 50,000 events. Drop-with-counter rather than unbounded growth.

**What's logged vs. not logged:**
- `list` (GET `/api/ls`) — deliberately NOT logged. Happens constantly from normal browsing; zero security value per event, would dominate volume.
- JwtFilter success (valid token on every request) — NOT logged for the same reason.
- Path-traversal attempts on download/upload — logged as `failure` with `failure_reason: path_traversal_or_forbidden`.
- `file.rename` vs `file.move` — distinguished by comparing parent directories of `from` and `to` paths. Same parent = rename; different parent = move. Heuristic, not API-enforced.

**EventsController route ordering:** Literal paths (`/api/events/export`, `/api/events/stats/summary`) registered before the parameterized `/{id}` route — otherwise `export` or `stats` could be matched as the `id` parameter depending on Drogon's route resolution order.

### C++17 compatibility bug (caught before first build)

The initial implementation used C++20 designated initializers:
```cpp
EventRecorder::emit({
    .type = EventType::FileDelete,
    .result = EventResult::Success,
    ...
});
```

`CMakeLists.txt` targets `CMAKE_CXX_STANDARD 17`. This would have been a compile failure. Fixed by adding a fluent builder pattern to `NasEvent`:
```cpp
EventRecorder::emit(NasEvent(EventType::FileDelete, EventResult::Success)
    .withActor(actor)
    .withSourceIp(req->getPeerAddr().toIp())
    .withTargetPath(rel));
```
The builder is C++17-compatible, matches the readability of designated initializers, and is strictly typed.

### Signal handling bug (caught before first run)

First draft of `main.cpp` used `std::signal(SIGTERM, handler)` / `std::signal(SIGINT, handler)`. Research confirmed Drogon installs its own signal handlers internally — a raw `std::signal()` call **replaces** Drogon's handler entirely rather than supplementing it, silently breaking Drogon's own graceful shutdown. Fixed by using `app.setTermSignalHandler()` / `app.setIntSignalHandler()`, the documented extension points for this use case.

### `EventWriter::shutdown()` threading note

`shutdown()` calls `workerThread_.join()` — a blocking call — inside the signal handler callback that Drogon invokes. Safe because `EventWriter`'s background thread never calls back into Drogon (only touches its own mutex/queue and SQLite), so there's no deadlock risk. Noted in code comments.

### Build issues encountered during deployment

**`find_package(SQLite3 REQUIRED)` conflict with Drogon:**
Drogon ships its own `cmake_modules/FindSQLite3.cmake` placed on `CMAKE_MODULE_PATH` ahead of CMake's built-in module when `find_package(Drogon REQUIRED)` runs. Drogon's internal resolution also calls `find_package(SQLite3)` internally, creating an imported target `SQLite3_lib`. A second explicit `find_package(SQLite3 REQUIRED)` re-runs the same module and tries to redefine `SQLite3_lib`, causing:
```
add_library cannot create imported target "SQLite3_lib" because another target with the same name already exists.
```
Fix: remove the explicit `find_package(SQLite3 REQUIRED)` entirely. Drogon's own resolution already populates `SQLITE3_INCLUDE_DIRS` and `SQLITE3_LIBRARIES`. Use those variables directly.

**`SQLite::SQLite3` imported target unavailable:**
An intermediate attempt used `target_link_libraries(... SQLite::SQLite3)`. Drogon's custom `FindSQLite3.cmake` doesn't create a modern imported target — only plain variables. Fixed by using `${SQLITE3_LIBRARIES}`.

**`cmake` pointed at wrong source directory:**
`CMakeLists.txt` was placed at the project root, but source files live in `backend/`. Fix: place `CMakeLists.txt` inside `backend/` and run `cmake ../backend`.

**`sqlite-devel` missing from `setup.sh` dependencies:**
Added to the `dnf install` list.

### Deployment path convention

All runtime paths use a `home_path` token in source files, replaced by `setup.sh` via `sed -i "s|home_path|$_HOME|g"` before compilation. Every file containing a `home_path` reference must be in `setup.sh`'s sed list — failures are silent path mismatches at runtime, not compile errors.

Files requiring `home_path` substitution (Phase 1):
- `backend/main.cpp`
- `backend/config.json`
- `backend/controllers/FilesystemController.cpp`
- `backend/controllers/UploadController.cpp`
- `backend/controllers/EventsController.cpp`
- `nginx/nas.conf`

### End-to-end verification results

Full event taxonomy verified against live stack (systemd + NGINX + TLS + Drogon):

```
1|auth.login.failure||failure       — actor_user NULL (unverified claim), correct
2|auth.login.success|admin|success  — actor_user set post-verification, correct
4|dir.create|test_folder|success
5|file.upload|test_folder/testfile.txt|success|bytes=10
6|file.rename|test_folder/testfile.txt → test_folder/renamed.txt|success
10|dir.create|test_folder2|success
11|file.move|test_folder/renamed.txt → test_folder2/renamed.txt|success
12|file.delete|test_folder2/renamed.txt|success
```

rename vs. move heuristic confirmed working. `bytes_transferred = 10` on upload of `"hello nas\n"` — exact match.

---

## Phase 2 — Real-time Security Detection

### Design decisions

**Where `EventAnalyzer` hooks in:** Called from `EventWriter::flushBatch()` immediately after `COMMIT`. Runs on the existing background thread — no new thread needed. Rule queries are cheap COUNT/GROUP BY operations at this data volume.

**Single-threshold-per-pass deduplication:** Each rule has a configurable cooldown period. `shouldFire(ruleId, key, cooldownSeconds)` checks an in-memory `lastFired_` map. The map is in-memory (not persisted) — a backend restart resets cooldowns, which is acceptable.

**`AlertWriter` vs routing through `EventWriter`:** `AlertWriter` is a separate singleton with its own SQLite connection to `alerts.db`. Writing alerts into `events.db` was rejected (conflates data types with different mutation semantics). Routing through `EventWriter`'s queue was rejected (adds polymorphism for no gain).

**Alerts are not deletable:** Only `PATCH /api/alerts/:id/status` is exposed for mutation. Alerts are permanent evidence — deliberately non-negotiable.

**`claimed_user` vs `actor_user`:** BF-006 (password spraying) requires knowing the targeted username across multiple IPs. Failed login events store no `actor_user` (unverified identity). Solution: a separate `claimed_user` column populated only on `auth.login.failure` from the raw request body. Schema enforces the distinction: `actor_user` = verified, `claimed_user` = unverified claim.

**Schema migration strategy:** Adding `claimed_user` to the already-deployed `events.db` used a `_migration_guard` table. `AlertWriter::init()` checks the guard before running `ALTER TABLE`, marks it done on success, and also marks it done on a "duplicate column" error. Idempotent across restarts.

**MD-006 (directory delete) design:** Every `dir.delete` fires MD-006 unconditionally, no cooldown. Rationale: each directory deletion is independently notable. The dashboard correlates MD-006 alerts with preceding events to show what was in the directory before deletion — more useful than trying to count files at deletion time (which would require a filesystem walk during the detection path).

### Rules not yet implemented (deferred)

- **IP blocking / session invalidation** — Phase 2 is detection-only. SOAR deferred to Phase 2.55.
- **Anomalous access hours** — requires establishing a per-user baseline over time.

### Files added/modified in Phase 2

**New:** `services/AlertTypes.h`, `services/AlertWriter.{h,cpp}`, `services/EventAnalyzer.{h,cpp}`, `controllers/AlertsController.{h,cpp}`, `db/schema_alerts.sql`

**Modified:** `services/EventTypes.h` (added `claimedUser`), `services/EventWriter.cpp` (claimed_user in INSERT, added analyze() call), `controllers/AuthController.cpp` (populate claimedUser on failure), `main.cpp` (AlertWriter/EventAnalyzer wiring), `config.json` (alerts_db_path), `CMakeLists.txt` (4 new source files), `setup.sh` (AlertsController.cpp in sed list)

### `.nas-meta` visibility fix

The `.nas-meta` directory was visible in the web UI's file browser. Fixed in `FilesystemController.cpp`'s `list()` handler:
```cpp
if (entry.path().filename() == ".nas-meta")
    continue;
```

---

## Phase 2 — group_by addition to EventsController

**Motivation:** The Overview dashboard needed "top source IPs" and "top actors" data. `GET /api/events/stats/summary` only returned counts by event_type × result. Client-side approximation from a capped page of events would be inaccurate.

**Solution:** Added an optional `group_by` parameter to the existing summary endpoint. `group_by=actor` or `group_by=source_ip` runs a different `GROUP BY` query and returns a ranked list with total count and failure count per key. The default (no parameter) behaviour is unchanged.

**Security note:** The column name inserted into SQL is never derived from raw user input. A strict whitelist check (`groupBy == "actor" || groupBy == "source_ip"`) maps to hard-coded column name strings via a ternary. Any other value falls through to the default branch.

**Response shape:**
```json
{
  "group_by": "source_ip",
  "items": [
    { "source_ip": "192.168.1.55", "total": 73, "failures": 73 },
    { "source_ip": "100.64.0.1",   "total": 389, "failures": 1 }
  ]
}
```

A source IP with `total == failures` is immediately distinguishable from a legitimate user at a glance.

---

## Phase 2.5 — Security Dashboard

### Design goals

Transform the web UI from a file manager with security APIs into a usable SOC console. The file manager must continue to work exactly as before — the dashboard is additive.

### UI architecture decisions

**Navigation model:** A persistent top nav bar (tabs) added above the existing file manager header. Four tabs: Files, Overview, Alerts, Events. The Files tab wraps the existing app unchanged. The three new tabs render into separate `tab-pane` divs using the same `.hidden` toggle pattern already in the codebase.

**No framework, no build step:** Consistent with the existing webui. The entire dashboard is vanilla JS functions and closures — same style as the original `app.js`. No classes, no state management library, no compilation required.

**Inline expand over full-page detail:** Alert and Event rows expand in-place when clicked, keeping the list visible for context. An expanded row is a `<tr class="expand-row">` inserted immediately after the clicked row via `row.after(expandRow)`. Only one expand is open at a time — clicking the same row again or clicking another row closes the current one.

**Polling over SSE/WebSocket:** 15 second interval for the alert badge count and Overview panel refresh. `clearInterval` on sign-out prevents orphaned timers. At 1–3 users on a personal NAS, 2 requests per 15 seconds is negligible load. SSE or WebSocket would require backend changes for zero meaningful gain at this scale.

**Promise.all for Overview:** All six Overview API calls are fired in parallel and awaited together. Sequential calls would add ~300ms of unnecessary latency on each tab switch.

### Timeline implementation

The Alert detail inline expand includes a ±10 minute investigation timeline. No new backend endpoint was needed — the frontend:

1. Parses `evidence` JSON from the alert record to extract `source_ip`
2. Builds a `from`/`to` ISO 8601 window of ±10 minutes around `alert.timestamp_utc`
3. Fires up to two parallel `GET /api/events` queries (one filtered by IP, one by actor) using `Promise.all`
4. Merges results, deduplicates by `id`, sorts ascending
5. Renders a CSS timeline with the alert marker inserted at its chronological position

The alert marker is placed by comparing each event's `timestamp_utc` against `alert.timestamp_utc` during the render loop — inserted before the first event that is >= the alert timestamp, or appended at the end if all events preceded it.

### group_by in Overview cards

The "Top Source IPs" and "Top Actors" overview cards call `GET /api/events/stats/summary?group_by=source_ip` and `?group_by=actor` respectively, scoped to today via the `from` parameter. Results render as ranked rows with a proportional bar chart. The bar turns red (`danger` class) when `total === failures` — indicating an IP or actor whose entire event history is failures (typical of a brute-force source).

### Performance decisions

- Default page size: 50 alerts/events. Max: 500 (cap enforced in backend).
- No infinite scroll — explicit prev/next pagination only. Simpler, no intersection observer, no scroll position management.
- Evidence JSON is parsed with `JSON.parse()` and re-serialized with `JSON.stringify(parsed, null, 2)` for pretty-printing. Never rendered with raw `innerHTML` from the API response — all values pass through `escHtml()`.
- Timeline queries fire after the expand panel is already rendered (loading state shown), so the panel appears immediately while timeline data loads in the background.

### Responsive breakpoints

- 768px: stat cards go 2×2, overview grid collapses to single column, expand panels go single column, secondary table columns hidden (`col-hide-tablet`).
- 480px: filter inputs reduce font size.
- Target: desktop and tablet. Not optimised for phones — consistent with the existing webui's implicit target.

### Validation performed

- File manager tab: upload, download, rename, delete, mkdir — all confirmed unaffected.
- Alert badge: fires correct count from `/api/alerts/stats/summary`.
- Overview: severity cards, events today, auth failures, deletes, recent alerts, live feed, top IPs, top actors — all render from real data.
- Alert list filters: severity, rule, status, date range — all confirmed filtering correctly.
- Alert inline expand: evidence JSON renders, status actions fire PATCH correctly, table refreshes after status update.
- Timeline: events appear in correct chronological order with alert marker at right position.
- Event list filters and inline expand: all fields render including null values.
- Pagination: prev/next buttons appear/disappear correctly at page boundaries.

### Known limitations

- Timeline deduplication merges by `id` only — if the same event appears from both IP and actor queries (which it will for most alerts since both filters are applied), it's correctly deduplicated. But if a user has no `actor_user` set (e.g. pre-auth failures), only the IP query fires, which is correct behaviour.
- The alert badge count is updated by polling, not pushed. A burst of alerts between poll intervals won't increment the badge until the next 15s tick.
- `setup.sh`'s sed list for `home_path` substitution was updated to include `AlertsController.cpp` — this was the only new file added in Phase 2 that contained a `home_path` fallback default.

### Files added/modified in Phase 2.5

**Modified (webui only — no backend rebuild required):**
- `webui/index.html` — added `#top-nav`, `#auth-shell` wrapper, three `tab-pane` divs, filter controls, table shells, pagination containers. Existing file manager elements moved inside `#tab-files` unchanged.
- `webui/style.css` — all original rules preserved. Dashboard styles added below a clear separator comment: severity/status/result pills, stat cards, overview grid, dash-card, mini-row, offender bars, data table, expand panel, timeline, pagination, polling dot animation, responsive breakpoints.
- `webui/app.js` — original NAS logic preserved exactly. Dashboard logic added below a separator. New: `switchTab()`, `startPolling()`/`stopPolling()`, `loadOverview()`, `loadAlerts()`, `loadEvents()`, alert/event row rendering, inline expand logic, `renderAlertExpand()`, `fetchAlertTimeline()`, `renderTimeline()`, `updateAlertStatus()`, `renderPagination()`, helper functions `relTime()`, `fmtUtc()`, `sevPill()`, `statusPill()`, `resultPill()`.

---

## Open items / future phases

### Phase 2.55 — Basic SOAR

Manual response actions from the dashboard:
- Block IP — write to `blocked_ips` table; `JwtFilter` / `AuthController` check before processing requests
- Invalidate sessions — rotate `jwt_secret` programmatically (all active JWTs immediately invalid)
- Disable user account — add `disabled_users` table; `AuthController` checks before credential verification
- Dismiss / mark investigated — already implemented via `PATCH /api/alerts/:id/status`

Requires backend changes: `blocked_ips` and `disabled_users` tables, new check in `JwtFilter`, new SOAR action endpoints.

### Phase 2.56 — Notifications

Alert delivery via email on high/critical severity triggers. SMTP configuration in `config.json`. No autonomous action — human-in-the-loop only.

### Phase 3 — Storage Intelligence

Duplicate detection, stale file identification, storage usage trends. Separate from the event stream; requires periodic filesystem walks and content hashing. Different architectural character from Phases 1–2.

### Known technical debt

- **Plaintext passwords in `config.json`:** Acceptable for personal single-user use. Multi-user deployment requires bcrypt in `AuthController.cpp`.
- **Single admin user:** Multi-user support requires a user table and per-user credentials.
- **JWT has no revocation:** Rotation of `jwt_secret` is the only server-side invalidation mechanism.
- **`_migration_guard` is in `alerts.db` but migrations apply to `events.db`:** Acceptable for now; a unified meta DB would be cleaner if migrations become more frequent.
- **`nas.conf` uses `home_path` token:** Correctly handled by `setup.sh`'s `sed` substitution. Must be substituted manually if edited post-install.
- **MD-006 fires on every directory delete with no cooldown:** A legitimate bulk directory reorganisation during normal use will flood the alert table with MD-006 rows. Acceptable at personal NAS scale; would need a cooldown or a "trusted operation" exemption mechanism before multi-user deployment.

---

## Phase 2.6 — Web UI Refactor + Drag-to-Move

### Context

The web UI worked correctly but had two usability problems that only became apparent after deploying to a real laptop screen at 170% browser zoom:

1. At high zoom levels the nav bar left-pinned tab pill and right-pinned Sign Out button occupied the far edges of the viewport, leaving the centre wordmark isolated in a sea of dead space. The file list and dashboard content were also left-anchored rather than centred.
2. There was no way to move files or folders between directories from the browser — the only options were download, rename, and delete. Moving required SSH or SCP.

No backend changes in this phase. All changes are `webui/` only.

---

### Nav bar centring refactor

**Problem:** `#top-nav` used `position: fixed` with `#nav-tabs-left` at `position: absolute; left: 14px` and `#nav-right` at `position: absolute; right: 14px`. At 170% zoom this spread them to the literal screen edges. Adding a `max-width` constraint to `#top-nav` itself doesn't work because the nav spans the full viewport width.

**Solution:** Introduced `#nav-inner` — a `max-width: 1100px; margin: 0 auto` flex container wrapping all three nav children. `#nav-tabs-left` and `#nav-right` are now flex siblings (no longer absolutely positioned). `#nav-centre` keeps `position: absolute; left: 50%; transform: translateX(-50%)` but relative to `#nav-inner` rather than the viewport, so the wordmark is always geometrically centred between the two flanking elements regardless of zoom.

`main#file-list` and `.dash-screen` both gained `margin: 0 auto; width: 100%` so content columns centre themselves within the viewport at any zoom level.

**Mobile:** at ≤768px `#nav-inner` switches to `flex-direction: column` — wordmark on top, tab pill below it as a single 4-column row. Sign Out moves to `position: absolute; right: 14px` within the column stack. At ≤520px the wordmark hides and the layout collapses to a single row.

---

### Path bar relocation

**Problem:** The directory traversal pill (`⌂ ← / dirname`) lived inside `#nav-centre`, stacked directly below the wordmark. This made it visually part of the title bar rather than part of the file browser.

**Decision:** Moved `#path-bar` out of `#nav-centre` entirely and placed it as the first child of `#tab-files`, directly above `#file-list`. `#nav-centre` now contains only the wordmark — isolated, clearly a branding element.

The path bar is no longer sticky; it scrolls with the file list. It uses `margin: 16px auto 0; width: fit-content` so it centres itself under the wordmark with visible breathing room. The pill style (dark surface, rounded border, monospace crumbs) is unchanged from the original design.

`syncPathBar()` in the inline script was updated to observe the new DOM position. No functional change to the home/back button logic.

---

### Gap reduction

`tab-pane` `margin-top` reduced from `calc(var(--nav-h) + 16px)` to `calc(var(--nav-h) + 4px)`. `main#file-list` `padding-top` reduced from `56px` to `24px`. The old values were compensating for space that the path bar previously occupied inside the nav — now that it's in the file list flow these offsets were unnecessary dead space.

---

### Drag-to-move

**Motivation:** Moving a file required rename modal → type the full destination path manually, or SCP. Neither is acceptable for a browser-based file manager.

**API surface used:** `POST /api/rename` with `{ from, to }`. No new endpoint. `FilesystemController::rename` already distinguished rename vs move by comparing parent directories and emitting `FileMove` vs `FileRename` accordingly — this feature gets that audit trail for free.

**Drag handle:** A `⠿` (braille pattern dots-123456) element with class `drag-handle` appended inside `.file-actions` on every row. At rest it's `color: var(--border)` (invisible but occupying space, so layout doesn't shift). It becomes visible (`var(--muted)`) on row hover and fully bright on direct hover. `cursor: grab` / `grabbing` on active. The handle sits in the existing 80px actions column — no grid template change needed.

**Handle-gated dragging:** `draggable="true"` is set on every `.file-row` at render time, but `dragstart` is suppressed unless `_dragViaHandle` is true. This flag is set by a `mousedown` listener on `#file-list` that checks `e.target.closest('.drag-handle')`. This prevents accidental drags when clicking a row name or icon — only grabbing the handle initiates a drag.

**Drop targets — folders in the list:** Only rows with `data-dir="true"` receive `dragenter/dragover/dragleave/drop` handlers. `dragleave` checks `e.relatedTarget` to avoid flickering when the cursor crosses child elements within the row. On drop: `post('/api/rename', { from: '/' + srcPath, to: '/' + destDirPath + '/' + srcName })` followed by `navigate(currentPath)`.

**Drop targets — path bar crumbs:** Moving items *out* of the current directory required a separate drop surface. The path pill crumbs (`/ dirname`) and the `⌂` home button are now drop targets, wired in `syncPathBar()` via a shared `attachCrumbDrop(el, destPath)` helper. On drop the same `_nasMove(src, dest)` function fires. The crumb highlights with an accent glow (`box-shadow: 0 0 0 1.5px var(--accent)`) when a dragged item hovers over it.

**Cross-script state sharing:** `_dragSourcePath` and `_dragSourceName` are module-level variables in `app.js` exposed on `window` via `Object.defineProperties`. `window._nasMove` is also exposed. This is necessary because `syncPathBar()` lives in an inline `<script>` in `index.html` and cannot import from `app.js` directly.

**Visual states:**
- `.file-row.dragging` — 35% opacity, `pointer-events: none` while in flight
- `.file-row.drop-target` — dashed accent outline, blue-tinted background, name turns accent colour
- `.crumb.crumb-drop-target` / `#path-home.crumb-drop-target` — accent glow + blue tint

**What drag-to-move does not cover:** Dragging items from the OS file manager into the NAS browser — that already worked via the existing `dragenter/drop` listener on `#auth-shell` which triggers `uploadFiles()`. Drag-to-move is purely internal (NAS item → NAS folder).

---

### Files modified in Phase 2.6

`webui/index.html` — added `#nav-inner` wrapper around nav children; moved `#path-bar` from `#nav-centre` into `#tab-files`; updated `syncPathBar()` to call `attachCrumbDrop()` on each crumb; added `attachCrumbDrop()` helper function.

`webui/style.css` — `#nav-inner` centring container; removed absolute positioning from `#nav-tabs-left` and `#nav-right`; `#nav-centre` simplified to wordmark-only; `#path-bar` repositioned with `margin: 16px auto 0; width: fit-content`; reduced `tab-pane` margin-top and `main#file-list` padding-top; added `.drag-handle`, `.file-row.dragging`, `.file-row.drop-target`, `.crumb-drop-target`, `#path-home.crumb-drop-target` rules; mobile breakpoints updated for new nav column layout.

`webui/app.js` — `renderFileList` adds `draggable="true"` and `<div class="drag-handle">` to each row; `attachDragHandlers()` wires all drag events; `_dragViaHandle` flag gates drag initiation to handle-only; `_dragSourcePath`/`_dragSourceName` exposed on `window`; `window._nasMove` exposed for inline script access.

---

## Phase 2.7 — UI Redesign + Folder Upload

### Context

Two separate problems addressed in this phase:

1. The nav layout, while centred, had inconsistent visual language — the tab pill, wordmark, and sign-out button had different weights and border styles, making the nav feel like three unrelated elements rather than a coherent system.
2. There was no way to upload an entire folder from the browser — only flat file uploads were supported.

No backend changes in this phase. All changes are `webui/` only, plus `build_deploy.sh`.

---

### Nav redesign — bilateral symmetry

**Problem:** A single 4-button pill on the left holding all four tabs, with sign-out as a bordered pill on the right. The left had significantly more visual weight than the right, making HOME NAS feel off-centre despite being geometrically centred.

**Solution:** Split the four tabs into two equal pills — `[Files | Overview]` on the left, `[Alerts | Events]` on the right — flanking the wordmark symmetrically. Both pills are identical containers: same `background`, same `border: 1.5px solid #2a2e40`, same `border-radius: 12px`, same padding. HOME NAS sits on `position: absolute; left: 50%; transform: translateX(-50%)` inside `#nav-inner`, so it is mathematically centred between the two pills regardless of their content width.

Sign-out moved out of the nav bar entirely. It is now a circular FAB (`position: fixed; bottom: 28px; left: 28px`) mirroring the upload FAB at `bottom: 28px; right: 28px`. This creates a second axis of symmetry — the two bottom-corner circles balance each other the same way the two nav pills balance each other. At rest the sign-out FAB is muted with a visible border; on hover it turns red with a faint glow, making the destructive action visually distinct without being aggressive at rest.

**CSS:** `#nav-tabs-left` and `#nav-tabs-right` share the `.nav-pill` class. `#logout-btn.corner-fab` shares the same 46px circle geometry and tooltip pattern as `.fab`.

---

### Alert glow — per-severity colour

**Problem:** Previous implementation used three glow classes (`glow-red`, `glow-yellow`, `glow-green`), collapsing critical and high into the same red, losing the distinction.

**Fix:** Five classes: `glow-critical` (#e05252 red), `glow-high` (#e07d2a orange), `glow-medium` (#c9bb3a yellow), `glow-low` (#4caf7d green), `glow-none` (faint green). The fetch interceptor on `/api/alerts/stats/summary` now picks the highest open severity and assigns the exact matching class. If the highest open severity is HIGH, the alerts tab pulses orange — not red.

---

### Folder upload

**Mechanism:** Two upload paths, selected from a small menu that appears above the upload FAB on click:

- **Files** — opens the normal `<input type="file" multiple>` picker. Flat upload to `currentPath`, same behaviour as before.
- **Folder** — opens `<input type="file" webkitdirectory multiple>`. The browser presents a directory picker and returns all files within it with `webkitRelativePath` set.

**Upload logic (`uploadFlatList`):**

1. Collect all unique intermediate directory paths from `webkitRelativePath` values, sorted shallowest-first.
2. Call `POST /api/mkdir` on each. `FilesystemController::mkdir` uses `fs::create_directories` which is idempotent — no error if the directory already exists.
3. Upload each file individually to its target subdirectory via `POST /api/upload?path=<fileDir>`, with a running counter in the progress bar (`Uploading 3 / 47: report.pdf`).

**Drag-and-drop folder support:** The existing `drop` handler on `#auth-shell` is extended to check for `DataTransferItem.webkitGetAsEntry()`. If any dropped item is a directory entry, the handler recursively walks the tree using `FileSystemDirectoryReader.readEntries()` (called in a loop until it returns an empty batch — the API silently caps at 100 entries per call). The resolved flat file list, with synthetic `webkitRelativePath` values set via `Object.defineProperty`, is passed to `uploadFlatList`. Flat file drops retain the original `uploadFiles` path.

This means: dragging a folder from the OS file manager onto the browser window uploads the entire tree with directory structure preserved. No picker interaction required.

**Mobile:** `webkitdirectory` is not supported on iOS Safari. The folder menu option will either show an empty picker or be rejected by the OS. This is a browser limitation — not regressed from the previous release where folder upload was not supported at all. Flat file upload via picker and drag-and-drop file upload are unaffected on mobile.

---

### `build_deploy.sh`

Added to the repository root. Automates the local build-and-deploy loop:

1. Stops `nas-backend` via systemd.
2. Runs `cmake` + `ninja` on `backend/` in Release mode.
3. Replaces the deployed binary at `~/nas/nas_main/nas_backend`.
4. Restarts `nas-backend`.
5. Opens `nas.local` in Firefox under the current user's display session.

Requires `sudo` — the systemd stop/start and binary copy need elevated privileges. The `$SUDO_USER` variable is used throughout to avoid writing files owned by root into the user's home directory.

---

### Files modified in Phase 2.7

`webui/index.html` — split `#nav-tabs-left` into `#nav-tabs-left` + `#nav-tabs-right` with shared `.nav-pill` class; removed `#nav-right` wrapper; added `#logout-btn.corner-fab` as a fixed bottom-left element; added `#upload-menu` with Files/Folder options above `#fab-upload`; added `<input id="folder-input" webkitdirectory multiple>`; updated FAB wiring in inline script.

`webui/style.css` — added `.nav-pill` shared container style; replaced single `#nav-tabs-left` grid layout with two flex pills; removed `#nav-right` / old `#logout-btn` rules; added `#logout-btn.corner-fab` with hover-red glow; added five `glow-*` keyframe animations replacing previous three; added `#upload-menu` pill styles.

`webui/app.js` — replaced `glow-red/yellow/green` with `glow-critical/high/medium/low/none` in fetch interceptor; replaced `drop` handler with `webkitGetAsEntry` branch; added `readEntries()` recursive directory walker; added `uploadFlatList()` for structure-preserving uploads; added `folder-input` change handler calling `uploadFlatList`; `uploadFiles` retained unchanged for flat file uploads.

`build_deploy.sh` — new file. Build, deploy, and browser-launch script for local development iteration.

---

## Phase 2.8 — Path Traversal / Data Exfiltration Rules + Rules Tab

### Context

PT-001/002 and DX-001/002/003 were specified in design docs and partially wired (header declarations, dispatch calls in `EventAnalyzer::analyze()`, evidence-building conventions matching BF/MD) but the rule bodies and two supporting query helpers were never written into `EventAnalyzer.cpp`. This shipped a linker error rather than a runtime bug — `ninja` failed at the link step with `undefined reference to EventAnalyzer::checkPT001_002(...)` and similarly for the two DX methods, since the symbols were declared and called but never defined.

A second, smaller gap surfaced on the next build attempt: `RuleId::PT001` etc. were referenced in the new rule bodies but never added to the `RuleId` namespace in `AlertTypes.h` — only BF and MD constants existed there.

### Rule implementations added

**`checkPT001_002(ip)`** — IP-keyed, mirrors the BF threshold-table pattern exactly: an array of `{ruleId, severity, threshold, windowSecs, title, cooldownSecs}` rows, iterated in escalating order, fires the highest applicable rule only, then breaks. PT-001 (5 failures/60s, MEDIUM) and PT-002 (20 failures/5min, HIGH).

**`checkDX001_002(actor)`** — actor-keyed, same table-driven pattern. Reuses the existing generic `countEvents("file.download", nullptr, actor, windowSecs)` helper rather than adding a redundant query — `file.download` + `result = 'success'` was already a supported `countEvents` call shape. DX-001 (50/5min, MEDIUM), DX-002 (200/10min, HIGH).

**`checkDX003(actor, loginTs)`** — directly mirrors `checkMD005`'s "login followed by burst" shape: a single threshold (30 downloads within 5 minutes of `loginTs`), one cooldown, one alert. Matches MD-005's HIGH severity and 5-minute window precedent rather than inventing a new convention.

### New query helpers

**`countTraversalFailures(ip, windowSecs)`** — `SELECT COUNT(*) ... WHERE result='failure' AND failure_reason='path_traversal_or_forbidden' AND source_ip=? AND timestamp_utc >= datetime('now','-N seconds')`. This is the first analyzer query to filter on `failure_reason` rather than just `event_type`/`result` — the column already existed (used by Phase 1's path-traversal logging) but no detection rule had read it back out until now.

**`countDownloadsAfterLogin(actor, loginTs, windowSecs)`** — structurally identical to `countDeletesAfterLogin`, swapping `event_type = 'file.delete'` for `event_type = 'file.download'` and `result = 'success'`. Window is computed forward from `loginTs` via SQLite `datetime(?, '+N seconds')`, same pattern as MD-005.

### `AlertTypes.h` — RuleId constants

Five `constexpr const char*` entries added to the existing `RuleId` namespace, immediately after `MD006`, matching the `"XX-NNN"` string format already used by every BF/MD constant: `PT001`, `PT002`, `DX001`, `DX002`, `DX003`.

### Rules tab — dashboard documentation view

A fifth nav tab (`#tab-rules`, gear icon) added between Alerts and Logs. Read-only — no API calls, no filters, no state. Renders the full rule catalogue (BF, MD, PT, DX) as four `.rules-group` blocks, each a header (title + one-line description of what the rule family is keyed on) followed by a `.rules-table` of `Rule | Condition | Window | Severity` rows using the same `sev-pill` component already used in the Alerts and Overview tabs, so severity colour coding is visually consistent across the whole dashboard.

**Why static HTML instead of generating it from `RuleId` + thresholds at runtime:** the rule descriptions ("possible ransomware wipe", "rotating exit nodes does not evade detection") are human-authored context that doesn't exist as backend data — there's no `/api/rules` endpoint and adding one purely to avoid duplicating six lines of HTML per rule was judged not worth a new backend surface for a page that changes only when rules themselves change.

**Mobile:** `.rules-row` collapses from a 4-column grid (`72px 1fr 90px 80px`) to a 2-column grid at ≤600px, hiding the Window column and left-aligning the severity pill onto its own line under the description — same collapse strategy as `.data-table`'s `col-hide-tablet` class elsewhere in the dashboard, applied via a dedicated breakpoint since `.rules-row` isn't a `<table>`.

### Files added/modified in Phase 2.8

**Modified (backend — rebuild required):**
- `backend/services/EventAnalyzer.cpp` — added `checkPT001_002`, `checkDX001_002`, `checkDX003`, `countTraversalFailures`, `countDownloadsAfterLogin`. No changes to existing BF/MD code.
- `backend/services/AlertTypes.h` — added `PT001`, `PT002`, `DX001`, `DX002`, `DX003` to the `RuleId` namespace.

**Modified (webui only — no backend rebuild required):**
- `webui/index.html` — added `#tab-rules` pane with four `.rules-group` blocks (BF, MD, PT, DX); added `Rules` nav tab button between Alerts and Logs; `af-rule` filter dropdown on the Alerts tab still only lists BF/MD options — **known gap, see below.**
- `webui/style.css` — added `.rules-intro`, `.rules-group`, `.rules-group-header/-title/-sub`, `.rules-table`, `.rules-row` (incl. `.rules-header` variant), `.rule-id`, `.rule-desc`, `.rule-window`, and a `≤600px` collapse breakpoint.

### Known gap carried forward

The `af-rule` `<select>` on the Alerts tab filter bar was not updated to include `PT-001`/`PT-002`/`DX-001`/`DX-002`/`DX-003` as options. PT/DX alerts will still appear in the unfiltered alert list and are fully clickable/expandable — they just can't be isolated via the rule dropdown filter yet. Low priority since severity filtering already covers the common triage case (e.g. filter to High to see PT-002/DX-002/DX-003 together).

---

## Phase 3 — Storage Sync Scaffolding

### Context

Design called for a sync feature with a deliberately unusual security model, specified before any sync engine existed to drive it: a status icon on the main dashboard, and a separate landing page served on a port that rotates daily, reachable only via a freshly-issued link from the authenticated main dashboard rather than its own login. Goal: build everything around that model — icon, polling, API contract, standalone page — against a real background thread doing real work (hashing, port rotation), with mocked sync progress, so the actual sync engine can be dropped in later without re-architecting the UI or API.

### Design decisions

**Port ownership inverted:** initial framing had the sync service telling HomeNAS its port. Since no sync service exists, this was inverted — HomeNAS (`SyncManager::maybeRotatePort()`) generates and owns the port; a future sync engine would read it from HomeNAS rather than the reverse. Removes an entire IPC mechanism that had no real consumer yet.

**Port persists rather than read-once-then-delete:** a consumed-once value would mean only the first reader gets a working link. Persisting it for the full rotation window means `/api/sync/status` can be polled freely and always returns a working link.

**No auth on the portal itself:** per design, the rotating port plus a freshly-issued link from the authenticated dashboard *is* the access boundary, not a second login.

**Hash-check is a periodic background thread, not folded into `EventAnalyzer`:** `EventAnalyzer` reacts to newly-written events; the sync hash-check needs to run on a wall-clock timer regardless of event activity, since an attacker modifying files directly on disk would never produce an event for `EventAnalyzer` to react to.

**`SyncManager` owns its own thread** (EventWriter-style), rather than riding on `EventWriter`'s thread the way `EventAnalyzer` does — sync hashing has no natural trigger event to hang off of.

**Hash algorithm is an aggregate signal, not cryptographic:** path+size+mtime combined via `hash_combine`, not full content hashing — reading every file's bytes on every check would make the integrity check itself a meaningful I/O cost on a NAS. `.nas-meta` is excluded from the walk since `events.db`/`alerts.db` mutate on every action (including the hash check's own log write), which would otherwise make the hash non-deterministic for reasons unrelated to user content.

**Error vs. hash-mismatch are visually distinct, not a shared "red and pulsing":** hash-mismatch is the one state `SyncManager` can enter unilaterally (storage drift, no sync running) and was treated as more concerning than a generic sync failure, so it got a different colour and a faster, sharper animation rather than the same red fade as a generic error.

**Mock sync progress and in-memory (non-SQLite) logs:** `startMockSync()`/`advanceMockSync()` simulate a run advancing per tick, purely so the UI has realistic data ahead of a real engine. Sync logs (`std::deque<SyncLogEntry>`) are operational/diagnostic rather than security evidence, so they don't get the same persistence guarantee as `events.db`/`alerts.db`.

### Locking discipline bug caught before shipping

Early draft of `SyncManager::log()` assumed callers already held `stateMutex_`. Several call sites held the lock via `std::lock_guard` across the call to `log()`, which also tried to lock the same non-recursive mutex — would have deadlocked the sync thread on its first hash check. Caught during self-review before building. Fixed by making `log()` self-locking and restructuring call sites to compute state-change booleans under the lock, release it, then log afterward.

### Files added/modified in Phase 3

**New (backend):** `SyncTypes.h`, `SyncManager.h/cpp`, `SyncController.h/cpp`.
**Modified (backend):** `main.cpp` (init/shutdown wiring), `config.json` (new `sync` block).
**Modified (webui):** `index.html` (`#sync-btn`, `#sync-logs-panel`), `style.css` (state animations, panel styles), `app.js` (polling, click routing, logs panel).
**New (standalone):** `sync-portal/index.html`, `style.css`, `app.js`.

### Known gaps at end of Phase 3

- No real sync engine — everything is scaffolding around mocked progress.
- `SyncController.cpp`/`SyncManager.cpp` needed manual addition to `CMakeLists.txt` (not editable directly during this phase).
- **Nothing was actually listening on the rotating port** — clicking the sync icon during idle/syncing/paused opened a link to nowhere. This became the subject of Phase 3.1 below.

---

## Phase 3.1 — Debugging session: sync button connectivity, tooltip formatting, icon animation rework

### Bug 1 — "the sync button does nothing"

Root-caused through a long chain of false leads before landing on the real issue, worth recording for future debugging hygiene:

1. First hypothesis: stale webui deploy (user had local edits, pasted assistant-generated files over them — files were in fact correctly merged, ruled out).
2. Second hypothesis: `setup.sh` not deploying webui correctly — ruled out, `cp -r webui/* "$INSTALL_DIR/webui/"` was present and correct, and grep confirmed the sync code was actually present in deployed files.
3. Third hypothesis: JS error silently swallowing the click — code reading showed `api()`'s `resp.json()` would throw on a non-JSON 404 body, and `.catch(() => null)` at the call site would swallow that silently, producing exactly "does nothing" with no console error. This was *a* real bug class but not *the* bug, since later testing showed valid JSON responses.
4. Direct `curl` against `/api/sync/status` returned a raw Drogon 404 page — not a `JwtFilter`-issued 401 (confirmed by reading `JwtFilter.cpp`, which only ever returns 401, never 404 on auth failure). This proved the route wasn't registered with Drogon's router at all, not an auth problem.
5. Checked `CMakeLists.txt` — `SyncController.cpp`/`SyncManager.cpp` were present in `add_executable()`'s direct source list (not behind an intermediate static lib that could get dead-stripped), ruling out the link-time-discard theory.
6. Checked `main.cpp` — `#include "controllers/SyncController.h"` present, `SyncManager::instance().init()` called correctly.
7. User restarted `nas-backend` fresh and re-tested: `/api/sync/status` now returned live data (`"Idle — hash verified 33s ago"` rendered correctly in the icon tooltip), confirming the earlier 404s were from a stale running binary predating the most recent rebuild — not a code defect at all. The investigation chain above was thorough but the actual fix was "restart the service after rebuilding," which had already nominally happened once but apparently not effectively until this point.
8. With the API confirmed live, clicking the icon correctly opened `https://nas.local:<port>/` in a new tab — but nothing was listening there, producing "Unable to connect." This was expected and previously flagged: `SyncManager` only ever generated and reported a port number, nothing bound a listener to it.

**Fix — stopgap portal listener:** rather than leave the portal permanently unreachable until a real sync engine exists, `SyncManager` now forks a detached `python3 -m http.server` process bound to whichever port it just rotated to, serving `sync-portal/`, and kills/respawns it on every rotation (`startPortalListener()`/`stopPortalListener()`). This is explicitly framed in code comments, the README, and here as a temporary hack to be deleted once a real sync engine owns its own port and listener — not a production sync mechanism.

Three supporting changes were required to make the stopgap actually survive in the deployed environment, each found by working backward from what could plausibly break a forked child process inside a hardened systemd service:
- `setup.sh` — added `python3` as a dependency (the stopgap execs it); opened the `49152-65535` dynamic port range in the firewall (the rotation range), since `firewall-cmd --add-service=https/http` only opens 443/80.
- `nas-backend.service` — added `KillMode=process`. Without it, the unit's default `control-group` KillMode would SIGKILL the forked listener alongside the main backend PID on every `systemctl restart nas-backend`, leaving the portal dead until the next 24h rotation regardless of whether the code itself worked. Also added `ReadOnlyPaths` for `sync-portal/`, since `ProtectSystem=strict` only allowlisted `nas_storage`/`logs`/`tmp_uploads` as accessible — the forked listener needs to read `sync-portal/`'s static files.

**Second connectivity bug, same symptom different cause:** after the listener was confirmed running (`ps aux | grep http.server` showed it, and the browser got `SSL_ERROR_RX_RECORD_TOO_LONG` rather than a connection refusal), the remaining issue was a protocol mismatch. The sync icon's click handler built the portal URL by reusing `location.protocol` from the main dashboard, which is `https:` (NGINX terminates TLS). The stopgap listener (`python3 -m http.server`) only ever speaks plain HTTP. Browser sent a TLS ClientHello at a plaintext server — `SSL_ERROR_RX_RECORD_TOO_LONG` is exactly what that looks like from the browser's side, not an "unreachable" or "refused" error. Fixed by hardcoding `http://` for the portal link specifically, with an inline comment explaining this is tied to the stopgap and should be revisited if/when a real sync engine terminates its own TLS.

### Bug 2 — tooltip text wrapping oddly

`#sync-btn.corner-fab::before` had `white-space: nowrap` immediately followed, two declarations later in the same block, by `white-space: normal` plus `max-width: 220px` and `text-align: center` — the later declaration always wins in CSS, so the `nowrap` was dead code and every tooltip was forced into a narrow, centered, word-wrapped column regardless of length. This was a copy-paste leftover from drafting (the centered-multi-line variant was tried and then abandoned in favor of matching `#logout-btn`'s existing single-line tooltip style, but the abandoned rules were never deleted). Fixed by removing the contradictory `max-width`/`white-space: normal`/`text-align: center` trio entirely, leaving a clean single-line `nowrap` tooltip consistent with every other FAB tooltip in the dashboard.

### Feature request folded into this session — consistent state animations

Originally the five sync icon states used an inconsistent mix of treatments (static dim for paused, slow fade for error, fast pulse-and-scale for hash-mismatch, plain colour+spin for syncing, plain colour for idle) — functional but visually unrelated to anything else in the dashboard. Per request, rewrote all five states to use the same fade-glow language already established by the Alerts tab's `glow-critical/-high/-medium/-low/-none` keyframes (fade between muted and a severity colour with a matching box-shadow glow, same 2.5s base cycle), so the sync icon and the Alerts nav tab read as the same kind of status indicator:

- **idle** → fades to `--sev-low` (green), mirrors `glow-none`'s "all clear" treatment.
- **syncing** → fades to `--accent` (blue, not a severity colour, since "actively syncing" isn't a problem state) *and* spins — fading alone doesn't convey "in progress" the way literal motion does, so the spin was kept as an additional cue layered on top of the fade.
- **paused** → fades to `--sev-medium` (amber/yellow).
- **error** → fades to `--sev-critical` (red) on the same 2.5s cycle as Alerts' `glow-critical`.
- **hash-mismatch** → fades to `--sev-high` (orange) but on a faster 1.4s cycle with an added scale pulse, intentionally different rhythm from error (not just colour) so the two failure states are distinguishable at a glance without reading the tooltip — preserves the original design requirement that hash-mismatch be "fully distinct from error," just expressed through the new shared animation language instead of the original ad hoc treatment.

### Files modified in Phase 3.1

- `backend/services/SyncManager.h/cpp` — added `startPortalListener()`/`stopPortalListener()`, `portalListenerPid_`, `portalDir_`; `init()` signature gained a `portalDir` parameter; `maybeRotatePort()` now calls `startPortalListener()`; `shutdown()` calls `stopPortalListener()`.
- `backend/main.cpp` — reads `sync.portal_dir` from config, passes to `SyncManager::init()`.
- `backend/config.json` — added `sync.portal_dir`.
- `setup.sh` — added `python3` dependency; opened `49152-65535/tcp` in the firewall; (sync-portal deploy step and directory creation were already added in Phase 3).
- `systemd/nas-backend.service` — added `KillMode=process`; added `ReadOnlyPaths` for `sync-portal/`.
- `webui/style.css` — fixed the dead `white-space`/`max-width`/`text-align` tooltip rules; replaced all five `.sync-*` state blocks with the new shared fade-glow keyframes (`sync-glow-idle/-syncing/-paused/-error/-hash-mismatch`), keeping `sync-spin` layered onto syncing only.
- `webui/app.js` — sync portal link now hardcodes `http://` instead of reusing `location.protocol`.

### Known gap carried forward

The stopgap portal listener (`python3 -m http.server`, forked/respawned by `SyncManager`) is explicitly not production-grade: no TLS, no auth beyond port secrecy, no self-supervision beyond the next daily rotation if it dies mid-window. This is documented prominently in the README's "Storage sync" section with an explicit warning banner, and is meant to be deleted in its entirety once a real sync engine exists and owns its own port/listener.

---

## Phase 3.2 — Cleanup, Lifecycle Hardening, Sync Architecture Decision

### Fix 1 — af-rule dropdown missing PT/DX options

The `af-rule` filter dropdown in the Alerts tab was never updated when PT-001/002 and DX-001/002/003 were added in Phase 2.7. The Rules tab content correctly listed all five new rules, but the dropdown only contained BF and MD entries. A user who received a PT-001 alert had no way to filter to it from the Alerts tab without using date as a workaround.

Fix: five missing `<option>` entries added to the `af-rule` `<select>` in `index.html`. Zero backend involvement.

Root cause worth noting for future rule additions: the `af-rule` dropdown and the Rules tab content are in separate sections of `index.html` with no structural relationship — adding a rule requires updating both independently. The three places that need touching when a new rule family is added: (1) the `af-rule` dropdown options, (2) the Rules tab rule table rows, (3) `AlertTypes.h` rule ID constants. The alert glow logic in `app.js` is severity-based not rule-ID-based, so it never needs touching.

---

### Issue 2 — Stopgap portal listener lifecycle gap

The current `startPortalListener()` / `stopPortalListener()` implementation forks a `python3 -m http.server` child and records its PID. The gap: if the child dies between port rotations (OOM kill, crash, manual kill), `SyncManager` has no knowledge of this — `portalListenerPid_` holds a stale PID, `stopPortalListener()` may `kill()` a PID that has since been reassigned to a different process, and the portal link silently returns "connection refused" until the next 24h rotation spawns a fresh listener.

**Decision: do not harden the stopgap.** The stopgap listener is scheduled for deletion when the real sync engine lands. Investing engineering time in lifecycle hardening for something that will be deleted is waste. The minimum viable workaround if the listener dies before then: `sudo systemctl restart nas-backend` respawns it. This is acceptable for a development-phase stopgap used by one person.

For reference, the correct fix if the stopgap needed to survive longer would be: call `kill(portalListenerPid_, 0)` (signal 0 = liveness probe, no actual signal sent) in `maybeRotatePort()` before attempting `stopPortalListener()`, and skip the kill if the process is already gone. The zombie reaping problem is solved by setting `SIGCHLD` to `SIG_IGN` in `main.cpp`, which makes the kernel auto-reap child processes without requiring `waitpid()` calls that would block the sync thread.

---

### Issue 3 — Sync architecture definition

"Sync" has been undefined since Phase 3. The scaffolding (SyncManager, SyncController, the portal, port rotation) exists but has no real engine because the architecture was never settled. This is the decision.

**Options considered and ruled out:**

- **Sync to cloud (S3, Backblaze, rclone):** Requires internet, third-party dependency, breaks the "no cloud, no subscription" principle. Ruled out.
- **Sync between two HomeNAS instances (peer-to-peer):** Requires both nodes online simultaneously, conflict resolution, and a Tailscale node discovery mechanism. Distributed systems scope, wrong for a personal NAS. Ruled out.
- **Sync a client folder to nas_storage over Tailscale:** This is Syncthing. Not building a custom Syncthing. `rsync` over SSH already handles this without NAS involvement. Ruled out.

**Decision: sync means one-way backup of nas_storage to a configured local path.**

The real engine will walk `nas_storage` and a configured `backup_root`, compare file trees (mtime + size as fast path, SHA-256 as fallback), and copy new/changed files to `backup_root`. Deletions in `nas_storage` are intentionally not mirrored to `backup_root` — the backup is a safety net, not a mirror. If ransomware wipes `nas_storage`, the backup survives a sync run intact.

Each run's outcome (files copied, bytes transferred, duration, errors) is persisted to `sync.db` (SQLite, WAL mode, same pattern as `events.db`). The current in-memory `std::deque<SyncLogEntry>` is replaced by this.

**What gets deleted from the existing scaffolding:**

- `startMockSync()` / `advanceMockSync()` — replaced by real file-tree walk.
- `std::deque<SyncLogEntry>` — replaced by `sync.db`.
- The stopgap `python3 -m http.server` portal listener — deleted. The sync portal becomes a static page served by NGINX alongside the main dashboard, inheriting its TLS and JWT auth.
- Port rotation (`rotatePort()`, `maybeRotatePort()`, the full rotating-port security model) — deleted. The rotating port was premised on the portal being a separate unauthenticated server; NGINX serving it makes this irrelevant.

**What survives:**

- `SyncManager` singleton — becomes the real engine owner.
- `SyncController` — API surface (`GET /api/sync/status`, `POST /api/sync/start`, `POST /api/sync/pause`) unchanged. Frontend doesn't change.
- The five sync state animations — unchanged, correctly model the state machine.
- Telegram notification hook — wired at `sync.complete` / `sync.error` as part of Phase 2.56.

**New config shape:**
```json
"sync": {
  "backup_root": "/mnt/backup_drive/nas_backup",
  "schedule_cron": "0 3 * * *",
  "delete_from_backup": false,
  "telegram_bot_token": "",
  "telegram_chat_id": ""
}
```

`schedule_cron` replaces the current 24h hardcoded interval. `delete_from_backup: false` is the safe default — user must opt in to mirrored deletions explicitly.

**Implementation order for next phase:**

1. Delete the stopgap listener and port rotation machinery from `SyncManager`.
2. Add `sync.db` with a `sync_runs` table (same WAL pattern as `events.db`).
3. Implement file-tree walk and copy engine in `SyncManager`.
4. Wire Telegram notifications at `sync.complete` / `sync.error`.
5. Update NGINX config to serve `sync-portal/` under `/sync/` on the main domain.

### Files modified in Phase 3.2

`webui/index.html` — added PT-001, PT-002, DX-001, DX-002, DX-003 to the `af-rule` filter dropdown in the Alerts tab.
