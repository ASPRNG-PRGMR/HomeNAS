// ── state ─────────────────────────────────────────────────────────────────────
let token    = sessionStorage.getItem('nas_token') || null;
let currentPath = '/';

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
    opts.body = body; // FormData
  }
  const resp = await fetch(path, opts);
  if (resp.status === 401) { signOut(); return null; }
  return resp.json();
}

function get(path)         { return api('GET',    path); }
function post(path, body)  { return api('POST',   path, body); }
function del(path)         { return api('DELETE', path); }

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
  token = null;
  sessionStorage.removeItem('nas_token');
  document.getElementById('app-screen').classList.add('hidden');
  document.getElementById('login-screen').classList.remove('hidden');
}

// ── init ──────────────────────────────────────────────────────────────────────
if (token) {
  showApp();
} 

function showApp() {
  document.getElementById('login-screen').classList.add('hidden');
  document.getElementById('app-screen').classList.remove('hidden');
  navigate('/');
}

// ── navigation ────────────────────────────────────────────────────────────────
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
    html += `<span class="sep">/</span><span class="crumb ${active ? 'active' : ''}" data-path="${built}">${p}</span>`;
  });
  nav.innerHTML = html;
  nav.querySelectorAll('.crumb:not(.active)').forEach(el => {
    el.addEventListener('click', () => navigate(el.dataset.path));
  });
}

// ── file list ─────────────────────────────────────────────────────────────────
function renderFileList(items) {
  const main = document.getElementById('file-list');

  // Sort: dirs first, then alpha
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
      <div></div>
      <div>Name</div>
      <div>Size</div>
      <div>Modified</div>
      <div></div>
    </div>`;

  const rows = items.map(item => {
    const icon     = item.is_dir ? '📁' : fileIcon(item.name);
    const size     = item.is_dir ? '—' : formatBytes(item.size);
    const modified = formatDate(item.modified);

    return `
      <div class="file-row" data-path="${item.path}" data-dir="${item.is_dir}" data-name="${escHtml(item.name)}">
        <div class="file-icon">${icon}</div>
        <div class="file-name">${escHtml(item.name)}</div>
        <div class="file-size">${size}</div>
        <div class="file-date">${modified}</div>
        <div class="file-actions">
          ${!item.is_dir ? `<button class="dl-btn">↓</button>` : ''}
          <button class="ren-btn">✎</button>
          <button class="del del-btn">✕</button>
        </div>
      </div>`;
  }).join('');

  main.innerHTML = header + rows;

  main.querySelectorAll('.file-row').forEach(row => {
    const path  = row.dataset.path;
    const isDir = row.dataset.dir === 'true';
    const name  = row.dataset.name;

    // Click to navigate or download
    row.addEventListener('click', e => {
      if (e.target.closest('.file-actions')) return;
      if (isDir) navigate('/' + path);
      else downloadFile(path, name);
    });

    // Download button
    row.querySelector('.dl-btn')?.addEventListener('click', e => {
      e.stopPropagation();
      downloadFile(path, name);
    });

    // Rename button
    row.querySelector('.ren-btn').addEventListener('click', e => {
      e.stopPropagation();
      showRenameModal(path, name);
    });

    // Delete button
    row.querySelector('.del-btn').addEventListener('click', async e => {
      e.stopPropagation();
      if (!confirm(`Delete "${name}"?`)) return;
      await del(`/api/file?path=${encodeURIComponent('/' + path)}`);
      navigate(currentPath);
    });
  });
}

// ── download ──────────────────────────────────────────────────────────────────
function downloadFile(path, name) {
  // Use anchor with Authorization header via fetch → blob
  fetch(`/api/file?path=${encodeURIComponent('/' + path)}`, {
    headers: { 'Authorization': `Bearer ${token}` }
  }).then(r => r.blob()).then(blob => {
    const url = URL.createObjectURL(blob);
    const a   = document.createElement('a');
    a.href    = url;
    a.download = name;
    a.click();
    URL.revokeObjectURL(url);
  });
}

// ── upload ────────────────────────────────────────────────────────────────────
document.getElementById('upload-btn').addEventListener('click', () => {
  document.getElementById('file-input').click();
});

document.getElementById('file-input').addEventListener('change', e => {
  uploadFiles(e.target.files);
  e.target.value = '';
});

// Drag and drop
const appScreen = document.getElementById('app-screen');
const dropOverlay = document.getElementById('drop-overlay');
let dragCounter = 0;

appScreen.addEventListener('dragenter', e => {
  e.preventDefault();
  dragCounter++;
  dropOverlay.classList.remove('hidden');
});
appScreen.addEventListener('dragleave', () => {
  dragCounter--;
  if (dragCounter === 0) dropOverlay.classList.add('hidden');
});
appScreen.addEventListener('dragover', e => e.preventDefault());
appScreen.addEventListener('drop', e => {
  e.preventDefault();
  dragCounter = 0;
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

  ok.onclick = () => { cleanup(); onOk(input.value); };
  cancel.onclick = cleanup;
  input.onkeydown = e => {
    if (e.key === 'Enter')  { cleanup(); onOk(input.value); }
    if (e.key === 'Escape') cleanup();
  };
}

// ── utils ─────────────────────────────────────────────────────────────────────
function formatBytes(n) {
  if (n === undefined) return '—';
  if (n < 1024)        return n + ' B';
  if (n < 1048576)     return (n / 1024).toFixed(1) + ' KB';
  if (n < 1073741824)  return (n / 1048576).toFixed(1) + ' MB';
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
  return s.replace(/&/g,'&amp;').replace(/</g,'&lt;').replace(/>/g,'&gt;').replace(/"/g,'&quot;');
}
