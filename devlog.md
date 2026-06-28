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
