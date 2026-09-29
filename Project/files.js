const express = require('express');
const { userDir } = require('../lib/userStore');
const engine = require('../lib/fileEngine');
const { requireAuthApi } = require('../middleware/auth');

const router = express.Router();
router.use(requireAuthApi); // every route below requires a logged-in user

// Every handler uses req.session.username to find THIS user's own folder only -
// that's what keeps one user's files separate from every other user's files.
function dirFor(req) {
  return userDir(req.session.username);
}

function handle(fn) {
  return (req, res) => {
    try {
      res.json(fn(req));
    } catch (err) {
      res.status(err.status || 500).json({ error: err.message || 'Server error.' });
    }
  };
}

router.get('/', handle(req => ({ files: engine.listFiles(dirFor(req)) })));

router.post('/', handle(req => {
  const { name, mode, size, content } = req.body || {};
  if (mode === 'sample') {
    return engine.createSampleFile(dirFor(req), name, parseInt(size, 10) || 0);
  }
  return engine.createFromText(dirFor(req), name, content || '');
}));

router.get('/:name/info', handle(req => engine.getInfo(dirFor(req), req.params.name)));

router.get('/:name/read', handle(req => {
  const offset = parseInt(req.query.offset, 10) || 0;
  const length = parseInt(req.query.length, 10) || 256;
  return engine.readRange(dirFor(req), req.params.name, offset, length);
}));

router.post('/:name/write', handle(req => {
  const { offset, text } = req.body || {};
  return engine.writeRange(dirFor(req), req.params.name, parseInt(offset, 10) || 0, text || '');
}));

router.post('/:name/append', handle(req => {
  const { text } = req.body || {};
  return engine.appendText(dirFor(req), req.params.name, text || '');
}));

router.get('/:name/search', handle(req => engine.searchText(dirFor(req), req.params.name, req.query.q || '')));

router.delete('/:name', handle(req => { engine.deleteFile(dirFor(req), req.params.name); return { ok: true }; }));

router.get('/:name/download', (req, res) => {
  try {
    const full = engine.resolveSafe(dirFor(req), req.params.name);
    res.download(full, req.params.name);
  } catch (err) {
    res.status(err.status || 500).json({ error: err.message });
  }
});

router.get('/_activity/log', handle(req => ({ activity: engine.getActivityLog(dirFor(req)) })));

module.exports = router;
