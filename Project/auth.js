const express = require('express');
const bcrypt = require('bcryptjs');
const { findByUsername, createUser, isValidUsername } = require('../lib/userStore');

const router = express.Router();

// POST /api/auth/signup  { username, password }
router.post('/signup', async (req, res) => {
  const { username, password } = req.body || {};

  if (!isValidUsername(username)) {
    return res.status(400).json({ error: 'Username must be 3-32 characters: letters, numbers, _ or -' });
  }
  if (typeof password !== 'string' || password.length < 6) {
    return res.status(400).json({ error: 'Password must be at least 6 characters.' });
  }
  if (findByUsername(username)) {
    return res.status(409).json({ error: 'That username is already taken.' });
  }

  const passwordHash = await bcrypt.hash(password, 10); // 10 = cost factor (salted automatically)
  const user = createUser(username, passwordHash);

  req.session.userId = user.id;
  req.session.username = user.username;
  res.status(201).json({ id: user.id, username: user.username });
});

// POST /api/auth/login  { username, password }
router.post('/login', async (req, res) => {
  const { username, password } = req.body || {};
  const user = findByUsername(username || '');
  if (!user) return res.status(401).json({ error: 'Invalid username or password.' });

  const ok = await bcrypt.compare(password || '', user.passwordHash);
  if (!ok) return res.status(401).json({ error: 'Invalid username or password.' });

  req.session.userId = user.id;
  req.session.username = user.username;
  res.json({ id: user.id, username: user.username });
});

// POST /api/auth/logout
router.post('/logout', (req, res) => {
  req.session.destroy(() => {
    res.clearCookie('connect.sid');
    res.json({ ok: true });
  });
});

// GET /api/auth/me  - who is currently logged in?
router.get('/me', (req, res) => {
  if (!req.session || !req.session.userId) return res.status(401).json({ error: 'Not logged in.' });
  res.json({ id: req.session.userId, username: req.session.username });
});

module.exports = router;
