// ═══════════════════════════════════════════════════════════════════════════
//  HomeNAS — app.js
//  Phase 1: NAS file manager (unchanged)
//  Phase 2.5: Security dashboard
// ═══════════════════════════════════════════════════════════════════════════

// ── state ─────────────────────────────────────────────────────────────────────
let token       = sessionStorage.getItem('nas_token') || null;
let currentPath = '/';
let activeTab   = 'files';

// ── api helpers ───────────────────────────────────────────────────────────────
async function api(method, path, body, isUpload) {
  const opts = {
    method,
    headers: { 'Authorization': `Bearer ${token}` }
  };
  if (body && !isUpload) {
    opts.headers['Content-Type'] = 'application/json';
    opts.body = JSON.stringify(body);
  } else if (isUpload) {
    opts.body = body;
  }
  const resp = await fetch(path, opts);
  if (resp.status === 401) { signOut(); return null; }
  return resp.json();
}

async function apiPatch(path, body) {
  const resp = await fetch(path, {
    method: 'PATCH',
    headers: {
      'Authorization': `Bearer ${token}`,
      'Content-Type': 'application/json'
    },
    body: JSON.stringify(body)
  });
  if (resp.status === 401) { signOut(); return null; }
  return resp.json();
}

function get(path)        { return api('GET',    path); }
function post(path, body) { return api('POST',   path, body); }
function del(path)        { return api('DELETE', path); }

// ── auth ──────────────────────────────────────────────────────────────────────
document.getElementById('login-btn').addEventListener('click', async () => {
  const username = document.getElementById('login-user').value.trim();
  const password = document.getElementById('login-pass').value;
  const err      = document.getElementById('login-error');

  const data = await fetch('/api/auth/login', {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ username, password })
  }).then(r => r.json()).catch(() => null);

  if (!data || !data.token) {
    err.textContent = data?.error || 'Login failed';
    err.classList.remove('hidden');
    return;
  }

  token = data.token;
  sessionStorage.setItem('nas_token', token);
  showApp();
});

document.getElementById('login-pass').addEventListener('keydown', e => {
  if (e.key === 'Enter') document.getElementById('login-btn').click();
});

document.getElementById('logout-btn').addEventListener('click', signOut);

function signOut() {
  stopPolling();
  token = null;
  sessionStorage.removeItem('nas_token');
  document.getElementById('auth-shell').classList.add('hidden');
  document.getElementById('login-screen').classList.remove('hidden');
}

// ── init ──────────────────────────────────────────────────────────────────────
if (token) showApp();

function showApp() {
  document.getElementById('login-screen').classList.add('hidden');
  document.getElementById('auth-shell').classList.remove('hidden');
  switchTab('files');
  navigate('/');
  startPolling();
}

// ── tab navigation ────────────────────────────────────────────────────────────
document.querySelectorAll('.nav-tab').forEach(btn => {
  btn.addEventListener('click', () => switchTab(btn.dataset.tab));
});

function switchTab(tab) {
  activeTab = tab;

  document.querySelectorAll('.nav-tab').forEach(b => {
    b.classList.toggle('active', b.dataset.tab === tab);
  });
  document.querySelectorAll('.tab-pane').forEach(p => {
    p.classList.add('hidden');
  });

  const pane = document.getElementById('tab-' + tab);
  if (pane) pane.classList.remove('hidden');

  // Lazy-load each view on first switch
  if (tab === 'overview') loadOverview();
  if (tab === 'alerts')   { alertsPage = 0; loadAlerts(); }
  if (tab === 'logs')     { eventsPage = 0; loadEvents(); }
}

// ── file manager navigation ───────────────────────────────────────────────────
async function navigate(path) {
  currentPath = path;
  renderBreadcrumb(path);
  const data = await get(`/api/ls?path=${encodeURIComponent(path)}`);
  if (!data) return;
  renderFileList(data.items || []);
}

function renderBreadcrumb(path) {
  const nav   = document.getElementById('breadcrumb');
  const parts = path.split('/').filter(Boolean);
  let html    = `<span class="crumb" data-path="/">~</span>`;
  let built   = '';
  parts.forEach((p, i) => {
    built += '/' + p;
    const active = i === parts.length - 1;
    html += `<span class="sep">/</span>
             <span class="crumb ${active ? 'active' : ''}"
                   data-path="${built}">${escHtml(p)}</span>`;
  });
  nav.innerHTML = html;
  nav.querySelectorAll('.crumb:not(.active)').forEach(el => {
    el.addEventListener('click', () => navigate(el.dataset.path));
  });
}

// ── file list ─────────────────────────────────────────────────────────────────
function renderFileList(items) {
  const main = document.getElementById('file-list');

  items.sort((a, b) => {
    if (a.is_dir !== b.is_dir) return b.is_dir - a.is_dir;
    return a.name.localeCompare(b.name);
  });

  if (items.length === 0) {
    main.innerHTML = `<div class="empty-state">This folder is empty</div>`;
    return;
  }

  const header = `
    <div class="list-header">
      <div></div><div>Name</div><div>Size</div><div>Modified</div><div></div>
    </div>`;

  const rows = items.map(item => {
    const icon     = item.is_dir ? '📁' : fileIcon(item.name);
    const size     = item.is_dir ? '—' : formatBytes(item.size);
    const modified = formatDate(item.modified);
    return `
      <div class="file-row"
           draggable="true"
           data-path="${escHtml(item.path)}"
           data-dir="${item.is_dir}"
           data-name="${escHtml(item.name)}">
        <div class="file-icon">${icon}</div>
        <div class="file-name">${escHtml(item.name)}</div>
        <div class="file-size">${size}</div>
        <div class="file-date">${modified}</div>
        <div class="file-actions">
          ${!item.is_dir ? `<button class="dl-btn">↓</button>` : ''}
          <button class="ren-btn">✎</button>
          <button class="del del-btn">✕</button>
          <div class="drag-handle" title="Drag to move">⠿</div>
        </div>
      </div>`;
  }).join('');

  main.innerHTML = header + rows;

  // ── attach click + action handlers ────────────────────────────────────────
  main.querySelectorAll('.file-row').forEach(row => {
    const path  = row.dataset.path;
    const isDir = row.dataset.dir === 'true';
    const name  = row.dataset.name;

    row.addEventListener('click', e => {
      if (e.target.closest('.file-actions')) return;
      if (isDir) navigate('/' + path);
      else downloadFile(path, name);
    });
    row.querySelector('.dl-btn')?.addEventListener('click', e => {
      e.stopPropagation(); downloadFile(path, name);
    });
    row.querySelector('.ren-btn').addEventListener('click', e => {
      e.stopPropagation(); showRenameModal(path, name);
    });
    row.querySelector('.del-btn').addEventListener('click', async e => {
      e.stopPropagation();
      if (!confirm(`Delete "${name}"?`)) return;
      await del(`/api/file?path=${encodeURIComponent('/' + path)}`);
      navigate(currentPath);
    });
  });

  // ── drag-to-move ──────────────────────────────────────────────────────────
  attachDragHandlers(main);
}

// Track what's being dragged — also exposed on window for path-bar drop zones
let _dragSourcePath = null;
let _dragSourceName = null;

// Exposed so index.html inline script can call it from crumb drop handlers
window._nasMove = async function(srcFull, destFull) {
  await post('/api/rename', { from: srcFull, to: destFull });
  navigate(currentPath);
};

Object.defineProperties(window, {
  _dragSourcePath: {
    get: () => _dragSourcePath,
    set: v => { _dragSourcePath = v; },
    configurable: true
  },
  _dragSourceName: {
    get: () => _dragSourceName,
    set: v => { _dragSourceName = v; },
    configurable: true
  }
});

function attachDragHandlers(container) {
  const rows = container.querySelectorAll('.file-row');

  rows.forEach(row => {
    const srcPath = row.dataset.path; // relative, no leading slash
    const srcName = row.dataset.name;

    // Only allow drag to start from the handle
    const handle = row.querySelector('.drag-handle');
    handle.addEventListener('mousedown', () => {
      row.setAttribute('draggable', 'true');
    });

    row.addEventListener('dragstart', e => {
      // Bail if drag didn't originate from handle
      if (!e.target.closest('.file-row') || !_dragViaHandle) return;
      _dragSourcePath = srcPath;
      _dragSourceName = srcName;
      e.dataTransfer.effectAllowed = 'move';
      // Minimal ghost — browser default ghost is fine, just tag it
      e.dataTransfer.setData('text/plain', srcName);
      setTimeout(() => row.classList.add('dragging'), 0);
    });

    row.addEventListener('dragend', () => {
      row.classList.remove('dragging');
      _dragSourcePath = null;
      _dragSourceName = null;
      _dragViaHandle  = false;
      // Clear any lingering drop targets
      container.querySelectorAll('.drop-target').forEach(r =>
        r.classList.remove('drop-target'));
    });

    // Only folder rows are valid drop targets
    if (row.dataset.dir !== 'true') return;

    row.addEventListener('dragenter', e => {
      e.preventDefault();
      if (!_dragSourcePath || row.dataset.path === _dragSourcePath) return;
      row.classList.add('drop-target');
    });

    row.addEventListener('dragover', e => {
      e.preventDefault();
      e.dataTransfer.dropEffect = 'move';
    });

    row.addEventListener('dragleave', e => {
      // Only clear if actually leaving this row (not entering a child)
      if (!row.contains(e.relatedTarget)) {
        row.classList.remove('drop-target');
      }
    });

    row.addEventListener('drop', async e => {
      e.preventDefault();
      row.classList.remove('drop-target');
      const destDirPath = row.dataset.path; // relative path of folder

      if (!_dragSourcePath || destDirPath === _dragSourcePath) return;

      // Build the new path: destination folder + source filename
      const srcFull  = '/' + _dragSourcePath;          // /dir/file.txt
      const destFull = '/' + destDirPath + '/' + _dragSourceName; // /dir/subdir/file.txt

      if (srcFull === destFull) return;

      await post('/api/rename', { from: srcFull, to: destFull });
      navigate(currentPath);
    });
  });
}

// Flag set only when a drag originates from the handle
let _dragViaHandle = false;
document.getElementById('file-list').addEventListener('mousedown', e => {
  _dragViaHandle = !!e.target.closest('.drag-handle');
});

// ── download ──────────────────────────────────────────────────────────────────
function downloadFile(path, name) {
  fetch(`/api/file?path=${encodeURIComponent('/' + path)}`, {
    headers: { 'Authorization': `Bearer ${token}` }
  }).then(r => r.blob()).then(blob => {
    const url = URL.createObjectURL(blob);
    const a   = document.createElement('a');
    a.href = url; a.download = name; a.click();
    URL.revokeObjectURL(url);
  });
}

// ── upload ────────────────────────────────────────────────────────────────────
document.getElementById('upload-btn').addEventListener('click', () => {
  document.getElementById('file-input').click();
});
document.getElementById('file-input').addEventListener('change', e => {
  uploadFiles(e.target.files); e.target.value = '';
});

const appShell    = document.getElementById('auth-shell');
const dropOverlay = document.getElementById('drop-overlay');
let dragCounter   = 0;

appShell.addEventListener('dragenter', e => {
  if (activeTab !== 'files') return;
  e.preventDefault(); dragCounter++;
  dropOverlay.classList.remove('hidden');
});
appShell.addEventListener('dragleave', () => {
  dragCounter--;
  if (dragCounter === 0) dropOverlay.classList.add('hidden');
});
appShell.addEventListener('dragover', e => e.preventDefault());
appShell.addEventListener('drop', e => {
  if (activeTab !== 'files') return;
  e.preventDefault(); dragCounter = 0;
  dropOverlay.classList.add('hidden');
  uploadFiles(e.dataTransfer.files);
});

async function uploadFiles(files) {
  if (!files.length) return;
  const bar   = document.getElementById('upload-bar');
  const prog  = document.getElementById('upload-progress');
  const label = document.getElementById('upload-label');
  bar.classList.remove('hidden');
  const fd = new FormData();
  for (const f of files) fd.append('files', f);
  label.textContent = `Uploading ${files.length} file(s)…`;
  prog.style.setProperty('--pct', '30%');
  await api('POST', `/api/upload?path=${encodeURIComponent(currentPath)}`, fd, true);
  prog.style.setProperty('--pct', '100%');
  label.textContent = 'Done';
  setTimeout(() => {
    bar.classList.add('hidden');
    prog.style.setProperty('--pct', '0%');
    navigate(currentPath);
  }, 800);
}

// ── new folder ────────────────────────────────────────────────────────────────
document.getElementById('new-folder-btn').addEventListener('click', () => {
  showModal('New folder', '', async name => {
    if (!name.trim()) return;
    const newPath = currentPath.replace(/\/$/, '') + '/' + name.trim();
    await post('/api/mkdir', { path: newPath });
    navigate(currentPath);
  });
});

// ── rename ────────────────────────────────────────────────────────────────────
function showRenameModal(path, currentName) {
  showModal('Rename', currentName, async newName => {
    if (!newName.trim() || newName === currentName) return;
    const dir     = '/' + path.substring(0, path.lastIndexOf('/') + 1);
    const newPath = dir.replace(/\/$/, '') + '/' + newName.trim();
    await post('/api/rename', { from: '/' + path, to: newPath });
    navigate(currentPath);
  });
}

// ── modal helper ──────────────────────────────────────────────────────────────
function showModal(title, value, onOk) {
  const overlay = document.getElementById('modal-overlay');
  const input   = document.getElementById('modal-input');
  document.getElementById('modal-title').textContent = title;
  input.value = value;
  overlay.classList.remove('hidden');
  setTimeout(() => { input.focus(); input.select(); }, 50);
  const ok     = document.getElementById('modal-ok');
  const cancel = document.getElementById('modal-cancel');
  const cleanup = () => overlay.classList.add('hidden');
  ok.onclick     = () => { cleanup(); onOk(input.value); };
  cancel.onclick = cleanup;
  input.onkeydown = e => {
    if (e.key === 'Enter')  { cleanup(); onOk(input.value); }
    if (e.key === 'Escape') cleanup();
  };
}

// ═══════════════════════════════════════════════════════════════════════════
//  DASHBOARD — Phase 2.5
// ═══════════════════════════════════════════════════════════════════════════

// ── polling ───────────────────────────────────────────────────────────────────
let pollTimer = null;
const POLL_INTERVAL = 15000; // 15 seconds

function startPolling() {
  stopPolling();
  pollTimer = setInterval(() => {
    updateAlertBadge();
    if (activeTab === 'overview') loadOverview();
  }, POLL_INTERVAL);
  updateAlertBadge();
}

function stopPolling() {
  if (pollTimer) { clearInterval(pollTimer); pollTimer = null; }
}

async function updateAlertBadge() {
  const data = await get('/api/alerts/stats/summary').catch(() => null);
  if (!data) return;
  const total = data.total_open || 0;
  const badge = document.getElementById('nav-alert-count');
  badge.textContent = total > 99 ? '99+' : total;
  badge.classList.toggle('hidden', total === 0);
}

// ── helpers ───────────────────────────────────────────────────────────────────
function todayIso() {
  return new Date().toISOString().slice(0, 10) + 'T00:00:00Z';
}

function relTime(isoUtc) {
  if (!isoUtc) return '—';
  const ms   = Date.now() - new Date(isoUtc).getTime();
  const s    = Math.floor(ms / 1000);
  if (s < 60)   return `${s}s ago`;
  if (s < 3600) return `${Math.floor(s/60)}m ago`;
  if (s < 86400)return `${Math.floor(s/3600)}h ago`;
  return `${Math.floor(s/86400)}d ago`;
}

function fmtUtc(isoUtc) {
  if (!isoUtc) return '—';
  return isoUtc.replace('T', ' ').replace('Z', ' UTC');
}

function sevPill(sev) {
  return `<span class="sev-pill ${sev}">${sev}</span>`;
}

function statusPill(st) {
  return `<span class="status-pill ${st}">${st}</span>`;
}

function resultPill(r) {
  return `<span class="result-pill ${r}">${r}</span>`;
}

function eventTypeBadge(t) {
  const color = t.includes('failure') || t.includes('delete')
    ? 'var(--sev-critical)' : 'var(--muted)';
  return `<span style="font-family:var(--mono);font-size:11px;color:${color}">${t}</span>`;
}

function nullOrVal(v, cls = '') {
  return v ? `<span class="${cls}">${escHtml(String(v))}</span>`
           : `<span class="expand-value null">—</span>`;
}

// ── OVERVIEW ──────────────────────────────────────────────────────────────────
async function loadOverview() {
  const today = todayIso();

  const [alertSummary, eventSummary, actorSummary, ipSummary,
         recentAlerts, recentEvents] = await Promise.all([
    get('/api/alerts/stats/summary'),
    get(`/api/events/stats/summary?from=${today}`),
    get(`/api/events/stats/summary?group_by=actor&limit=5&from=${today}`),
    get(`/api/events/stats/summary?group_by=source_ip&limit=5&from=${today}`),
    get('/api/alerts?limit=8&status=open'),
    get('/api/events?limit=12')
  ]);

  // Severity cards
  const bySev = {};
  if (alertSummary?.by_severity) {
    for (const [sev, counts] of Object.entries(alertSummary.by_severity)) {
      bySev[sev] = (counts.open || 0);
    }
  }
  ['critical','high','medium','low'].forEach(s => {
    document.getElementById('count-' + s).textContent = bySev[s] ?? 0;
  });
  document.getElementById('count-alerts-total').textContent =
    alertSummary?.total_open ?? 0;

  // Events today totals from summary
  let eventsToday = 0, authFail = 0, deletes = 0;
  if (eventSummary?.by_type) {
    for (const [type, counts] of Object.entries(eventSummary.by_type)) {
      const n = (counts.success || 0) + (counts.failure || 0);
      eventsToday += n;
      if (type === 'auth.login.failure') authFail += (counts.failure || 0);
      if (type === 'file.delete' || type === 'dir.delete') deletes += n;
    }
  }
  document.getElementById('count-events-today').textContent = eventsToday;
  document.getElementById('count-auth-failures').textContent = authFail;
  document.getElementById('count-deletes').textContent = deletes;

  // Severity cards click → alerts tab filtered
  document.querySelectorAll('#overview-sev-cards .stat-card').forEach(card => {
    card.onclick = () => {
      document.getElementById('af-severity').value = card.dataset.sev;
      document.getElementById('af-status').value   = 'open';
      switchTab('alerts');
      alertsPage = 0;
      loadAlerts();
    };
  });

  // Recent alerts panel
  renderOverviewAlerts(recentAlerts?.items || []);

  // Live feed panel
  renderOverviewFeed(recentEvents?.items || []);

  // Top IPs
  renderOffenders('overview-top-ips', ipSummary?.items || [], 'source_ip');

  // Top actors
  renderOffenders('overview-top-actors', actorSummary?.items || [], 'actor');
}

function renderOverviewAlerts(alerts) {
  const el = document.getElementById('overview-recent-alerts');
  if (!alerts.length) {
    el.innerHTML = `<div class="dash-empty">No open alerts</div>`; return;
  }
  el.innerHTML = alerts.map(a => `
    <div class="mini-row" data-alert-id="${a.id}">
      <div>${sevPill(a.severity)}</div>
      <div class="mini-row-main">
        <div class="mini-row-title">${escHtml(a.title)}</div>
        <div class="mini-row-sub">${escHtml(a.rule_id)} · ${escHtml(a.source_ip || a.actor_user || '—')}</div>
      </div>
      <div class="mini-row-right">${relTime(a.timestamp_utc)}</div>
    </div>`).join('');

  el.querySelectorAll('.mini-row').forEach(row => {
    row.addEventListener('click', () => {
      switchTab('alerts');
      alertsPage = 0;
      loadAlerts().then(() => expandAlert(parseInt(row.dataset.alertId)));
    });
  });
}

function renderOverviewFeed(events) {
  const el = document.getElementById('overview-live-feed');
  if (!events.length) {
    el.innerHTML = `<div class="dash-empty">No recent events</div>`; return;
  }
  el.innerHTML = events.map(e => {
    const isFailure = e.result === 'failure';
    const dotCls    = isFailure ? 'failure' : 'success';
    return `
      <div class="mini-row">
        <div class="live-dot ${dotCls}"></div>
        <div class="mini-row-main">
          <div class="mini-row-title" style="font-family:var(--mono);font-size:12px">
            ${escHtml(e.event_type)}
          </div>
          <div class="mini-row-sub">${escHtml(e.actor_user || '—')} · ${escHtml(e.source_ip || '—')}</div>
        </div>
        <div class="mini-row-right">${relTime(e.timestamp_utc)}</div>
      </div>`;
  }).join('');
}

function renderOffenders(containerId, items, keyField) {
  const el  = document.getElementById(containerId);
  if (!items.length) {
    el.innerHTML = `<div class="dash-empty">No data</div>`; return;
  }
  const maxTotal = items[0]?.total || 1;
  el.innerHTML = items.map((item, i) => {
    const key      = item[keyField] || '—';
    const allFail  = item.total > 0 && item.failures === item.total;
    const barPct   = Math.round((item.total / maxTotal) * 100);
    return `
      <div class="offender-row">
        <span class="offender-rank">${i + 1}</span>
        <span class="offender-key" title="${escHtml(key)}">${escHtml(key)}</span>
        <div class="offender-bar-wrap">
          <div class="offender-bar ${allFail ? 'danger' : ''}"
               style="width:${barPct}%"></div>
        </div>
        <span class="offender-count">${item.total}
          ${item.failures > 0
            ? `<span style="color:var(--sev-critical)"> ·${item.failures}✗</span>`
            : ''}
        </span>
      </div>`;
  }).join('');
}

// ── ALERTS TABLE ──────────────────────────────────────────────────────────────
let alertsPage       = 0;
const ALERTS_PAGE_SZ = 50;
let expandedAlertId  = null;

async function loadAlerts() {
  const tbody = document.getElementById('alerts-tbody');
  tbody.innerHTML = `<tr><td colspan="7"><div class="dash-loading">Loading…</div></td></tr>`;

  const sev    = document.getElementById('af-severity').value;
  const rule   = document.getElementById('af-rule').value;
  const status = document.getElementById('af-status').value;
  const from   = document.getElementById('af-from').value;
  const to     = document.getElementById('af-to').value;

  const params = new URLSearchParams({
    limit:  ALERTS_PAGE_SZ,
    offset: alertsPage * ALERTS_PAGE_SZ,
    ...(sev    && { severity: sev }),
    ...(rule   && { rule }),
    ...(status && { status }),
    ...(from   && { from: from + 'T00:00:00Z' }),
    ...(to     && { to:   to   + 'T23:59:59Z' }),
  });

  const data = await get('/api/alerts?' + params);
  if (!data) return;

  const alerts = data.items || [];
  if (!alerts.length) {
    tbody.innerHTML = `<tr><td colspan="7"><div class="dash-empty">No alerts found</div></td></tr>`;
    renderPagination('alerts-pagination', 0, 0, ALERTS_PAGE_SZ);
    return;
  }

  tbody.innerHTML = alerts.map(a => alertRow(a)).join('');
  attachAlertRowHandlers(alerts);
  renderPagination('alerts-pagination', alertsPage, alerts.length, ALERTS_PAGE_SZ,
    (p) => { alertsPage = p; loadAlerts(); });
}

function alertRow(a) {
  return `
    <tr class="expandable" data-alert-id="${a.id}">
      <td><div class="td-inner">${sevPill(a.severity)}</div></td>
      <td><div class="td-inner td-mono">${escHtml(a.rule_id)}</div></td>
      <td><div class="td-inner">${escHtml(a.title)}</div></td>
      <td class="col-hide-tablet">
        <div class="td-inner td-mono" style="font-size:11px">
          ${escHtml(a.source_ip || a.actor_user || '—')}
        </div>
      </td>
      <td><div class="td-inner td-mono" style="font-size:11px">${relTime(a.timestamp_utc)}</div></td>
      <td><div class="td-inner">${statusPill(a.status)}</div></td>
      <td><div class="td-inner" style="color:var(--muted);font-size:16px">›</div></td>
    </tr>`;
}

function attachAlertRowHandlers(alerts) {
  document.querySelectorAll('#alerts-tbody tr.expandable').forEach(row => {
    row.addEventListener('click', () => {
      const id    = parseInt(row.dataset.alertId);
      const alert = alerts.find(a => a.id === id);
      if (!alert) return;
      toggleAlertExpand(row, alert);
    });
  });
}

function toggleAlertExpand(row, alert) {
  const id = alert.id;

  // Close any existing expand
  const existing = document.getElementById('expand-alert-' + expandedAlertId);
  if (existing) existing.remove();

  if (expandedAlertId === id) {
    expandedAlertId = null;
    return;
  }
  expandedAlertId = id;
  renderAlertExpand(row, alert);
}

async function expandAlert(id) {
  // Find the row and expand it, loading detail from API
  const rows = document.querySelectorAll('#alerts-tbody tr.expandable');
  for (const row of rows) {
    if (parseInt(row.dataset.alertId) === id) {
      const data = await get('/api/alerts/' + id);
      if (data) toggleAlertExpand(row, data);
      return;
    }
  }
}

async function renderAlertExpand(row, alert) {
  // Parse evidence
  let evidence = {};
  try { evidence = JSON.parse(alert.evidence || '{}'); } catch {}

  // Fetch timeline in parallel with rendering
  const timelinePromise = fetchAlertTimeline(alert, evidence);

  // Build expand row
  const expandRow = document.createElement('tr');
  expandRow.className = 'expand-row';
  expandRow.id = 'expand-alert-' + alert.id;

  const colCount = 7;
  expandRow.innerHTML = `<td colspan="${colCount}">
    <div class="expand-panel">
      <div>
        <div class="expand-section-title">Alert Details</div>
        <div class="expand-field">
          <div class="expand-label">Rule</div>
          <div class="expand-value">${escHtml(alert.rule_id)}</div>
        </div>
        <div class="expand-field">
          <div class="expand-label">Severity</div>
          <div class="expand-value">${sevPill(alert.severity)}</div>
        </div>
        <div class="expand-field">
          <div class="expand-label">Timestamp</div>
          <div class="expand-value">${escHtml(fmtUtc(alert.timestamp_utc))}</div>
        </div>
        <div class="expand-field">
          <div class="expand-label">Source IP</div>
          ${nullOrVal(alert.source_ip, 'expand-value td-mono')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Actor</div>
          ${nullOrVal(alert.actor_user, 'expand-value td-mono')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Claimed User</div>
          ${nullOrVal(alert.claimed_user, 'expand-value td-mono')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Status</div>
          <div class="expand-value">${statusPill(alert.status)}</div>
        </div>
        <div class="status-actions" id="status-actions-${alert.id}">
          ${alert.status !== 'investigating'
            ? `<button class="status-action-btn primary"
                       onclick="updateAlertStatus(${alert.id},'investigating')">
                 Mark Investigating
               </button>` : ''}
          ${alert.status !== 'dismissed'
            ? `<button class="status-action-btn danger-btn"
                       onclick="updateAlertStatus(${alert.id},'dismissed')">
                 Dismiss
               </button>` : ''}
          ${alert.status !== 'open'
            ? `<button class="status-action-btn"
                       onclick="updateAlertStatus(${alert.id},'open')">
                 Reopen
               </button>` : ''}
        </div>
      </div>

      <div>
        <div class="expand-section-title">Evidence</div>
        <pre class="evidence-pre">${escHtml(JSON.stringify(evidence, null, 2))}</pre>
      </div>

      <div class="timeline" id="timeline-${alert.id}">
        <div class="expand-section-title">Timeline (±10 min)</div>
        <div class="dash-loading" style="padding:20px 0">Loading timeline…</div>
      </div>
    </div>
  </td>`;

  row.after(expandRow);

  // Fill timeline when it resolves
  timelinePromise.then(events => {
    const el = document.getElementById('timeline-' + alert.id);
    if (el) renderTimeline(el, events, alert);
  });
}

async function fetchAlertTimeline(alert, evidence) {
  // Build ±10min window around the alert
  const alertTime = new Date(alert.timestamp_utc);
  const from = new Date(alertTime.getTime() - 10 * 60000).toISOString()
    .replace(/\.\d+Z$/, 'Z');
  const to   = new Date(alertTime.getTime() + 10 * 60000).toISOString()
    .replace(/\.\d+Z$/, 'Z');

  const params = { from, to, limit: 50 };

  // Decide filter: IP-keyed rules use source_ip from evidence,
  // actor-keyed rules use actor from alert
  const ip    = evidence.source_ip || alert.source_ip;
  const actor = alert.actor_user;

  const queries = [];
  if (ip)    queries.push(get(`/api/events?${new URLSearchParams({...params, source_ip: ip})}`));
  if (actor) queries.push(get(`/api/events?${new URLSearchParams({...params, user: actor})}`));
  if (!queries.length) queries.push(get(`/api/events?${new URLSearchParams(params)}`));

  const results = await Promise.all(queries);

  // Merge, deduplicate by id, sort ascending
  const seen = new Set();
  const merged = [];
  for (const r of results) {
    for (const ev of (r?.items || [])) {
      if (!seen.has(ev.id)) { seen.add(ev.id); merged.push(ev); }
    }
  }
  merged.sort((a, b) => a.id - b.id);
  return merged;
}

function renderTimeline(container, events, alert) {
  if (!events.length) {
    container.innerHTML = `<div class="expand-section-title">Timeline (±10 min)</div>
      <div class="dash-empty" style="padding:12px 0">No related events found</div>`;
    return;
  }

  const alertTs = alert.timestamp_utc;

  let html = `<div class="expand-section-title">Timeline (±10 min)</div>
    <ul class="timeline-list">`;

  let alertInserted = false;
  for (const ev of events) {
    // Insert alert marker at the right chronological position
    if (!alertInserted && ev.timestamp_utc >= alertTs) {
      html += `
        <li class="tl-item">
          <div class="tl-dot alert-marker"></div>
          <div class="tl-content">
            <div class="tl-time">${fmtUtc(alertTs)}</div>
            <div class="tl-event alert-label">▶ ALERT ${escHtml(alert.rule_id)} — ${escHtml(alert.title)}</div>
          </div>
        </li>`;
      alertInserted = true;
    }
    const dotCls = ev.result === 'failure' ? 'failure' : 'success';
    html += `
      <li class="tl-item">
        <div class="tl-dot ${dotCls}"></div>
        <div class="tl-content">
          <div class="tl-time">${fmtUtc(ev.timestamp_utc)}</div>
          <div class="tl-event">${escHtml(ev.event_type)}</div>
          <div class="tl-meta">
            ${escHtml(ev.actor_user || '—')} · ${escHtml(ev.source_ip || '—')}
            ${ev.target_path ? ' · ' + escHtml(ev.target_path) : ''}
          </div>
        </div>
      </li>`;
  }

  // Insert alert marker at end if all events were before it
  if (!alertInserted) {
    html += `
      <li class="tl-item">
        <div class="tl-dot alert-marker"></div>
        <div class="tl-content">
          <div class="tl-time">${fmtUtc(alertTs)}</div>
          <div class="tl-event alert-label">▶ ALERT ${escHtml(alert.rule_id)} — ${escHtml(alert.title)}</div>
        </div>
      </li>`;
  }

  html += `</ul>`;
  container.innerHTML = html;
}

async function updateAlertStatus(id, status) {
  const data = await apiPatch(`/api/alerts/${id}/status`, { status });
  if (!data || data.error) return;
  // Refresh the table row and expand panel
  alertsPage = alertsPage; // no-op, just signal
  expandedAlertId = null;
  await loadAlerts();
  updateAlertBadge();
}

// ── ALERTS FILTERS ────────────────────────────────────────────────────────────
document.getElementById('af-apply').addEventListener('click', () => {
  alertsPage = 0; loadAlerts();
});
document.getElementById('af-reset').addEventListener('click', () => {
  ['af-severity','af-rule','af-status'].forEach(id => {
    document.getElementById(id).value = '';
  });
  ['af-from','af-to'].forEach(id => {
    document.getElementById(id).value = '';
  });
  alertsPage = 0; loadAlerts();
});

// ── EVENTS TABLE ──────────────────────────────────────────────────────────────
let eventsPage       = 0;
const EVENTS_PAGE_SZ = 50;
let expandedEventId  = null;

async function loadEvents() {
  const tbody = document.getElementById('events-tbody');
  tbody.innerHTML = `<tr><td colspan="7"><div class="dash-loading">Loading…</div></td></tr>`;

  const type   = document.getElementById('ef-type').value;
  const result = document.getElementById('ef-result').value;
  const user   = document.getElementById('ef-user').value.trim();
  const from   = document.getElementById('ef-from').value;
  const to     = document.getElementById('ef-to').value;

  const params = new URLSearchParams({
    limit:  EVENTS_PAGE_SZ,
    offset: eventsPage * EVENTS_PAGE_SZ,
    ...(type   && { type }),
    ...(result && { result }),
    ...(user   && { user }),
    ...(from   && { from: from + 'T00:00:00Z' }),
    ...(to     && { to:   to   + 'T23:59:59Z' }),
  });

  const data = await get('/api/events?' + params);
  if (!data) return;

  const events = data.items || [];
  if (!events.length) {
    tbody.innerHTML = `<tr><td colspan="7"><div class="dash-empty">No events found</div></td></tr>`;
    renderPagination('events-pagination', 0, 0, EVENTS_PAGE_SZ);
    return;
  }

  tbody.innerHTML = events.map(ev => eventRow(ev)).join('');
  attachEventRowHandlers(events);
  renderPagination('events-pagination', eventsPage, events.length, EVENTS_PAGE_SZ,
    (p) => { eventsPage = p; loadEvents(); });
}

function eventRow(ev) {
  return `
    <tr class="expandable" data-event-id="${ev.id}">
      <td><div class="td-inner td-mono" style="font-size:11px">${relTime(ev.timestamp_utc)}</div></td>
      <td><div class="td-inner">${eventTypeBadge(ev.event_type)}</div></td>
      <td class="col-hide-tablet">
        <div class="td-inner td-mono" style="font-size:11px">${escHtml(ev.actor_user || '—')}</div>
      </td>
      <td class="col-hide-tablet">
        <div class="td-inner td-mono" style="font-size:11px">${escHtml(ev.source_ip || '—')}</div>
      </td>
      <td><div class="td-inner">${resultPill(ev.result)}</div></td>
      <td class="col-hide-tablet">
        <div class="td-inner td-mono" style="font-size:11px;max-width:200px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap">
          ${escHtml(ev.target_path || '—')}
        </div>
      </td>
      <td><div class="td-inner" style="color:var(--muted);font-size:16px">›</div></td>
    </tr>`;
}

function attachEventRowHandlers(events) {
  document.querySelectorAll('#events-tbody tr.expandable').forEach(row => {
    row.addEventListener('click', () => {
      const id  = parseInt(row.dataset.eventId);
      const ev  = events.find(e => e.id === id);
      if (!ev) return;
      toggleEventExpand(row, ev);
    });
  });
}

function toggleEventExpand(row, ev) {
  const id = ev.id;
  const existing = document.getElementById('expand-event-' + expandedEventId);
  if (existing) existing.remove();
  if (expandedEventId === id) { expandedEventId = null; return; }
  expandedEventId = id;
  renderEventExpand(row, ev);
}

function renderEventExpand(row, ev) {
  const expandRow = document.createElement('tr');
  expandRow.className = 'expand-row';
  expandRow.id = 'expand-event-' + ev.id;

  expandRow.innerHTML = `<td colspan="7">
    <div class="expand-panel">
      <div>
        <div class="expand-section-title">Event Record</div>
        <div class="expand-field">
          <div class="expand-label">ID</div>
          <div class="expand-value td-mono">#${ev.id}</div>
        </div>
        <div class="expand-field">
          <div class="expand-label">Timestamp (UTC)</div>
          <div class="expand-value td-mono">${escHtml(fmtUtc(ev.timestamp_utc))}</div>
        </div>
        <div class="expand-field">
          <div class="expand-label">Event Type</div>
          <div class="expand-value td-mono">${escHtml(ev.event_type)}</div>
        </div>
        <div class="expand-field">
          <div class="expand-label">Result</div>
          <div class="expand-value">${resultPill(ev.result)}</div>
        </div>
        <div class="expand-field">
          <div class="expand-label">Actor</div>
          ${nullOrVal(ev.actor_user, 'expand-value td-mono')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Source IP</div>
          ${nullOrVal(ev.source_ip, 'expand-value td-mono')}
        </div>
      </div>
      <div>
        <div class="expand-section-title">Details</div>
        <div class="expand-field">
          <div class="expand-label">Target Path</div>
          ${nullOrVal(ev.target_path, 'expand-value td-mono')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Secondary Path</div>
          ${nullOrVal(ev.secondary_path, 'expand-value td-mono')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Failure Reason</div>
          ${nullOrVal(ev.failure_reason, 'expand-value td-mono')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Bytes Transferred</div>
          ${ev.bytes_transferred != null
            ? `<div class="expand-value td-mono">${formatBytes(ev.bytes_transferred)}</div>`
            : `<span class="expand-value null">—</span>`}
        </div>
        <div class="expand-field">
          <div class="expand-label">User Agent</div>
          ${nullOrVal(ev.user_agent, 'expand-value')}
        </div>
        <div class="expand-field">
          <div class="expand-label">Request ID</div>
          ${nullOrVal(ev.request_id, 'expand-value td-mono')}
        </div>
      </div>
    </div>
  </td>`;

  row.after(expandRow);
}

// ── EVENT FILTERS ─────────────────────────────────────────────────────────────
document.getElementById('ef-apply').addEventListener('click', () => {
  eventsPage = 0; loadEvents();
});
document.getElementById('ef-reset').addEventListener('click', () => {
  document.getElementById('ef-type').value   = '';
  document.getElementById('ef-result').value = '';
  document.getElementById('ef-user').value   = '';
  document.getElementById('ef-from').value   = '';
  document.getElementById('ef-to').value     = '';
  eventsPage = 0; loadEvents();
});

// ── PAGINATION ────────────────────────────────────────────────────────────────
function renderPagination(containerId, page, itemCount, pageSize, onPage) {
  const el    = document.getElementById(containerId);
  const hasPrev = page > 0;
  const hasNext = itemCount >= pageSize;

  if (!hasPrev && !hasNext) { el.innerHTML = ''; return; }

  el.innerHTML = `
    <button class="page-btn" id="${containerId}-prev"
            ${!hasPrev ? 'disabled' : ''}>← Prev</button>
    <span class="page-info">Page ${page + 1}</span>
    <button class="page-btn" id="${containerId}-next"
            ${!hasNext ? 'disabled' : ''}>Next →</button>`;

  if (hasPrev) {
    el.querySelector('#' + containerId + '-prev')
      .addEventListener('click', () => onPage(page - 1));
  }
  if (hasNext) {
    el.querySelector('#' + containerId + '-next')
      .addEventListener('click', () => onPage(page + 1));
  }
}

// ═══════════════════════════════════════════════════════════════════════════
//  SYNC STATUS — bottom-left FAB
// ═══════════════════════════════════════════════════════════════════════════

let syncPollTimer = null;
const SYNC_POLL_INTERVAL = 5000; // tighter than the 15s dashboard poll —
                                   // sync state (esp. % complete) is more
                                   // time-sensitive while actively syncing
let lastSyncState = null;

function startSyncPolling() {
  stopSyncPolling();
  syncPollTimer = setInterval(updateSyncIcon, SYNC_POLL_INTERVAL);
  updateSyncIcon();
}
function stopSyncPolling() {
  if (syncPollTimer) { clearInterval(syncPollTimer); syncPollTimer = null; }
}

async function updateSyncIcon() {
  const data = await get('/api/sync/status').catch(() => null);
  if (!data) return;

  const btn = document.getElementById('sync-btn');
  btn.classList.remove('sync-idle','sync-syncing','sync-paused','sync-error','sync-hash-mismatch');
  btn.classList.add('sync-' + data.state.replace('_', '-'));
  lastSyncState = data.state;

  if (data.state === 'syncing') {
    const etaMin = Math.ceil(data.eta_seconds / 60);
    btn.setAttribute('data-tip',
      `Syncing — ${data.percent_complete}% — ~${etaMin}m remaining`);
  } else if (data.state === 'paused') {
    btn.setAttribute('data-tip', `Sync paused — ${data.percent_complete}% complete`);
  } else if (data.state === 'error') {
    btn.setAttribute('data-tip', 'Problem during sync — click to see logs');
  } else if (data.state === 'hash_mismatch') {
    btn.setAttribute('data-tip', 'Storage hash mismatch — click to see logs');
  } else {
    const checked = data.last_hash_check_utc ? relTime(data.last_hash_check_utc) : '—';
    btn.setAttribute('data-tip', `Idle — hash verified ${checked}`);
  }
}

document.getElementById('sync-btn').addEventListener('click', async () => {
  if (lastSyncState === 'error' || lastSyncState === 'hash_mismatch') {
    openSyncLogsPanel();
    return;
  }
  // Normal operation (idle/syncing/paused) — open the standalone sync
  // landing page, served on the daily-rotating port. The port is read
  // fresh on every click rather than cached, since it can rotate at any
  // time and a stale port would silently 404.
  //
  // Forced http:// rather than reusing location.protocol: the main
  // dashboard is served over https (NGINX terminates TLS), but the
  // stopgap portal listener (python3 -m http.server — see SyncManager)
  // speaks plain HTTP with no TLS at all. Reusing https here sends a TLS
  // ClientHello at a plaintext server, which the browser reports as
  // SSL_ERROR_RX_RECORD_TOO_LONG rather than a clear "wrong protocol"
  // message. This is specific to the stopgap; once a real sync engine
  // exists and (if) it terminates TLS itself, this should switch back to
  // matching the main page's protocol or to whatever scheme the real
  // service requires.
  const data = await get('/api/sync/status').catch(() => null);
  if (!data || !data.current_port) return;
  const url = `http://${location.hostname}:${data.current_port}/`;
  window.open(url, '_blank');
});

async function openSyncLogsPanel() {
  const panel = document.getElementById('sync-logs-panel');
  const body  = document.getElementById('sync-logs-body');
  body.innerHTML = `<div class="dash-loading">Loading…</div>`;
  panel.classList.remove('hidden');

  const data = await get('/api/sync/logs?limit=100').catch(() => null);
  const entries = data?.items || [];

  if (!entries.length) {
    body.innerHTML = `<div class="dash-empty">No log entries</div>`;
    return;
  }

  // Most recent first for a logs panel — opposite of the timeline view,
  // where chronological-ascending makes sense for following an incident.
  body.innerHTML = entries.slice().reverse().map(e => `
    <div class="sync-log-row">
      <span class="sync-log-time">${fmtUtc(e.timestamp_utc)}</span>
      <span class="sync-log-level ${e.level}">${e.level}</span>
      <div class="sync-log-message">${escHtml(e.message)}</div>
    </div>`).join('');
}

document.getElementById('sync-logs-close').addEventListener('click', () => {
  document.getElementById('sync-logs-panel').classList.add('hidden');
});

// Start sync polling alongside alert polling once the app shell is shown.
const _origShowApp = showApp;
showApp = function() {
  _origShowApp();
  startSyncPolling();
};
const _origSignOut = signOut;
signOut = function() {
  stopSyncPolling();
  document.getElementById('sync-logs-panel').classList.add('hidden');
  _origSignOut();
};

function formatBytes(n) {
  if (n === undefined || n === null) return '—';
  if (n < 1024)       return n + ' B';
  if (n < 1048576)    return (n / 1024).toFixed(1) + ' KB';
  if (n < 1073741824) return (n / 1048576).toFixed(1) + ' MB';
  return (n / 1073741824).toFixed(2) + ' GB';
}

function formatDate(ts) {
  if (!ts) return '—';
  return new Date(ts * 1000).toLocaleDateString(undefined, {
    year: 'numeric', month: 'short', day: 'numeric'
  });
}

function fileIcon(name) {
  const ext = name.split('.').pop().toLowerCase();
  const map = {
    pdf: '📄', jpg: '🖼', jpeg: '🖼', png: '🖼', gif: '🖼', webp: '🖼', svg: '🖼',
    mp4: '🎬', mov: '🎬', mkv: '🎬', avi: '🎬',
    mp3: '🎵', flac: '🎵', wav: '🎵', ogg: '🎵',
    zip: '🗜', gz: '🗜', tar: '🗜', rar: '🗜',
    txt: '📝', md: '📝', log: '📝',
    js: '📦', ts: '📦', py: '📦', cpp: '📦', c: '📦', go: '📦',
  };
  return map[ext] || '📄';
}

function escHtml(s) {
  if (s === null || s === undefined) return '';
  return String(s)
    .replace(/&/g,'&amp;')
    .replace(/</g,'&lt;')
    .replace(/>/g,'&gt;')
    .replace(/"/g,'&quot;');
}
