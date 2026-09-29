/*
 * userStore.js
 * ---------------------------------------------------------------------------
 * A tiny file-based "database" for user accounts, so the project runs with
 * zero external database setup (no MySQL/Mongo install needed for a demo).
 *
 * All users live in one JSON file: data/users.json
 *      { "users": [ { id, username, passwordHash, createdAt }, ... ] }
 *
 * Each user also gets their OWN separate folder for their files:
 *      data/users/<username>/
 * That folder is what keeps every user's data completely separate from
 * every other user's data (this is the part your professor asked for).
 * ---------------------------------------------------------------------------
 */
const fs = require('fs');
const path = require('path');
const crypto = require('crypto');

const DATA_DIR   = path.join(__dirname, '..', 'data');
const USERS_FILE = path.join(DATA_DIR, 'users.json');
const USERS_ROOT = path.join(DATA_DIR, 'users');

function ensureDataFiles() {
  if (!fs.existsSync(DATA_DIR)) fs.mkdirSync(DATA_DIR, { recursive: true });
  if (!fs.existsSync(USERS_ROOT)) fs.mkdirSync(USERS_ROOT, { recursive: true });
  if (!fs.existsSync(USERS_FILE)) fs.writeFileSync(USERS_FILE, JSON.stringify({ users: [] }, null, 2));
}

function readDb() {
  ensureDataFiles();
  return JSON.parse(fs.readFileSync(USERS_FILE, 'utf8'));
}

function writeDb(db) {
  // Synchronous write: simple and safe for a small course project
  // (Node is single-threaded, so this can't race with itself).
  fs.writeFileSync(USERS_FILE, JSON.stringify(db, null, 2));
}

const USERNAME_RE = /^[a-zA-Z0-9_-]{3,32}$/;

function isValidUsername(name) {
  return typeof name === 'string' && USERNAME_RE.test(name);
}

function findByUsername(username) {
  const db = readDb();
  return db.users.find(u => u.username.toLowerCase() === String(username).toLowerCase()) || null;
}

function findById(id) {
  const db = readDb();
  return db.users.find(u => u.id === id) || null;
}

// Returns the absolute path of a user's private data folder, creating it if needed.
function userDir(username) {
  const dir = path.join(USERS_ROOT, username);
  if (!fs.existsSync(dir)) fs.mkdirSync(dir, { recursive: true });
  return dir;
}

function createUser(username, passwordHash) {
  const db = readDb();
  const user = {
    id: crypto.randomUUID(),
    username,
    passwordHash,
    createdAt: new Date().toISOString(),
  };
  db.users.push(user);
  writeDb(db);
  userDir(username); // create their separate storage folder right away
  return user;
}

module.exports = { findByUsername, findById, createUser, isValidUsername, userDir, ensureDataFiles };
