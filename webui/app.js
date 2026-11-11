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

function showApp() {
  document.getElementById('login-screen').classList.add('hidden');
  document.getElementById('auth-shell').classList.remove('hidden');
  switchTab('files');
  navigate('/');
  startPolling();
}

// ── theme ─────────────────────────────────────────────────────────────────────
// The initial data-theme is set by the inline script in <head> so it lands
// before first paint; this only handles toggling and label sync afterwards.
const themeToggle = document.getElementById('theme-toggle');

function applyTheme(theme) {
  document.documentElement.setAttribute('data-theme', theme);
  // Show the theme you'd get by clicking, not the one you're in — the button
  // is an action, and labelling it with the current state makes the click
  // feel like it does the opposite of what it says.
  const goingTo = theme === 'dark' ? 'light' : 'dark';
  const label   = `Switch to ${goingTo} theme`;
  themeToggle.querySelector('.theme-icon').innerHTML =
    theme === 'dark' ? '&#9728;' : '&#9790;';   /* ☀ sun : ☾ moon */
  themeToggle.title = label;
  themeToggle.setAttribute('aria-label', label);
}

themeToggle.addEventListener('click', () => {
  const next = document.documentElement.getAttribute('data-theme') === 'dark'
    ? 'light' : 'dark';
  applyTheme(next);
  // Persisted in localStorage, not sessionStorage (where the auth token
  // lives): a theme choice should outlive the session, a token must not.
  try { localStorage.setItem('nas_theme', next); } catch (e) { /* storage blocked */ }
});

applyTheme(document.documentElement.getAttribute('data-theme') || 'dark');

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
  currentOrder = Array.isArray(data.order) ? data.order : [];
  renderFileList(orderItems(data.items || [], currentOrder));
}

// ── row order ─────────────────────────────────────────────────────────────────
// currentOrder is the manual order saved on the server for this folder (a
// list of names; empty = none). currentItems is what is on screen, in order.
let currentOrder = [];
let currentItems = [];

// Folders first, then by name — unless a manual order was saved, in which
// case its names come first in that order and anything it doesn't mention
// (a file uploaded since) follows in the default order. The saved list is
// only ever a hint: a stale or partial one can't hide an entry.
function orderItems(items, order) {
  items.sort((a, b) => {
    if (a.is_dir !== b.is_dir) return b.is_dir - a.is_dir;
    return a.name.localeCompare(b.name);
  });
  if (!order.length) return items;
  const pos    = new Map(order.map((n, i) => [n, i]));
  const listed = items.filter(i => pos.has(i.name))
                      .sort((a, b) => pos.get(a.name) - pos.get(b.name));
  return listed.concat(items.filter(i => !pos.has(i.name)));
}

async function saveOrder(names) {
  const data = await post('/api/order', { path: currentPath, order: names });
  if (!data || data.error) {
    showToast(data?.error || 'Could not save the order', { error: true });
    return false;
  }
  currentOrder = names;
  return true;
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
  currentItems = items;

  if (items.length === 0) {
    main.innerHTML = `<div class="empty-state">This folder is empty</div>`;
    return;
  }

  const header = `
    <div class="list-header">
      <div></div>
      <div>Name${currentOrder.length
        ? ` <span class="order-note">· custom order <button id="order-reset" type="button">Reset</button></span>`
        : ''}</div>
      <div>Size</div><div>Modified</div><div></div>
    </div>`;

  const rows = items.map(item => {
    const icon     = item.is_dir ? '📁' : fileIcon(item.name);
    const size     = item.is_dir ? '—' : formatBytes(item.size);
    const modified = formatDate(item.modified);
    return `
      <div class="file-row"
           data-path="${escHtml(item.path)}"
           data-dir="${item.is_dir}"
           data-name="${escHtml(item.name)}">
        <div class="file-icon">${icon}</div>
        <div class="file-name">${escHtml(item.name)}</div>
        <div class="file-size">${size}</div>
        <div class="file-date">${modified}</div>
        <div class="file-actions">
          ${!item.is_dir ? `<button class="dl-btn" aria-label="Download">${ICON_DOWNLOAD}</button>` : ''}
          <button class="ren-btn" aria-label="Rename">✎</button>
          <button class="del del-btn" aria-label="Delete">${ICON_TRASH}</button>
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

  main.querySelector('#order-reset')?.addEventListener('click', async () => {
    const previous = currentOrder;
    if (!await saveOrder([])) return;
    renderFileList(orderItems(currentItems, []));
    showToast('Back to the default order', {
      undo: async () => {
        if (await saveOrder(previous)) renderFileList(orderItems(currentItems, previous));
      }
    });
  });
}

// ── drag: rearrange, move into a folder, merge ───────────────────────────────
//
// One drag gesture, three outcomes, chosen by where on a row you let go:
//
//     top edge      → place the dragged row above this one     (rearrange)
//     middle        → move it INTO this folder                 (folders only)
//     bottom edge   → place it below this one                  (rearrange)
//
// The middle band exists only on folder rows, and only while Shift is not
// held. On file rows — and on every row with Shift down — the whole row is
// rearrange-only, split at its midpoint. So Shift is the "I am just
// rearranging" key: with it held there is no position on screen that moves
// anything into a folder. A label follows the cursor saying exactly what
// letting go will do, so none of this has to be judged by eye.
//
// This is built on pointer events, not the HTML5 drag-and-drop API. Native
// DnD suspends keyboard events for the length of the drag and leaves it to
// each browser whether drag events carry modifier state at all — Firefox on
// Linux reports shiftKey as false throughout — so "hold Shift" cannot be
// made reliable on top of it. With pointer events Shift is an ordinary key,
// the label can track the cursor, and touch screens work too.

// Fraction of a folder row's height, at top and bottom, that means
// "rearrange" rather than "move into". Adjacent rows' edge bands meet at the
// boundary between them, so the rearrange target between two folders is
// twice this.
const EDGE_BAND = 0.3;
// Pointer travel before a press on the handle becomes a drag.
const DRAG_THRESHOLD = 4;

const fileListEl = document.getElementById('file-list');

// The drag in progress, or null.
let _drag = null;

// Nothing in the list is natively draggable any more; stop stray native
// drags (a selected filename, an emoji icon) from starting.
fileListEl.addEventListener('dragstart', e => e.preventDefault());

fileListEl.addEventListener('pointerdown', e => {
  const handle = e.target.closest('.drag-handle');
  if (!handle || e.button !== 0 || _drag) return;
  const row = handle.closest('.file-row');
  if (!row) return;
  e.preventDefault();                       // no text selection while dragging
  handle.setPointerCapture(e.pointerId);
  _drag = {
    row, pointerId: e.pointerId,
    path: row.dataset.path, name: row.dataset.name, isDir: row.dataset.dir === 'true',
    startX: e.clientX, startY: e.clientY, x: e.clientX, y: e.clientY,
    shift: e.shiftKey, active: false, scroller: null, raf: 0
  };
});

document.addEventListener('pointermove', e => {
  if (!_drag || e.pointerId !== _drag.pointerId) return;
  _drag.x = e.clientX; _drag.y = e.clientY; _drag.shift = e.shiftKey;
  if (!_drag.active) {
    if (Math.hypot(e.clientX - _drag.startX, e.clientY - _drag.startY) < DRAG_THRESHOLD) return;
    _drag.active = true;
    _drag.row.classList.add('dragging');
    document.body.classList.add('row-dragging');
    _drag.scroller = scrollParent(fileListEl);
    _drag.raf = requestAnimationFrame(autoScroll);
  }
  trackDrag();
});

document.addEventListener('pointerup', e => {
  if (!_drag || e.pointerId !== _drag.pointerId) return;
  _drag.x = e.clientX; _drag.y = e.clientY; _drag.shift = e.shiftKey;
  const drag = _drag;
  const intent = drag.active ? dropIntent() : null;
  endDrag();
  if (intent) performDrop(drag, intent);
});

document.addEventListener('pointercancel', e => {
  if (_drag && e.pointerId === _drag.pointerId) endDrag();
});

// Shift pressed or released without moving the mouse must update the marker
// and label straight away; Esc abandons the drag.
function onDragKey(e) {
  if (!_drag || !_drag.active) return;
  if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); endDrag(); return; }
  if (e.key === 'Shift') { _drag.shift = e.type === 'keydown'; trackDrag(); }
}
document.addEventListener('keydown', onDragKey, true);
document.addEventListener('keyup',   onDragKey, true);
// Alt-tabbing away mid-drag would otherwise leave it stuck to the cursor.
window.addEventListener('blur', () => { if (_drag) endDrag(); });

function endDrag() {
  if (!_drag) return;
  const wasActive = _drag.active;
  cancelAnimationFrame(_drag.raf);
  _drag.row.classList.remove('dragging');
  _drag = null;
  document.body.classList.remove('row-dragging');
  clearDropMarks();
  setDragHint(null);
  setDragTip(null);
  if (wasActive) {
    // The release also produces a click; swallow that one so it can't open
    // or download whatever happens to be under the cursor.
    const swallow = ev => { ev.stopPropagation(); ev.preventDefault(); };
    window.addEventListener('click', swallow, { capture: true, once: true });
    setTimeout(() => window.removeEventListener('click', swallow, true), 0);
  }
}

function clearDropMarks() {
  document.querySelectorAll('.drop-target, .drop-before, .drop-after, .crumb-drop-target')
    .forEach(r => r.classList.remove('drop-target', 'drop-before', 'drop-after', 'crumb-drop-target'));
}

function scrollParent(el) {
  for (let p = el; p && p !== document.body; p = p.parentElement) {
    const oy = getComputedStyle(p).overflowY;
    if ((oy === 'auto' || oy === 'scroll') && p.scrollHeight > p.clientHeight) return p;
  }
  return document.scrollingElement;
}

// Native drag-and-drop scrolled the page when you neared an edge; do the same.
function autoScroll() {
  if (!_drag || !_drag.active) return;
  const zone = 70, max = 14;
  const fromTop = _drag.y, fromBottom = window.innerHeight - _drag.y;
  let dy = 0;
  if      (fromTop    < zone) dy = -max * (1 - Math.max(fromTop, 0)    / zone);
  else if (fromBottom < zone) dy =  max * (1 - Math.max(fromBottom, 0) / zone);
  if (dy) {
    const before = _drag.scroller.scrollTop;
    _drag.scroller.scrollTop += dy;
    if (_drag.scroller.scrollTop !== before) trackDrag();
  }
  _drag.raf = requestAnimationFrame(autoScroll);
}

// Resolve the current pointer position to what a release would do:
//   { zone: 'before' | 'after' | 'into', row }   on the file list
//   { zone: 'crumb', el, destPath }              on the path bar
// or null when letting go there would change nothing.
function dropIntent() {
  const d = _drag;
  if (!d.row.isConnected) return null;      // list re-rendered under the drag
  const under = document.elementFromPoint(d.x, d.y);
  if (!under) return null;

  // Path bar: move up to a parent folder.
  const crumb = under.closest('[data-drop-path]');
  if (crumb) {
    const destPath = crumb.dataset.dropPath;                       // "/" or "/a/b"
    const srcDir   = '/' + d.path.substring(0, d.path.lastIndexOf('/') + 1).replace(/\/$/, '');
    if (srcDir === destPath) return null;                          // already there
    if (('/' + d.path) === destPath) return null;
    return { zone: 'crumb', el: crumb, destPath };
  }

  if (!fileListEl.contains(under)) return null;
  const rows = [...fileListEl.querySelectorAll('.file-row')];
  if (!rows.length) return null;

  let row = under.closest('.file-row');
  let zone;
  if (!row) {
    // Over the header or the empty space under the list: snap to the
    // nearest end, so "drag it to the bottom" doesn't need pixel aim.
    // Anywhere else that isn't a row (the gap beside one) changes nothing.
    const first = rows[0].getBoundingClientRect();
    const last  = rows[rows.length - 1].getBoundingClientRect();
    if      (d.y < first.top)   { row = rows[0]; zone = 'before'; }
    else if (d.y > last.bottom) { row = rows[rows.length - 1]; zone = 'after'; }
    else return null;
  } else {
    const box = row.getBoundingClientRect();
    const y   = (d.y - box.top) / box.height;
    const canEnter = row.dataset.dir === 'true' && !d.shift;
    if (!canEnter)                 zone = y < 0.5 ? 'before' : 'after';
    else if (y < EDGE_BAND)        zone = 'before';
    else if (y > 1 - EDGE_BAND)    zone = 'after';
    else                           zone = 'into';
  }

  const src = rows.indexOf(d.row);
  const dst = rows.indexOf(row);
  if (dst === src) return null;                          // onto itself
  // Positions that leave the row exactly where it already is.
  if (zone === 'before' && dst === src + 1) return null;
  if (zone === 'after'  && dst === src - 1) return null;
  return { zone, row };
}

// Repaint the marker, the cursor label and the legend for the current
// pointer position and Shift state.
function trackDrag() {
  if (!_drag || !_drag.active) return;
  const intent = dropIntent();
  clearDropMarks();
  setDragHint(_drag.shift);
  if (!intent) { setDragTip(null); return; }

  if (intent.zone === 'crumb') {
    intent.el.classList.add('crumb-drop-target');
    setDragTip('into', `Move to “${crumbLabel(intent.destPath)}”`);
    return;
  }
  const name = intent.row.dataset.name;
  if (intent.zone === 'into') {
    intent.row.classList.add('drop-target');
    setDragTip('into', `Move into “${name}”`);
  } else {
    intent.row.classList.add('drop-' + intent.zone);
    setDragTip('order', `Move ${intent.zone === 'before' ? 'above' : 'below'} “${name}”`);
  }
}

function crumbLabel(path) {
  return path === '/' ? 'Home' : path.substring(path.lastIndexOf('/') + 1);
}

// The label beside the cursor. kind: 'order' (rearranging) or 'into'
// (moving somewhere else); null hides it.
function setDragTip(kind, text) {
  let el = document.getElementById('drag-tip');
  if (!kind) { el?.remove(); return; }
  if (!el) {
    el = document.createElement('div');
    el.id = 'drag-tip';
    document.body.appendChild(el);
  }
  el.dataset.kind = kind;
  el.textContent = text;
  // Below-right of the cursor, flipped back on screen near the edges.
  const pad = 8, w = el.offsetWidth, h = el.offsetHeight;
  let x = _drag.x + 16, y = _drag.y + 18;
  if (x + w > window.innerWidth  - pad) x = _drag.x - w - 12;
  if (y + h > window.innerHeight - pad) y = _drag.y - h - 12;
  el.style.left = Math.max(pad, x) + 'px';
  el.style.top  = Math.max(pad, y) + 'px';
}

async function performDrop(src, intent) {
  if (intent.zone === 'crumb') {
    const destFull = (intent.destPath === '/' ? '' : intent.destPath) + '/' + src.name;
    return moveEntry('/' + src.path, destFull, src.name, crumbLabel(intent.destPath));
  }
  const dst = { path: intent.row.dataset.path, name: intent.row.dataset.name };

  if (intent.zone !== 'into') return rearrange(src.name, dst.name, intent.zone);

  if (!src.isDir) {
    return moveEntry('/' + src.path, '/' + dst.path + '/' + src.name, src.name, dst.name);
  }
  // Folder onto folder is the one that used to bite: it could mean "nest
  // it" or "combine them", and it is the easiest drop to make by accident
  // while rearranging. Always ask.
  const choice = await askFolderDrop(src.name, dst.name);
  if (choice === 'inside') {
    await moveEntry('/' + src.path, '/' + dst.path + '/' + src.name, src.name, dst.name);
  } else if (choice === 'merge') {
    await mergeFolders('/' + src.path, '/' + dst.path, src.name, dst.name);
  }
}

// ── the three outcomes ───────────────────────────────────────────────────────

async function rearrange(srcName, dstName, zone) {
  const previous = currentOrder;
  const names = currentItems.map(i => i.name).filter(n => n !== srcName);
  const at = names.indexOf(dstName);
  if (at < 0) return;
  names.splice(zone === 'before' ? at : at + 1, 0, srcName);

  if (!await saveOrder(names)) return;
  renderFileList(orderItems(currentItems, names));
  showToast(`Moved “${srcName}” ${zone === 'before' ? 'above' : 'below'} “${dstName}”`, {
    undo: async () => {
      if (await saveOrder(previous)) renderFileList(orderItems(currentItems, previous));
    }
  });
}

// Move one entry to a new path, with Undo. Used by drops on a folder row
// and on the path bar.
async function moveEntry(srcFull, destFull, name, destLabel) {
  if (srcFull === destFull) return;
  const where = currentPath;
  const order = currentOrder;
  const data  = await post('/api/rename', { from: srcFull, to: destFull });
  if (!data) return;
  if (data.error) {
    showToast(`Couldn't move “${name}”: ${data.error}`, { error: true });
    return;
  }
  await navigate(currentPath);
  showToast(`Moved “${name}” into “${destLabel}”`, {
    undo: async () => {
      const back = await post('/api/rename', { from: destFull, to: srcFull });
      if (!back || back.error) {
        showToast(`Couldn't undo: ${back?.error || 'request failed'}`, { error: true });
        return;
      }
      // Moving out dropped it from this folder's saved order; put the list
      // back so it returns to the slot it left, not the end.
      if (order.length && currentPath === where) await saveOrder(order);
      await navigate(currentPath);
    }
  });
}
async function mergeFolders(srcFull, destFull, srcName, destName) {
  const data = await post('/api/merge', { from: srcFull, to: destFull });
  if (!data) return;
  await navigate(currentPath);
  if (data.error) {
    // The server stops at the first failure and leaves the rest in place.
    const partial = data.moved ? ` ${data.moved} item(s) were moved before it stopped;` +
                                 ` the rest are still in “${srcName}”.` : '';
    showToast(`Couldn't merge “${srcName}”: ${data.error}.${partial}`, { error: true });
    return;
  }
  const kept = (data.renamed || []).length;
  showToast(`Merged “${srcName}” into “${destName}”` +
            (kept ? ` — ${kept} same-named item${kept > 1 ? 's' : ''} kept as a copy` : ''));
}

// ── folder-on-folder dialog ──────────────────────────────────────────────────
// Resolves to 'inside', 'merge' or null (cancelled).
function askFolderDrop(srcName, destName) {
  return new Promise(resolve => {
    const overlay = document.createElement('div');
    overlay.id = 'move-overlay';
    overlay.innerHTML = `
      <div class="modal move-modal" role="dialog" aria-modal="true" aria-labelledby="move-title">
        <p id="move-title">Move “${escHtml(srcName)}” into “${escHtml(destName)}”?</p>
        <button type="button" class="move-choice" data-choice="inside">
          <span class="move-choice-title">Put it inside</span>
          <span class="move-choice-desc">“${escHtml(srcName)}” becomes a subfolder of “${escHtml(destName)}”.</span>
        </button>
        <button type="button" class="move-choice" data-choice="merge">
          <span class="move-choice-title">Merge contents</span>
          <span class="move-choice-desc">Everything in “${escHtml(srcName)}” moves into “${escHtml(destName)}”, then “${escHtml(srcName)}” is removed. Nothing is overwritten. Can’t be undone.</span>
        </button>
        <div class="modal-btns"><button type="button" data-choice="">Cancel</button></div>
        <p class="move-oops">Didn’t mean to do this? Press <kbd>Esc</kbd>, then hold <kbd>Shift</kbd> while dragging to rearrange instead.</p>
      </div>`;

    const finish = choice => {
      document.removeEventListener('keydown', onKey, true);
      overlay.remove();
      resolve(choice || null);
    };
    const onKey = e => {
      if (e.key === 'Escape') { e.preventDefault(); e.stopPropagation(); finish(null); }
    };
    overlay.addEventListener('click', e => {
      if (e.target === overlay) return finish(null);        // click on the scrim
      const btn = e.target.closest('[data-choice]');
      if (btn) finish(btn.dataset.choice);
    });
    document.addEventListener('keydown', onKey, true);
    document.body.appendChild(overlay);
    // Focus starts on Cancel: a stray Enter or Space after an accidental
    // drop must not move anything.
    overlay.querySelector('.modal-btns button').focus();
  });
}

// ── drag hint ────────────────────────────────────────────────────────────────
// A one-line legend shown only while a row is being dragged; the label by
// the cursor says what a release will do, this says how to change that.
// Pass null to hide it, otherwise whether Shift is currently held.
function setDragHint(shift) {
  let el = document.getElementById('drag-hint');
  if (shift === null) { el?.remove(); return; }
  if (!el) {
    el = document.createElement('div');
    el.id = 'drag-hint';
    document.body.appendChild(el);
  }
  const mode = shift ? 'shift' : 'normal';
  if (el.dataset.mode === mode) return;
  el.dataset.mode = mode;
  el.innerHTML = shift
    ? `<b>Rearranging only</b> — nothing goes into a folder while <kbd>Shift</kbd> is held · <kbd>Esc</kbd> cancels`
    : `Hold <kbd>Shift</kbd> to only rearrange · <kbd>Esc</kbd> cancels`;
}

// ── toast ────────────────────────────────────────────────────────────────────
// One at a time; a new one replaces the last. opts.undo adds an Undo button,
// opts.error styles it as a failure and keeps it up a little longer.
let _toastTimer = null;
function showToast(message, opts = {}) {
  document.getElementById('toast')?.remove();
  clearTimeout(_toastTimer);

  const el = document.createElement('div');
  el.id = 'toast';
  el.setAttribute('role', opts.error ? 'alert' : 'status');
  if (opts.error) el.classList.add('error');
  const text = document.createElement('span');
  text.textContent = message;
  el.appendChild(text);

  if (opts.undo) {
    const btn = document.createElement('button');
    btn.type = 'button';
    btn.textContent = 'Undo';
    btn.addEventListener('click', async () => {
      el.remove();
      clearTimeout(_toastTimer);
      await opts.undo();
    });
    el.appendChild(btn);
  }
  document.body.appendChild(el);
  _toastTimer = setTimeout(() => el.remove(), opts.error ? 9000 : opts.undo ? 8000 : 4000);
}
// ── row action icons ──────────────────────────────────────────────────────────
// Inline SVG rather than text glyphs: "↓" and "✕" render at whatever weight
// the fallback font happens to have, which is how the download arrow ended up
// a hairline. These stroke in currentColor, so the button's colour rules in
// style.css still drive them in both themes.
const ICON_DOWNLOAD = `<svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.7" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M8 2.5v7.5"/><path d="M4.75 7 8 10.25 11.25 7"/><path d="M2.75 11v1.5a1 1 0 0 0 1 1h8.5a1 1 0 0 0 1-1V11"/></svg>`;
const ICON_TRASH = `<svg viewBox="0 0 16 16" width="14" height="14" fill="none" stroke="currentColor" stroke-width="1.5" stroke-linecap="round" stroke-linejoin="round" aria-hidden="true"><path d="M2.75 4.25h10.5"/><path d="M6.25 4.25V3a.75.75 0 0 1 .75-.75h2a.75.75 0 0 1 .75.75v1.25"/><path d="M4 4.25l.6 8.35a1 1 0 0 0 1 .9h4.8a1 1 0 0 0 1-.9l.6-8.35"/><path d="M6.75 6.75v4M9.25 6.75v4"/></svg>`;

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
  dragCounter = Math.max(0, dragCounter - 1);
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
    // Fetched well past what the card shows so "Show more" has a full list
    // without a second round trip. Still a small response, and it's the same
    // poll that was already running every 15s.
    get('/api/events?limit=50')
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

  // Offenders first, and not just for display order: they sit in the grid
  // row *above* the feed, so until they're populated the feed card measures
  // its own top against a page that's still short and overestimates how many
  // rows fit. Filling them first makes that measurement final.
  renderOffenders('overview-top-ips', ipSummary?.items || [], 'source_ip');
  renderOffenders('overview-top-actors', actorSummary?.items || [], 'actor');

  // Both row-2 panels are capped to what fits, so neither can push the page
  // into scrolling. The full alert list is one click away in the Alerts tab.
  const rows = feedRowsThatFit();
  renderOverviewAlerts(recentAlerts?.items || [], rows);
  renderOverviewFeed(recentEvents?.items || [], rows);
}

function renderOverviewAlerts(alerts, maxRows) {
  const el = document.getElementById('overview-recent-alerts');
  if (!alerts.length) {
    el.innerHTML = `<div class="dash-empty">No open alerts</div>`; return;
  }
  el.innerHTML = alerts.slice(0, maxRows || alerts.length).map(a => `
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

// The inline feed renders however many rows fit in the space left below it,
// so the Overview never pushes the page into scrolling — the rest go behind
// "Show more". A fixed count can't work here: at a 900px viewport only 4
// rows fit, at 1000px it's 6.
const FEED_ROWS_MIN = 3;
const FEED_ROWS_MAX = 12;
// Space below the card to leave clear — .dash-screen's 60px bottom padding
// plus a little breathing room.
const FEED_BOTTOM_GAP = 68;
const FEED_ROW_H_FALLBACK = 55;

// Measured from a real row once one exists, so this tracks .mini-row's
// padding/line-heights instead of silently drifting from a hardcoded guess.
let feedRowH = 0;

// Full set from the last poll, so opening the modal doesn't re-fetch.
let lastFeedEvents = [];

function feedRowsThatFit() {
  const body = document.getElementById('overview-live-feed');
  const card = body?.closest('.dash-card');
  if (!card) return FEED_ROWS_MIN;

  // Everything above this card is fixed-height (stat cards, and the
  // offender panels, which are capped at 5 rows by the API limit), so the
  // card's own top doesn't move with the number of rows we render — no
  // feedback loop between this measurement and its result.
  const docTop  = card.getBoundingClientRect().top + window.scrollY;
  const headerH = card.querySelector('.dash-card-header').offsetHeight;
  const avail   = window.innerHeight - docTop - headerH - FEED_BOTTOM_GAP;

  const rows = Math.floor(avail / (feedRowH || FEED_ROW_H_FALLBACK));
  return Math.max(FEED_ROWS_MIN, Math.min(FEED_ROWS_MAX, rows));
}

function feedRowHtml(e) {
  const dotCls = e.result === 'failure' ? 'failure' : 'success';
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
}

function renderOverviewFeed(events, maxRows) {
  const el      = document.getElementById('overview-live-feed');
  const moreBtn = document.getElementById('feed-show-more');
  lastFeedEvents = events;

  if (!events.length) {
    el.innerHTML = `<div class="dash-empty">No recent events</div>`;
    moreBtn.classList.add('hidden');
    return;
  }

  const shown = maxRows || feedRowsThatFit();
  el.innerHTML = events.slice(0, shown).map(feedRowHtml).join('');

  // Cache the real row height off the first render so later calls size
  // against the actual CSS rather than the fallback constant.
  const firstRow = el.querySelector('.mini-row');
  if (firstRow) feedRowH = firstRow.getBoundingClientRect().height || feedRowH;

  // Only offer "Show more" when there is actually more — otherwise the pill
  // is a button that opens the same list you're already looking at.
  const hidden = events.length - shown;
  moreBtn.classList.toggle('hidden', hidden <= 0);
  if (hidden > 0) moreBtn.textContent = `Show more (${hidden})`;

  // Keep the open modal current with the poll rather than freezing it at
  // whatever was on screen when it opened.
  if (!document.getElementById('feed-overlay').classList.contains('hidden')) {
    renderFeedModal();
  }
}

function renderFeedModal() {
  document.getElementById('feed-modal-body').innerHTML =
    lastFeedEvents.length
      ? lastFeedEvents.map(feedRowHtml).join('')
      : `<div class="dash-empty">No recent events</div>`;
}

function openFeedModal() {
  renderFeedModal();
  document.getElementById('feed-overlay').classList.remove('hidden');
}

function closeFeedModal() {
  document.getElementById('feed-overlay').classList.add('hidden');
}

document.getElementById('feed-show-more').addEventListener('click', openFeedModal);
document.getElementById('feed-modal-close').addEventListener('click', closeFeedModal);
// Click the backdrop to dismiss, but not a click that started inside the
// panel — otherwise dragging to select text and releasing outside closes it.
document.getElementById('feed-overlay').addEventListener('click', e => {
  if (e.target.id === 'feed-overlay') closeFeedModal();
});
document.addEventListener('keydown', e => {
  if (e.key === 'Escape') closeFeedModal();
});

// Re-fit the inline feed when the window changes size, so the "no page
// scroll" property holds after a resize instead of only at load.
let feedResizeTimer = null;
window.addEventListener('resize', () => {
  clearTimeout(feedResizeTimer);
  feedResizeTimer = setTimeout(() => {
    if (activeTab === 'overview' && lastFeedEvents.length) {
      renderOverviewFeed(lastFeedEvents);
    }
  }, 150);
});

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
    // ETA is only meaningful once the engine has timed a few files; it
    // reports 0 until then rather than guessing, so don't render "~0m".
    const eta = data.eta_seconds > 0
      ? ` — ~${Math.ceil(data.eta_seconds / 60)}m remaining` : '';
    btn.setAttribute('data-tip',
      `Syncing — ${data.percent_complete}% — ${data.files_copied} copied${eta}`);
  } else if (data.state === 'paused') {
    btn.setAttribute('data-tip', `Sync paused — ${data.percent_complete}% complete`);
  } else if (data.state === 'error') {
    btn.setAttribute('data-tip', 'Problem during sync — click to see logs');
  } else if (data.state === 'hash_mismatch') {
    btn.setAttribute('data-tip', 'Storage hash mismatch — click to see logs');
  } else {
    const checked = data.last_hash_check_utc ? relTime(data.last_hash_check_utc) : '—';
    const last = data.last_run_utc
      ? ` — last backup ${relTime(data.last_run_utc)} (${data.last_run_files_copied} copied)`
      : ' — no backup run yet';
    btn.setAttribute('data-tip', `Idle — hash verified ${checked}${last}`);
  }
}

document.getElementById('sync-btn').addEventListener('click', async () => {
  if (lastSyncState === 'error' || lastSyncState === 'hash_mismatch') {
    openSyncLogsPanel();
    return;
  }
  // Normal operation (idle/syncing/paused) — open the sync portal. NGINX
  // serves it from /sync/ on this same origin, so it inherits the
  // dashboard's TLS and needs no port lookup. This replaces the stopgap
  // listener's daily-rotating plaintext port, which is why the scheme is no
  // longer forced to http:// — a same-origin relative path can't get the
  // protocol wrong.
  window.open('/sync/', '_blank');
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

// ── init ──────────────────────────────────────────────────────────────────────
// Deliberately the last statement in the file. showApp() reaches
// startPolling(), which touches `let pollTimer` — declared ~370 lines below
// where this call used to sit. Running it there threw "Cannot access
// 'pollTimer' before initialization" on any load that already had a session
// token (i.e. every refresh while signed in), and because that throw is at
// module scope it aborted the rest of app.js: every listener and `const`
// below the call site silently never initialised.
if (token) showApp();
