let selectedFile = null;

async function api(path, opts = {}) {
  const res = await fetch(path, {
    headers: { 'Content-Type': 'application/json' },
    ...opts,
  });
  const data = await res.json().catch(() => ({}));
  if (!res.ok) throw new Error(data.error || 'Request failed');
  return data;
}

async function loadWhoAmI() {
  try {
    const me = await api('/api/auth/me');
    document.getElementById('whoami').textContent = `Logged in as ${me.username}`;
  } catch {
    window.location.href = '/login.html';
  }
}

async function loadFiles() {
  const { files } = await api('/api/files');
  const list = document.getElementById('fileList');
  list.innerHTML = '';
  if (files.length === 0) {
    list.innerHTML = '<li class="small">No files yet — create one below.</li>';
  }
  files.forEach(f => {
    const li = document.createElement('li');
    li.className = f.name === selectedFile ? 'active' : '';
    li.innerHTML = `<span>${f.name}</span><span class="size">${f.size} B</span>`;
    li.addEventListener('click', () => selectFile(f.name));
    list.appendChild(li);
  });
}

async function selectFile(name) {
  selectedFile = name;
  document.getElementById('noFilePanel').style.display = 'none';
  document.getElementById('fileOpsPanel').style.display = 'block';
  document.getElementById('selName').textContent = name;
  document.getElementById('downloadLink').href = `/api/files/${encodeURIComponent(name)}/download`;
  await Promise.all([loadFiles(), loadInfo(), loadActivity()]);
}

async function loadInfo() {
  if (!selectedFile) return;
  const info = await api(`/api/files/${encodeURIComponent(selectedFile)}/info`);
  document.getElementById('fileInfo').innerHTML =
    `Size: ${info.size} bytes &nbsp;•&nbsp; Pages (${info.pageSize} B each): ${info.pages} &nbsp;•&nbsp; Modified: ${new Date(info.modified).toLocaleString()}`;
}

async function loadActivity() {
  const { activity } = await api('/api/files/_activity/log');
  document.getElementById('activityOutput').textContent = activity.length ? activity.join('\n') : '(no activity yet)';
}

// ---- Tabs ----
document.querySelectorAll('.tabbtn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.tabbtn').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.tabpane').forEach(p => p.classList.remove('active'));
    btn.classList.add('active');
    document.querySelector(`.tabpane[data-pane="${btn.dataset.tab}"]`).classList.add('active');
  });
});

// ---- Create file ----
document.getElementById('newMode').addEventListener('change', (e) => {
  const isSample = e.target.value === 'sample';
  document.getElementById('sizeRow').style.display = isSample ? 'block' : 'none';
  document.getElementById('contentRow').style.display = isSample ? 'none' : 'block';
});

document.getElementById('createBtn').addEventListener('click', async () => {
  const name = document.getElementById('newName').value.trim();
  const mode = document.getElementById('newMode').value;
  const msg = document.getElementById('createMsg');
  msg.textContent = '';
  try {
    const body = mode === 'sample'
      ? { name, mode, size: document.getElementById('newSize').value }
      : { name, mode, content: document.getElementById('newContent').value };
    await api('/api/files', { method: 'POST', body: JSON.stringify(body) });
    msg.className = 'msg ok';
    msg.textContent = `Created '${name}'.`;
    document.getElementById('newName').value = '';
    await selectFile(name);
  } catch (err) {
    msg.className = 'msg error';
    msg.textContent = err.message;
  }
});

// ---- Read ----
document.getElementById('readBtn').addEventListener('click', async () => {
  const out = document.getElementById('readOutput');
  try {
    const offset = document.getElementById('readOffset').value;
    const length = document.getElementById('readLength').value;
    const r = await api(`/api/files/${encodeURIComponent(selectedFile)}/read?offset=${offset}&length=${length}`);
    out.textContent = `offset ${r.offset}  length ${r.length}  (file size ${r.fileSize})\n\nHEX:\n${r.hex}\n\nTEXT:\n${r.text}`;
  } catch (err) {
    out.textContent = 'Error: ' + err.message;
  }
});

// ---- Write ----
document.getElementById('writeBtn').addEventListener('click', async () => {
  const out = document.getElementById('writeOutput');
  try {
    const offset = document.getElementById('writeOffset').value;
    const text = document.getElementById('writeText').value;
    const r = await api(`/api/files/${encodeURIComponent(selectedFile)}/write`, {
      method: 'POST', body: JSON.stringify({ offset, text }),
    });
    out.textContent = `Wrote ${r.length} byte(s) at offset ${r.offset}.`;
    await Promise.all([loadInfo(), loadActivity()]);
  } catch (err) {
    out.textContent = 'Error: ' + err.message;
  }
});

// ---- Append ----
document.getElementById('appendBtn').addEventListener('click', async () => {
  const out = document.getElementById('appendOutput');
  try {
    const text = document.getElementById('appendText').value;
    const r = await api(`/api/files/${encodeURIComponent(selectedFile)}/append`, {
      method: 'POST', body: JSON.stringify({ text }),
    });
    out.textContent = `Appended. File is now ${r.newSize} bytes.`;
    await Promise.all([loadInfo(), loadFiles(), loadActivity()]);
  } catch (err) {
    out.textContent = 'Error: ' + err.message;
  }
});

// ---- Search ----
document.getElementById('searchBtn').addEventListener('click', async () => {
  const out = document.getElementById('searchOutput');
  try {
    const q = document.getElementById('searchText').value;
    const r = await api(`/api/files/${encodeURIComponent(selectedFile)}/search?q=${encodeURIComponent(q)}`);
    if (r.matches.length === 0) { out.textContent = 'No matches.'; return; }
    const lines = r.matches.map(m => `offset ${String(m.offset).padEnd(8)} line ${String(m.line).padEnd(6)} ${m.preview}`);
    out.textContent = `Found ${r.totalCount} match(es)${r.truncated ? ' (first 25 shown)' : ''}:\n\n` + lines.join('\n');
    await loadActivity();
  } catch (err) {
    out.textContent = 'Error: ' + err.message;
  }
});

// ---- Delete ----
document.getElementById('deleteBtn').addEventListener('click', async () => {
  if (!confirm(`Delete '${selectedFile}'? This cannot be undone.`)) return;
  try {
    await api(`/api/files/${encodeURIComponent(selectedFile)}`, { method: 'DELETE' });
    selectedFile = null;
    document.getElementById('fileOpsPanel').style.display = 'none';
    document.getElementById('noFilePanel').style.display = 'block';
    await loadFiles();
  } catch (err) {
    alert('Error: ' + err.message);
  }
});

// ---- Logout ----
document.getElementById('logoutBtn').addEventListener('click', async () => {
  await api('/api/auth/logout', { method: 'POST' });
  window.location.href = '/login.html';
});

// ---- Init ----
(async function init() {
  await loadWhoAmI();
  await loadFiles();
})();
