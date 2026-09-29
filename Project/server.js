/*
 * server.js - entry point.  Run with:  npm start   (see README.md)
 */
const path = require('path');
const express = require('express');
const session = require('express-session');

const { ensureDataFiles } = require('./lib/userStore');
const { requireAuthPage } = require('./middleware/auth');
const authRoutes = require('./routes/auth');
const fileRoutes = require('./routes/files');

ensureDataFiles(); // creates data/users.json and data/users/ on first run

const app = express();
const PORT = process.env.PORT || 3000;
const SESSION_SECRET = process.env.SESSION_SECRET || 'dev-only-secret-change-me';

if (!process.env.SESSION_SECRET) {
  console.warn('[warn] SESSION_SECRET is not set (using an insecure default). See .env.example.');
}

app.use(express.json());
app.use(session({
  secret: SESSION_SECRET,
  resave: false,
  saveUninitialized: false,
  cookie: { httpOnly: true, maxAge: 1000 * 60 * 60 * 4 }, // 4 hour session
}));

// --- API routes ---
app.use('/api/auth', authRoutes);
app.use('/api/files', fileRoutes);

// --- Protected page: must be logged in to reach the dashboard ---
app.get('/dashboard.html', requireAuthPage, (req, res) => {
  res.sendFile(path.join(__dirname, 'public', 'dashboard.html'));
});

// --- Static assets (login page, signup page, css, js) ---
app.use(express.static(path.join(__dirname, 'public')));

app.get('/', (req, res) => {
  res.redirect(req.session && req.session.userId ? '/dashboard.html' : '/login.html');
});

app.listen(PORT, () => {
  console.log(`mmfms-web running at http://localhost:${PORT}`);
});
