// ═══════════════════════════════════════════════════════════════════════════
//  HomeNAS Sync Portal — standalone landing page
//
//  This page is intentionally NOT part of the main webui SPA. Per design:
//  it's served on a separate, daily-rotating port and has no auth of its
//  own — the rotating port + a freshly-issued link from the authenticated
//  main dashboard IS the access control. See SyncManager devlog notes.
//
//  Data source: today, no separate sync backend exists yet (mocked — see
//  SyncController/SyncManager). This page calls back to the main HomeNAS
//  API on the well-known port (8080) for status/logs/file listing, since
//  that's where the real (mocked) data lives. When a real standalone sync
//  service exists, MAIN_API_BASE below is the one line that changes to
//  point at it instead.
// ═══════════════════════════════════════════════════════════════════════════

const MAIN_API_PORT = 8080;
const MAIN_API_BASE = `${location.protocol}//${location.hostname}:${MAIN_API_PORT}`;

let connected = false;
let lastState = null;

async function apiGet(path) {
  try {
    const resp = await fetch(MAIN_API_BASE + path);
    if (!resp.ok) throw new Error('bad status');
    setConnected(true);
    return await resp.json();
  } catch (e) {
    setConnected(false);
    return null;
  }
}

function setConnected(ok) {
  if (ok === connected) return;
  connected = ok;
  const dot   = document.getElementById('conn-dot');
  const label = document.getElementById('conn-label');
  dot.classList.toggle('connected', ok);
  dot.classList.toggle('disconnected', !ok);
  label.textContent = ok
    ? 'Connected'
    : 'Disconnected — link may have expired (port rotates daily)';
}

function fmtUtc(iso) {
  if (!iso) return '—';
  return iso.replace('T', ' ').replace('Z', ' UTC');
}

function relTime(iso) {
  if (!iso) return '—';
  const s = Math.floor((Date.now() - new Date(iso).getTime()) / 1000);
  if (s < 60)    return `${s}s ago`;
  if (s < 3600)  return `${Math.floor(s/60)}m ago`;
  if (s < 86400) return `${Math.floor(s/3600)}h ago`;
  return `${Math.floor(s/86400)}d ago`;
}

function escHtml(s) {
  if (s === null || s === undefined) return '';
  return String(s)
    .replace(/&/g,'&amp;').replace(/</g,'&lt;')
    .replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}

function stateLabel(state) {
  return {
    idle: 'Idle', syncing: 'Syncing', paused: 'Paused',
    error: 'Error', hash_mismatch: 'Hash Mismatch'
  }[state] || state;
}

// ── status panel ─────────────────────────────────────────────────────────────

async function refreshStatus() {
  const data = await apiGet('/api/sync/status');
  if (!data) return;

  lastState = data.state;

  const badge = document.getElementById('sync-state-badge');
  badge.className = 'status-badge ' + data.state;
  badge.textContent = stateLabel(data.state);

  const detail = document.getElementById('sync-state-detail');
  const progressWrap = document.getElementById('progress-wrap');

  if (data.state === 'syncing' || data.state === 'paused') {
    progressWrap.classList.remove('hidden');
    document.getElementById('progress-fill').style.width = data.percent_complete + '%';
    document.getElementById('progress-label').textContent =
      `${data.percent_complete}% complete`;
    detail.textContent = data.state === 'paused' ? 'Sync paused by user' : '';
  } else {
    progressWrap.classList.add('hidden');
    detail.textContent = data.state === 'idle'
      ? 'All files up to date' : '';
  }

  const etaEl = document.getElementById('stat-eta');
  etaEl.textContent = data.state === 'syncing'
    ? `~${Math.ceil(data.eta_seconds / 60)}m`
    : '—';

  document.getElementById('stat-hash').textContent =
    data.last_hash ? data.last_hash.slice(0, 16) + '…' : '—';
  document.getElementById('stat-hash-time').textContent =
    data.last_hash_check_utc ? `verified ${relTime(data.last_hash_check_utc)}` : '';

  document.getElementById('stat-port').textContent = data.current_port || '—';
  document.getElementById('stat-port-rotated').textContent =
    data.port_rotated_at_utc ? `rotated ${relTime(data.port_rotated_at_utc)}` : '';

  const issuesCard = document.getElementById('card-issues');
  if (data.state === 'error' || data.state === 'hash_mismatch') {
    issuesCard.classList.remove('hidden');
    loadIssues();
  } else {
    issuesCard.classList.add('hidden');
  }
}

// ── issues panel (error / hash_mismatch only) ─────────────────────────────────

let issuesExpanded = true;

async function loadIssues() {
  const body = document.getElementById('issues-body');
  if (!issuesExpanded) { body.classList.add('hidden'); return; }
  body.classList.remove('hidden');

  const data = await apiGet('/api/sync/logs?limit=50');
  const entries = data?.items || [];
  if (!entries.length) {
    body.innerHTML = `<div class="portal-empty">No issues logged</div>`;
    return;
  }
  body.innerHTML = entries.slice().reverse().map(e => `
    <div class="sync-issue-row">
      <span class="sync-issue-time">${fmtUtc(e.timestamp_utc)}</span>
      <span class="sync-issue-level ${e.level}">${e.level}</span>
      <div class="sync-issue-message">${escHtml(e.message)}</div>
    </div>`).join('');
}

document.getElementById('issues-toggle').addEventListener('click', () => {
  issuesExpanded = !issuesExpanded;
  document.getElementById('issues-toggle').textContent =
    issuesExpanded ? 'Hide' : 'Show';
  document.getElementById('issues-body').classList.toggle('hidden', !issuesExpanded);
});

// ── files panel ────────────────────────────────────────────────────────────────
// No auth on this page, so file listing is intentionally NOT wired to the
// real (JWT-protected) /api/ls endpoint — that would either require
// embedding credentials in a page reachable via a guessable-ish rotating
// port, or relaxing FilesystemController's auth, both worse than the
// current gap. Shown as a clearly-labelled placeholder until the real sync
// engine defines what "files" means in this view (e.g. files currently
// being synced, not the whole NAS tree).

function renderFilesPlaceholder() {
  const el = document.getElementById('portal-files');
  el.innerHTML = `<div class="portal-empty">
    File-level sync detail isn't available yet — this view will show
    in-flight and recently synced files once the sync engine is built.
  </div>`;
}

// ── init ──────────────────────────────────────────────────────────────────────

refreshStatus();
renderFilesPlaceholder();
setInterval(refreshStatus, 4000);
