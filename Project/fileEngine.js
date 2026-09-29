/*
 * fileEngine.js
 * ---------------------------------------------------------------------------
 * This module does for the website what mmfms.c's commands (open / read /
 * write / append / search / info) do for the terminal program: it operates
 * on a file at a byte OFFSET instead of only sequentially.
 *
 * HOW THIS RELATES TO THE C PROGRAM (be accurate about this in your report):
 *   - mmfms.c uses mmap() -> the file's bytes appear directly in the
 *     process's virtual memory; reads/writes are plain memory access with
 *     NO system call per byte.
 *   - This Node.js backend uses fs.readSync()/fs.writeSync() WITH a
 *     position argument (the POSIX pread()/pwrite() system calls under the
 *     hood). That still gives true random-access, offset-based I/O without
 *     reading the whole file first - the same idea mmap demonstrates - but
 *     each call is still one system call, not a raw memory access.
 *   - Real mmap() is a C-level OS call. The included native/mmfms.c is the
 *     actual mmap() program; this file is the web layer your professor
 *     asked for, with per-user accounts and separate storage.
 * ---------------------------------------------------------------------------
 */
const fs = require('fs');
const path = require('path');

const PAGE_SIZE = 4096; // typical Linux page size, used only for display/stats

// Only allow simple filenames: letters, numbers, dot, dash, underscore.
// This blocks path traversal like "../../etc/passwd".
const FILENAME_RE = /^[a-zA-Z0-9._-]{1,128}$/;

function isValidFilename(name) {
  return typeof name === 'string' && FILENAME_RE.test(name) && !name.startsWith('.');
}

function resolveSafe(userDir, filename) {
  if (!isValidFilename(filename)) throw httpError(400, 'Invalid file name.');
  const full = path.join(userDir, filename);
  // Defence in depth: make sure the resolved path is still inside the user's folder.
  if (!full.startsWith(path.resolve(userDir) + path.sep) && full !== path.resolve(userDir)) {
    throw httpError(400, 'Invalid file path.');
  }
  return full;
}

function httpError(status, message) {
  const e = new Error(message);
  e.status = status;
  return e;
}

function logActivity(userDir, line) {
  const logPath = path.join(userDir, '.activity.log');
  const ts = new Date().toISOString();
  fs.appendFileSync(logPath, `[${ts}] ${line}\n`);
}

function listFiles(userDir) {
  return fs.readdirSync(userDir)
    .filter(f => !f.startsWith('.'))
    .map(f => {
      const st = fs.statSync(path.join(userDir, f));
      return {
        name: f,
        size: st.size,
        pages: Math.ceil(st.size / PAGE_SIZE) || 0,
        modified: st.mtime.toISOString(),
      };
    })
    .sort((a, b) => a.name.localeCompare(b.name));
}

// Mirrors the C program's `create <file> <size>` command: fills the file
// with repeating sample lines up to the requested size.
function createSampleFile(userDir, filename, sizeBytes) {
  const full = resolveSafe(userDir, filename);
  if (fs.existsSync(full)) throw httpError(409, 'A file with that name already exists.');
  if (sizeBytes <= 0 || sizeBytes > 50 * 1024 * 1024) throw httpError(400, 'Size must be between 1 byte and 50 MB.');

  const fd = fs.openSync(full, 'wx');
  let written = 0, lineNo = 0;
  const chunks = [];
  let bufLen = 0;
  try {
    while (written < sizeBytes) {
      const line = `Line ${String(lineNo++).padStart(10, '0')} | mmfms-web demo data | the quick brown fox jumps over the lazy dog\n`;
      const remaining = sizeBytes - written;
      const take = Buffer.byteLength(line) <= remaining ? line : line.slice(0, remaining - 1) + '\n';
      chunks.push(take);
      bufLen += Buffer.byteLength(take);
      written += Buffer.byteLength(take);
      if (bufLen > 65536 || written >= sizeBytes) {
        fs.writeSync(fd, Buffer.from(chunks.join('')));
        chunks.length = 0;
        bufLen = 0;
      }
    }
  } finally {
    fs.closeSync(fd);
  }
  logActivity(userDir, `CREATE ${filename} (${sizeBytes} bytes)`);
  return { name: filename, size: sizeBytes };
}

function createFromText(userDir, filename, content) {
  const full = resolveSafe(userDir, filename);
  if (fs.existsSync(full)) throw httpError(409, 'A file with that name already exists.');
  fs.writeFileSync(full, content ?? '');
  logActivity(userDir, `CREATE ${filename} (${Buffer.byteLength(content ?? '')} bytes, custom text)`);
  return { name: filename, size: Buffer.byteLength(content ?? '') };
}

function getInfo(userDir, filename) {
  const full = resolveSafe(userDir, filename);
  if (!fs.existsSync(full)) throw httpError(404, 'File not found.');
  const st = fs.statSync(full);
  return {
    name: filename,
    size: st.size,
    pageSize: PAGE_SIZE,
    pages: Math.ceil(st.size / PAGE_SIZE) || 0,
    created: st.birthtime.toISOString(),
    modified: st.mtime.toISOString(),
  };
}

// Random-access read at a byte offset (fs.readSync with `position` = pread()).
function readRange(userDir, filename, offset, length) {
  const full = resolveSafe(userDir, filename);
  if (!fs.existsSync(full)) throw httpError(404, 'File not found.');
  const st = fs.statSync(full);
  if (offset < 0 || offset > st.size) throw httpError(400, `Offset out of range (file is ${st.size} bytes).`);
  const len = Math.max(0, Math.min(length, st.size - offset, 8192)); // capped for the UI

  const fd = fs.openSync(full, 'r');
  const buf = Buffer.alloc(len);
  try {
    fs.readSync(fd, buf, 0, len, offset); // <-- pread(): read at `offset` without moving a shared cursor
  } finally {
    fs.closeSync(fd);
  }
  return {
    offset, length: len, fileSize: st.size,
    hex: buf.toString('hex'),
    text: buf.toString('utf8').replace(/[^\x20-\x7e\n\t]/g, '.'),
  };
}

// Overwrite bytes in place at a byte offset (fs.writeSync with `position` = pwrite()).
function writeRange(userDir, filename, offset, text) {
  const full = resolveSafe(userDir, filename);
  if (!fs.existsSync(full)) throw httpError(404, 'File not found.');
  const st = fs.statSync(full);
  const data = Buffer.from(text ?? '', 'utf8');
  if (offset < 0 || offset > st.size) throw httpError(400, `Offset out of range (file is ${st.size} bytes).`);
  if (offset + data.length > st.size) throw httpError(400, 'Write would go past end of file. Use append to grow the file.');

  const fd = fs.openSync(full, 'r+');
  try {
    fs.writeSync(fd, data, 0, data.length, offset); // <-- pwrite(): write at `offset` in place
  } finally {
    fs.closeSync(fd);
  }
  logActivity(userDir, `WRITE ${filename} @${offset} (${data.length} bytes)`);
  return { offset, length: data.length };
}

function appendText(userDir, filename, text) {
  const full = resolveSafe(userDir, filename);
  if (!fs.existsSync(full)) throw httpError(404, 'File not found.');
  fs.appendFileSync(full, text ?? '');
  const st = fs.statSync(full);
  logActivity(userDir, `APPEND ${filename} (+${Buffer.byteLength(text ?? '')} bytes)`);
  return { newSize: st.size };
}

function searchText(userDir, filename, query) {
  const full = resolveSafe(userDir, filename);
  if (!fs.existsSync(full)) throw httpError(404, 'File not found.');
  if (!query) throw httpError(400, 'Search text is required.');

  const content = fs.readFileSync(full, 'utf8');
  const matches = [];
  let idx = 0, line = 1, lastPos = 0;
  while (matches.length < 25) {
    const pos = content.indexOf(query, idx);
    if (pos === -1) break;
    for (let i = lastPos; i < pos; i++) if (content[i] === '\n') line++;
    lastPos = pos;
    const eol = content.indexOf('\n', pos);
    const preview = content.slice(pos, eol === -1 ? pos + 60 : Math.min(eol, pos + 60));
    matches.push({ offset: pos, line, preview });
    idx = pos + query.length;
  }
  // Count any remaining matches beyond the 25 shown, without storing them all.
  let total = matches.length, scan = idx;
  while (total < 100000) {
    const p = content.indexOf(query, scan);
    if (p === -1) break;
    total++;
    scan = p + query.length;
  }
  logActivity(userDir, `SEARCH ${filename} "${query}" (${total} match${total === 1 ? '' : 'es'})`);
  return { matches, totalCount: total, truncated: total > matches.length };
}

function deleteFile(userDir, filename) {
  const full = resolveSafe(userDir, filename);
  if (!fs.existsSync(full)) throw httpError(404, 'File not found.');
  fs.unlinkSync(full);
  logActivity(userDir, `DELETE ${filename}`);
}

function getActivityLog(userDir, limit = 50) {
  const logPath = path.join(userDir, '.activity.log');
  if (!fs.existsSync(logPath)) return [];
  const lines = fs.readFileSync(logPath, 'utf8').trim().split('\n').filter(Boolean);
  return lines.slice(-limit).reverse();
}

module.exports = {
  isValidFilename, resolveSafe, listFiles, createSampleFile, createFromText,
  getInfo, readRange, writeRange, appendText, searchText, deleteFile, getActivityLog,
};
