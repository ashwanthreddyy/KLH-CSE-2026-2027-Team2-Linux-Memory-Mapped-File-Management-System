document.getElementById('loginForm').addEventListener('submit', async (e) => {
  e.preventDefault();
  const username = document.getElementById('username').value.trim();
  const password = document.getElementById('password').value;
  const msg = document.getElementById('msg');
  msg.textContent = '';

  try {
    const res = await fetch('/api/auth/login', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, password }),
    });
    const data = await res.json();
    if (!res.ok) {
      msg.className = 'msg error';
      msg.textContent = data.error || 'Login failed.';
      return;
    }
    window.location.href = '/dashboard.html';
  } catch (err) {
    msg.className = 'msg error';
    msg.textContent = 'Could not reach the server.';
  }
});
