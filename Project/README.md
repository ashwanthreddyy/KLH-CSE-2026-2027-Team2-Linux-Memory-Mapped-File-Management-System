# mmfms-web — Memory-Mapped File Management System (Web Edition)

A multi-user website version of the OSSP project. Each visitor can sign up,
log in, and manage their own text files through offset-based read / write /
append / search operations — the same operations the original C program
(`native/mmfms.c`) demonstrates with `mmap()`.

**Every user's files are stored in a separate folder** (`data/users/<username>/`),
so one account can never see or touch another account's data.

## How this relates to the C program

- `native/mmfms.c` (included here) is your original OS-level program. It uses
  the real `mmap()` / `msync()` / `munmap()` system calls — build and run it
  from a terminal for the actual kernel-level demonstration your OS course
  is about.
- This website is a **Node.js/Express backend** that performs the same kind
  of offset-based, random-access file I/O (`fs.readSync`/`fs.writeSync` with
  a byte position — the `pread()`/`pwrite()` system calls) plus accounts,
  sessions, and per-user storage, which is the multi-user web layer your
  professor asked for.

## Requirements

- [Node.js](https://nodejs.org) version 16 or newer (includes `npm`)
- VS Code (or any editor/terminal)

## Setup (run once)

Open a terminal in this folder and run:

```bash
npm install
cp .env.example .env
```

Open `.env` and change `SESSION_SECRET` to any random string (this is what
encrypts login sessions). `.env` is optional for local testing — the app
will still run without it, just with a warning.

## Run the website

```bash
npm start
```

Then open **http://localhost:3000** in your browser.

To stop the server, press `Ctrl+C` in the terminal.

## Using it

1. Go to **Sign up**, create an account (username + password).
2. You're taken to the **Dashboard**.
3. Create a file — either "Generate sample data" (like the C program's
   `create` command) or type your own text.
4. Select the file on the left, then use the tabs to:
   - **Read** — hex + text dump at a byte offset
   - **Write** — overwrite bytes in place at an offset
   - **Append** — grow the file with more text
   - **Search** — find all occurrences and their byte offsets / line numbers
   - **Delete / Download**
5. Log out and sign up with a **second account** in a private/incognito
   window to prove the two accounts' files are completely separate.

## Project structure

```
mmfms-web/
├── server.js              Express app entry point
├── routes/
│   ├── auth.js             signup / login / logout / who-am-i
│   └── files.js             per-user file operations API
├── middleware/auth.js       blocks routes/pages for logged-out users
├── lib/
│   ├── userStore.js         JSON-file user database + per-user folders
│   └── fileEngine.js        create / read / write / append / search / delete
├── public/                  front-end (plain HTML/CSS/JS, no framework)
│   ├── login.html, signup.html, dashboard.html
│   ├── css/style.css
│   └── js/login.js, signup.js, dashboard.js
├── native/mmfms.c            the original mmap() C program (compile separately)
├── data/                     created at runtime
│   ├── users.json            user accounts (hashed passwords only)
│   └── users/<username>/     each user's own separate files
└── .env.example
```

## Notes for your report / viva

- Passwords are hashed with **bcrypt** (`bcryptjs`) before storage — the
  plaintext password is never saved.
- Login state is kept in a server-side **session** (`express-session`),
  identified by a cookie; every `/api/files/*` route checks
  `req.session.userId` before touching any file.
- File names are validated with a strict regex and resolved against each
  user's own folder to prevent path-traversal attacks (e.g. `../../etc`).
- `data/users.json` is a simple JSON file so the project needs **no external
  database install** — good for a quick classroom demo. For a real
  deployment you'd swap this for a proper database (e.g. SQLite/Postgres)
  and a persistent session store.
- This backend uses `fs.readSync`/`fs.writeSync` with a byte **position**
  argument — that is the same `pread()`/`pwrite()` system call family the
  OS course covers, giving true offset-based random access without loading
  the whole file. It is *not* `mmap()`; the true `mmap()` demonstration is
  `native/mmfms.c`.

## Building the original C program (optional, for the OS-level demo)

```bash
cd native
gcc -O2 -Wall -Wextra -o mmfms mmfms.c
./mmfms
```
