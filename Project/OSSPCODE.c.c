/*
 * ============================================================================
 *  Linux Memory-Mapped File Management System  (mmfms)
 *  Operating Systems and Systems Programming (25CS2104E) - Section 4, Team 2
 *
 *  Team : 2520030081 S. Ashwanth Reddy
 *         2520030518 P. Naveen
 *         2520030280 CH. Nripendra
 *  Guide: Ms. Soumya Enukonda
 *
 *  BUILD :  gcc -O2 -Wall -Wextra -o mmfms mmfms.c
 *  RUN   :  ./mmfms                 interactive shell
 *           ./mmfms myfile.txt      open a file straight away
 *           ./mmfms bench 128M      run the benchmark and exit
 *
 *  WORKFLOW (slide 7):
 *      open() -> mmap() -> access / modify -> msync() -> munmap() -> close()
 *
 *  ---------------------------------------------------------------------------
 *  SYSTEM CALL QUICK REFERENCE  (what each call does and where it is used)
 *  ---------------------------------------------------------------------------
 *   open()        get a file descriptor for the file          cmd_open, cmd_create
 *   fstat()       read file size / type from the descriptor   cmd_open
 *   mmap()        map the file into virtual memory            cmd_open, cmd_append, bench
 *   munmap()      remove the mapping                          cmd_close, bench
 *   msync()       flush modified pages of the map to disk     cmd_sync, cmd_close, bench
 *   mremap()      grow / move an existing mapping             cmd_append
 *   madvise()     hint the kernel about the access pattern    cmd_madvise, bench
 *   mincore()     ask which pages are resident in RAM         count_resident
 *   posix_fallocate() reserve disk blocks before growing      cmd_append
 *   ftruncate()   set file length (used for rollback)         cmd_append
 *   close()       release the file descriptor                 cmd_close
 *   read/write    classic copy-based I/O (used in bench only) bench, cmd_create
 *   pread/pwrite  read/write at an offset without lseek()     bench
 *   lseek()       move the file offset                        bench
 *   fsync()       force file data to disk                     bench
 *   getrusage()   page-fault counters (minor / major)         get_faults
 *   sigaction()   install SIGSEGV / SIGBUS handler            main
 *   clock_gettime() high-resolution timer                     now
 *   mkstemp()/unlink() temp file for the benchmark            run_bench
 *
 *  OS CONCEPTS DEMONSTRATED:
 *     virtual memory, page tables (/proc/self/maps), demand paging,
 *     page-fault counting, page cache residency, SIGSEGV/SIGBUS handling.
 * ============================================================================
 */
#define _GNU_SOURCE                 /* needed for memmem(), mremap(), MREMAP_MAYMOVE */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>                  /* open(), O_* flags, posix_fallocate()          */
#include <limits.h>                 /* PATH_MAX                                      */
#include <signal.h>                 /* sigaction(), SIGSEGV, SIGBUS                  */
#include <setjmp.h>                 /* sigsetjmp()/siglongjmp() for fault recovery   */
#include <stdint.h>
#include <time.h>                   /* clock_gettime()                               */
#include <unistd.h>                 /* read, write, close, lseek, ftruncate, ...     */
#include <sys/mman.h>               /* mmap, munmap, msync, mremap, madvise, mincore */
#include <sys/stat.h>               /* fstat()                                       */
#include <sys/types.h>
#include <sys/resource.h>           /* getrusage()                                   */

#define DEFAULT_BENCH_SIZE (64ULL << 20)   /* 64 MB */

/* =========================================================================
 *  GLOBAL STATE - describes the one file that is currently mapped
 * ========================================================================= */
typedef struct {
    int    fd;                 /* file descriptor returned by open()             */
    char  *map;                /* start address returned by mmap() (NULL = none) */
    size_t size;               /* length of the mapping == size of the file      */
    int    writable;           /* 1 = opened read-write, 0 = read-only           */
    int    dirty;              /* 1 = changed in memory but not yet msync()'d    */
    char   path[PATH_MAX];     /* name of the open file                          */
    long   base_min, base_maj; /* page-fault counters at the moment of open()    */
} MFile;

static MFile M = { .fd = -1 };      /* everything else is zero-initialised */
static int   quit_flag = 0;         /* set by the 'quit' command */

/* =========================================================================
 *  SIGSEGV / SIGBUS PROTECTION
 *  Touching a mapping can fault (e.g. file truncated by another process ->
 *  SIGBUS). We catch it while 'guard' is set and jump back to a safe point
 *  instead of crashing the whole program.
 * ========================================================================= */
static sigjmp_buf            jb;                 /* saved "safe point"          */
static volatile sig_atomic_t guard      = 0;     /* 1 = inside guarded access   */
static volatile sig_atomic_t fault_sig  = 0;     /* which signal fired          */
static void * volatile       fault_addr = NULL;  /* address that faulted        */

static void on_fault(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    if (guard) {                         /* fault happened inside mmap access   */
        fault_sig  = sig;
        fault_addr = si->si_addr;        /* kernel tells us the bad address     */
        siglongjmp(jb, 1);               /* jump back to GUARDED_BEGIN          */
    }
    signal(sig, SIG_DFL);                /* unexpected fault: restore default...*/
    raise(sig);                          /* ...and die normally (core dump)     */
}

static void report_fault(void)
{
    printf("\n[!] %s at address %p while touching the mapped region.\n",
           fault_sig == SIGBUS ? "SIGBUS (bus error)" : "SIGSEGV (segmentation fault)",
           (void *)fault_addr);
    if (fault_sig == SIGBUS)
        printf("    The backing file was probably truncated by another process\n"
               "    or the disk is full. Close and re-open the file.\n");
    else
        printf("    Invalid access (e.g. writing to a read-only mapping).\n");
}

/* Wrap every direct access to M.map between these two macros. */
#define GUARDED_BEGIN  if (sigsetjmp(jb, 1)) { guard = 0; report_fault(); return; } guard = 1
#define GUARDED_END    guard = 0

/* =========================================================================
 *  SMALL HELPERS
 * ========================================================================= */
static double now(void)                          /* seconds, monotonic clock */
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);         /* clock_gettime(): high-resolution timer,
                                                    unaffected by system clock changes */
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static void get_faults(long *minf, long *majf)
{
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);                 /* getrusage(): kernel's per-process statistics */
    *minf = ru.ru_minflt;                        /* minor fault = page in page cache, only page-table filled */
    *majf = ru.ru_majflt;                        /* major fault = page had to be read from disk */
}

static unsigned long long parse_size(const char *s)   /* "10M" -> 10485760 */
{
    char *end;
    unsigned long long v = strtoull(s, &end, 10);
    switch (toupper((unsigned char)*end)) {
        case 'K': v <<= 10; break;
        case 'M': v <<= 20; break;
        case 'G': v <<= 30; break;
        default:  break;
    }
    return v;
}

static size_t unescape(char *s)                  /* in place: "\n" "\t" "\\" -> real characters */
{
    char *r = s, *w = s;
    while (*r) {
        if (*r == '\\' && r[1]) {
            r++;
            switch (*r) {
                case 'n':  *w++ = '\n'; break;
                case 't':  *w++ = '\t'; break;
                case '\\': *w++ = '\\'; break;
                default:   *w++ = '\\'; *w++ = *r; break;
            }
            r++;
        } else {
            *w++ = *r++;
        }
    }
    *w = '\0';
    return (size_t)(w - s);                      /* new length (text may contain '\0' via memcpy use) */
}

static int need_open(void)
{
    if (M.fd < 0) { printf("No file is open. Use: open <file> [ro|rw]\n"); return 0; }
    return 1;
}

static int need_map(void)
{
    if (!need_open()) return 0;
    if (!M.map) { printf("The file is empty (nothing mapped). Use: append <text>\n"); return 0; }
    return 1;
}

/* How many pages of the mapping are currently in RAM (page cache)? */
static size_t count_resident(void)
{
    long   ps    = sysconf(_SC_PAGESIZE);        /* page size, normally 4096 bytes */
    size_t pages = (M.size + (size_t)ps - 1) / (size_t)ps, n = 0;
    unsigned char *vec = malloc(pages);          /* one status byte per page */
    if (!vec) return 0;
    if (mincore(M.map, M.size, vec) == 0)        /* mincore(): fills vec[i]'s lowest bit = 1 if page i is in RAM */
        for (size_t i = 0; i < pages; i++) n += vec[i] & 1;
    free(vec);
    return n;
}

/* Shared parser for 'read' and 'text':  <offset> [length]  -> clamped to file.
 * Returns 1 if OK, 0 if the command should stop (message already printed). */
static int parse_range(const char *cmd, const char *args, size_t dflt, size_t *off, size_t *len)
{
    long long o = 0, l = (long long)dflt;
    if (sscanf(args, "%lli %lli", &o, &l) < 1 || o < 0 || l <= 0) {   /* %lli accepts 0x.. hex too */
        printf("Usage: %s <offset> [length]\n", cmd);
        return 0;
    }
    if ((size_t)o >= M.size) {
        printf("Offset %lld is beyond end of file (%zu bytes).\n", o, M.size);
        return 0;
    }
    *off = (size_t)o;
    *len = (size_t)l;
    if (*len > M.size - *off) *len = M.size - *off;    /* never run past the end of the mapping */
    return 1;
}

/* =========================================================================
 *  FILE CREATION
 * ========================================================================= */
static void cmd_create(char *args)
{
    char path[PATH_MAX], sz[32];
    if (sscanf(args, "%4095s %31s", path, sz) != 2) {
        printf("Usage: create <file> <size>     e.g. create test.txt 10M\n");
        return;
    }
    unsigned long long size = parse_size(sz);
    if (size == 0 || size > (4ULL << 30)) { printf("Size must be between 1 byte and 4G.\n"); return; }

    /* open(): O_CREAT creates it, O_EXCL refuses to overwrite an existing file,
       0644 = owner read/write, others read-only */
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) { printf("Cannot create '%s': %s\n", path, strerror(errno)); return; }

    /* Build the file in a 64 KB buffer so we make few write() calls (fast). */
    const size_t BUFSZ = 1 << 16;
    char *buf = malloc(BUFSZ);
    if (!buf) { close(fd); return; }
    size_t used = 0;
    unsigned long long written = 0, ln = 0;
    char line[160];
    while (written < size) {
        int n = snprintf(line, sizeof line,
                 "Line %010llu | Memory-mapped I/O demo | the quick brown fox jumps over the lazy dog\n", ln++);
        unsigned long long room = size - written;
        size_t take = ((unsigned long long)n < room) ? (size_t)n : (size_t)room;
        memcpy(buf + used, line, take);
        if (take < (size_t)n) buf[used + take - 1] = '\n';       /* last line still ends with newline */
        used += take; written += take;
        if (used + sizeof line > BUFSZ || written >= size) {     /* buffer full (or done): flush it */
            if (write(fd, buf, used) != (ssize_t)used) {         /* write(): copy buffer -> kernel -> file */
                printf("Write error: %s\n", strerror(errno));
                free(buf); close(fd); return;
            }
            used = 0;
        }
    }
    free(buf);
    close(fd);                                   /* close(): release the descriptor */
    printf("Created '%s' (%llu bytes, %llu lines).\n", path, size, ln);
}

/* =========================================================================
 *  OPEN / CLOSE / INFO      (steps 1, 2 and 4-6 of the workflow)
 * ========================================================================= */
static void cmd_open(char *args)
{
    if (M.fd >= 0) { printf("'%s' is already open. Run 'close' first.\n", M.path); return; }

    char path[PATH_MAX], mode[8] = "rw";
    if (sscanf(args, "%4095s %7s", path, mode) < 1) { printf("Usage: open <file> [ro|rw]\n"); return; }

    int wr;
    if      (!strcmp(mode, "rw")) wr = 1;
    else if (!strcmp(mode, "ro")) wr = 0;
    else { printf("Mode must be 'ro' or 'rw'.\n"); return; }

    /* STEP 1 - open(): returns a file descriptor (small integer handle) */
    int fd = open(path, wr ? O_RDWR : O_RDONLY);
    if (fd < 0) {
        printf("open('%s') failed: %s\n", path, strerror(errno));
        if (errno == EACCES && wr) printf("Hint: try  open %s ro\n", path);
        return;
    }

    /* fstat(): fetch size and type; mmap can only map regular files */
    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode)) {
        printf("'%s' is not a regular file.\n", path);
        close(fd); return;
    }

    char *map = NULL;
    if (st.st_size > 0) {                        /* mmap() of length 0 is an error, so skip empty files */
        int prot = PROT_READ | (wr ? PROT_WRITE : 0);   /* allowed access to the mapped pages */
        /* STEP 2 - mmap(): MAP_SHARED = stores go to the file itself (and are visible to other
           processes). NULL lets the kernel pick the virtual address; offset 0 = map from start. */
        map = mmap(NULL, (size_t)st.st_size, prot, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) { printf("mmap() failed: %s\n", strerror(errno)); close(fd); return; }
    }

    M.fd = fd; M.map = map; M.size = (size_t)st.st_size;
    M.writable = wr; M.dirty = 0;
    snprintf(M.path, sizeof M.path, "%s", path);
    get_faults(&M.base_min, &M.base_maj);        /* remember fault counts so 'faults' can show the difference */

    printf("Opened '%s' (%s), %zu bytes.\n", path, wr ? "read-write" : "read-only", M.size);
    if (map) printf("Mapped at virtual address %p. Nothing is copied at this point -\n"
                    "page-table entries are filled in lazily (demand paging) on first access.\n", (void *)map);
    else     printf("File is empty - use 'append' to add data.\n");
}

static void cmd_close(char *args)
{
    (void)args;
    if (!need_open()) return;
    if (M.map) {
        /* STEP 4 - msync(): write dirty pages back to disk. MS_SYNC blocks until the data is written. */
        if (M.writable && msync(M.map, M.size, MS_SYNC) != 0)
            printf("msync() failed: %s\n", strerror(errno));
        else if (M.dirty)
            printf("Flushed pending changes to disk (msync).\n");
        /* STEP 5 - munmap(): remove the mapping and give the virtual address range back */
        if (munmap(M.map, M.size) != 0)
            printf("munmap() failed: %s\n", strerror(errno));
    }
    close(M.fd);                                 /* STEP 6 - close(): release the file descriptor */
    printf("Closed '%s' - mapping released.\n", M.path);
    M.fd = -1; M.map = NULL; M.size = 0; M.dirty = 0; M.path[0] = '\0';
}

static void cmd_info(char *args)
{
    (void)args;
    if (!need_open()) return;
    long ps = sysconf(_SC_PAGESIZE);
    printf("File            : %s\n", M.path);
    printf("Mode            : %s\n", M.writable ? "read-write (PROT_READ|PROT_WRITE, MAP_SHARED)"
                                                : "read-only (PROT_READ, MAP_SHARED)");
    printf("Size            : %zu bytes\n", M.size);
    printf("Page size       : %ld bytes\n", ps);
    if (!M.map) { printf("Mapping         : none (empty file)\n"); return; }

    size_t pages = (M.size + (size_t)ps - 1) / (size_t)ps;
    size_t res   = count_resident();
    printf("Mapping         : %p - %p\n", (void *)M.map, (void *)(M.map + M.size));
    printf("Virtual pages   : %zu\n", pages);
    printf("In page cache   : %zu / %zu pages (%.1f%%) are in RAM (mincore)\n",
           res, pages, pages ? 100.0 * (double)res / (double)pages : 0.0);
    printf("Unsynced changes: %s\n", M.dirty ? "YES - run 'sync'" : "no");

    /* /proc/self/maps = the kernel's view of this process's virtual memory areas.
       We print the one line that contains our mapping (address range, permissions, file). */
    FILE *f = fopen("/proc/self/maps", "r");
    if (f) {
        char ln[512];
        while (fgets(ln, sizeof ln, f)) {
            unsigned long lo, hi;
            if (sscanf(ln, "%lx-%lx", &lo, &hi) == 2 &&
                (unsigned long)M.map >= lo && (unsigned long)M.map < hi) {
                printf("/proc/self/maps : %s", ln);
                break;
            }
        }
        fclose(f);
    }
}

/* =========================================================================
 *  READ-TYPE COMMANDS  (plain memory loads - no read() system call!)
 * ========================================================================= */
static void cmd_read(char *args)                 /* random access: hex dump */
{
    if (!need_map()) return;
    size_t off, len;
    if (!parse_range("read", args, 256, &off, &len)) return;
    if (len > 4096) { len = 4096; printf("(showing first 4096 bytes)\n"); }

    GUARDED_BEGIN;
    for (size_t i = 0; i < len; i += 16) {
        printf("%08zx  ", off + i);
        for (size_t j = 0; j < 16; j++) {
            if (i + j < len) printf("%02x ", (unsigned char)M.map[off + i + j]);   /* array-style access = page fault on 1st touch */
            else             printf("   ");
            if (j == 7) putchar(' ');
        }
        printf(" |");
        for (size_t j = 0; j < 16 && i + j < len; j++) {
            unsigned char c = (unsigned char)M.map[off + i + j];
            putchar(isprint(c) ? c : '.');
        }
        printf("|\n");
    }
    GUARDED_END;
}

static void cmd_text(char *args)                 /* random access: print as text */
{
    if (!need_map()) return;
    size_t off, len;
    if (!parse_range("text", args, 256, &off, &len)) return;

    GUARDED_BEGIN;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)M.map[off + i];
        putchar((isprint(c) || c == '\n' || c == '\t') ? c : '.');
    }
    putchar('\n');
    GUARDED_END;
}

static void cmd_head(char *args)                 /* sequential access: first n lines */
{
    if (!need_map()) return;
    int n = *args ? atoi(args) : 10;
    if (n <= 0) n = 10;

    GUARDED_BEGIN;
    size_t pos = 0;
    for (int i = 0; i < n && pos < M.size; i++) {
        const char *nl = memchr(M.map + pos, '\n', M.size - pos);    /* scan mapped memory for end of line */
        size_t len = nl ? (size_t)(nl - (M.map + pos)) : M.size - pos;
        fwrite(M.map + pos, 1, len, stdout);     /* print straight from the mapping - no intermediate buffer */
        putchar('\n');
        pos += len + 1;
    }
    GUARDED_END;
}

static void cmd_search(char *args)               /* memmem() over the whole mapping */
{
    if (!need_map()) return;
    size_t plen = unescape(args);
    if (plen == 0) { printf("Usage: search <text>\n"); return; }

    long f0, m0, f1, m1;
    get_faults(&f0, &m0);                        /* fault counters BEFORE the search */
    double t0 = now();

    GUARDED_BEGIN;
    size_t pos = 0, count = 0, last = 0, line = 1;
    while (pos < M.size) {
        char *hit = memmem(M.map + pos, M.size - pos, args, plen);   /* fast substring search directly in the mapping */
        if (!hit) break;
        size_t o = (size_t)(hit - M.map);
        /* Count newlines only since the previous hit (O(n) total, not O(n*hits)). */
        const char *q = M.map + last;
        while ((q = memchr(q, '\n', (size_t)(M.map + o - q))) != NULL) { line++; q++; }
        last = o;
        if (count < 10) {                        /* print only the first 10 matches */
            const char *eol = memchr(hit, '\n', M.size - o);
            size_t show = eol ? (size_t)(eol - hit) : M.size - o;
            if (show > 60) show = 60;
            printf("  offset %-10zu line %-8zu %.*s\n", o, line, (int)show, hit);
        }
        count++;
        pos = o + plen;
    }
    GUARDED_END;

    double ms = (now() - t0) * 1000.0;
    get_faults(&f1, &m1);                        /* fault counters AFTER: difference = faults caused by the search */
    printf("Found %zu match(es)%s in %.3f ms  [page faults during search: %ld minor, %ld major]\n",
           count, count > 10 ? " (first 10 shown)" : "", ms, f1 - f0, m1 - m0);
}

/* =========================================================================
 *  MODIFYING COMMANDS
 * ========================================================================= */
static void cmd_write(char *args)                /* overwrite bytes in place */
{
    if (!need_map()) return;
    if (!M.writable) { printf("File is read-only. Re-open with: open <file> rw\n"); return; }

    char *end;
    unsigned long long off = strtoull(args, &end, 0);      /* base 0 = accepts decimal or 0x.. hex */
    if (end == args || *end == '\0') { printf("Usage: write <offset> <text>\n"); return; }
    if (*end == ' ') end++;
    size_t len = unescape(end);
    if (len == 0) { printf("Usage: write <offset> <text>\n"); return; }
    if (off > M.size || len > M.size - off) {
        printf("Out of bounds: file is %zu bytes. Use 'append' to grow the file.\n", M.size);
        return;
    }
    GUARDED_BEGIN;
    memcpy(M.map + off, end, len);               /* a plain memory store - no write() system call.
                                                    The kernel marks the page dirty and writes it later. */
    GUARDED_END;
    M.dirty = 1;
    printf("Wrote %zu byte(s) at offset %llu in memory. Run 'sync' to flush to disk.\n", len, off);
}

static void cmd_append(char *args)               /* grow file + mapping, then add text */
{
    if (!need_open()) return;
    if (!M.writable) { printf("File is read-only. Re-open with: open <file> rw\n"); return; }
    size_t len = unescape(args);
    if (len == 0) { printf("Usage: append <text>\n"); return; }

    size_t oldsz = M.size, newsz = M.size + len;

    /* posix_fallocate(): reserve disk blocks ONLY for the new range [oldsz, newsz) and extend the
       file. Doing this first means we cannot later hit SIGBUS from "disk full" while writing. */
    int r = posix_fallocate(M.fd, (off_t)oldsz, (off_t)len);
    if (r != 0) { printf("Cannot extend file: %s\n", strerror(r)); return; }

    char *nm;
    if (M.map) nm = mremap(M.map, oldsz, newsz, MREMAP_MAYMOVE);   /* mremap(): resize the mapping, kernel may move it
                                                                      (cheaper than munmap + mmap again) */
    else       nm = mmap(NULL, newsz, PROT_READ | PROT_WRITE, MAP_SHARED, M.fd, 0);   /* file was empty: first mapping */
    if (nm == MAP_FAILED) {
        printf("Remap failed: %s\n", strerror(errno));
        if (ftruncate(M.fd, (off_t)oldsz) != 0) { /* best-effort rollback of the file length */ }
        return;
    }
    M.map = nm; M.size = newsz;

    GUARDED_BEGIN;
    memcpy(M.map + oldsz, args, len);            /* copy new text into the newly mapped tail */
    GUARDED_END;
    M.dirty = 1;
    printf("Appended %zu byte(s). File is now %zu bytes (mapping resized with mremap).\n", len, M.size);
}

static void cmd_sync(char *args)                 /* STEP 4 on demand: msync() */
{
    if (!need_map()) return;
    if (!M.writable) { printf("Read-only mapping - nothing to flush.\n"); return; }
    int async = (strncmp(args, "async", 5) == 0);
    double t0 = now();
    /* msync(): MS_SYNC  = block until pages are physically written (data is safe)
                MS_ASYNC = only schedule the write and return immediately (faster, less safe) */
    if (msync(M.map, M.size, async ? MS_ASYNC : MS_SYNC) != 0) {
        printf("msync() failed: %s\n", strerror(errno));
        return;
    }
    if (!async) M.dirty = 0;
    printf("msync(%s) completed in %.3f ms.%s\n", async ? "MS_ASYNC" : "MS_SYNC",
           (now() - t0) * 1000.0,
           async ? " (flush scheduled; not guaranteed on disk yet)" : " Changes are on disk.");
}

/* =========================================================================
 *  VIRTUAL-MEMORY OBSERVATION
 * ========================================================================= */
static void cmd_faults(char *args)
{
    (void)args;
    if (!need_open()) return;
    long mn, mj;
    get_faults(&mn, &mj);
    printf("Page faults since file was opened : %ld minor, %ld major\n", mn - M.base_min, mj - M.base_maj);
    printf("Page faults for whole process     : %ld minor, %ld major\n", mn, mj);
    printf("  minor = page already in page cache, only the page table is filled in\n");
    printf("  major = page had to be read from disk\n");
    if (M.map) printf("Pages of mapping in page cache    : %zu\n", count_resident());
}

static void cmd_madvise(char *args)
{
    if (!need_map()) return;
    char a[32];
    if (sscanf(args, "%31s", a) != 1) {
        printf("Usage: madvise <seq|rand|normal|willneed|dontneed>\n");
        return;
    }
    int adv;
    if      (!strcmp(a, "seq"))      adv = MADV_SEQUENTIAL;   /* read-ahead more, drop pages behind us      */
    else if (!strcmp(a, "rand"))     adv = MADV_RANDOM;       /* disable read-ahead (random access)         */
    else if (!strcmp(a, "normal"))   adv = MADV_NORMAL;       /* default kernel behaviour                   */
    else if (!strcmp(a, "willneed")) adv = MADV_WILLNEED;     /* pre-load these pages into the page cache   */
    else if (!strcmp(a, "dontneed")) adv = MADV_DONTNEED;     /* drop this process's page-table entries     */
    else { printf("Unknown advice '%s'.\n", a); return; }

    /* madvise(): purely a hint about how we will use the range; lets the kernel tune paging */
    if (madvise(M.map, M.size, adv) != 0) { printf("madvise() failed: %s\n", strerror(errno)); return; }
    printf("madvise(%s) applied. Pages in page cache: %zu\n", a, count_resident());
    if (adv == MADV_DONTNEED)
        printf("This process's page-table entries were dropped (the data stays in the page\n"
               "cache). Access the file again and watch 'faults' - minor faults will rise.\n");
}

/* =========================================================================
 *  BENCHMARK:  mmap  vs  read()/write()
 * ========================================================================= */
static long minflt_now(void) { long a, b; get_faults(&a, &b); return a; }

static void bench_row(const char *name, double t_rw, double t_mm, long faults)
{
    printf("%-24s %12.2f %12.2f %9.2fx %10ld\n", name, t_rw * 1000.0, t_mm * 1000.0,
           t_mm > 0 ? t_rw / t_mm : 0.0, faults);
}

/* mmap the whole benchmark file, optionally applying a madvise() hint (advice < 0 = none). */
static unsigned char *bmap(int fd, size_t size, int prot, int advice)
{
    unsigned char *p = mmap(NULL, size, prot, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) { printf("mmap failed: %s\n", strerror(errno)); return NULL; }
    if (advice >= 0) madvise(p, size, advice);
    return p;
}

static void run_bench(unsigned long long size)
{
    const size_t BUF = 1 << 16;                  /* 64 KB buffer for the read()/write() side */
    const size_t N   = 1000000;                  /* number of random operations              */
    if (size < (1ULL << 20)) size = 1ULL << 20;  /* minimum 1 MB */
    if (size > (2ULL << 30)) size = 2ULL << 30;  /* maximum 2 GB (temp file lives in /tmp)   */

    char path[] = "/tmp/mmfms_bench_XXXXXX";
    int fd = mkstemp(path);                      /* mkstemp(): create a unique temp file and open it read-write */
    if (fd < 0) { printf("Cannot create temp file: %s\n", strerror(errno)); return; }

    unsigned char *buf  = malloc(BUF);
    size_t        *offs = malloc(N * sizeof *offs);
    if (!buf || !offs) { printf("Out of memory.\n"); close(fd); unlink(path); free(buf); free(offs); return; }
    for (size_t i = 0; i < BUF; i++) buf[i] = (unsigned char)(i * 131 + 17);   /* non-zero pattern */

    printf("Preparing %llu MB temporary file %s ...\n", size >> 20, path);
    for (unsigned long long left = size; left;) {           /* fill the file with write() */
        size_t n = left < BUF ? (size_t)left : BUF;
        if (write(fd, buf, n) != (ssize_t)n) { printf("Write error: %s\n", strerror(errno)); goto done; }
        left -= n;
    }
    fsync(fd);                                   /* fsync(): make sure file is fully on disk before timing */

    /* Same random offsets for both methods (xorshift64, fixed seed) => fair, repeatable comparison. */
    uint64_t x = 88172645463325252ULL;
    for (size_t i = 0; i < N; i++) {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        offs[i] = (size_t)(x % size);
    }

    printf("\nRunning benchmark (file in page cache, %zu random ops per random test)...\n\n", N);
    printf("%-24s %12s %12s %10s %10s\n", "Test", "read/write", "mmap", "speed-up", "mmap flts");
    printf("%-24s %12s %12s %10s %10s\n", "", "(ms)", "(ms)", "", "(minor)");
    printf("--------------------------------------------------------------------------\n");

    double t0, t_rw, t_mm; long f0; uint64_t s1, s2; ssize_t n;
    unsigned char *p; unsigned char b;

    /* 1. SEQUENTIAL READ ----------------------------------------------------- */
    s1 = 0; lseek(fd, 0, SEEK_SET); t0 = now();               /* lseek(): rewind file offset to 0 */
    while ((n = read(fd, buf, BUF)) > 0)                      /* read(): one syscall + one copy per 64 KB */
        for (ssize_t i = 0; i < n; i++) s1 += buf[i];
    t_rw = now() - t0;

    s2 = 0; f0 = minflt_now(); t0 = now();
    if (!(p = bmap(fd, size, PROT_READ, MADV_SEQUENTIAL))) goto done;   /* hint: reading front-to-back */
    for (size_t i = 0; i < size; i++) s2 += p[i];             /* plain memory reads; page faults load pages */
    munmap(p, size);
    t_mm = now() - t0;
    bench_row("Sequential read", t_rw, t_mm, minflt_now() - f0);
    if (s1 != s2) printf("  WARNING: checksums differ!\n");

    /* 2. RANDOM READ (1 byte each) ------------------------------------------- */
    s1 = 0; t0 = now();
    for (size_t i = 0; i < N; i++)                            /* pread(): read at offset, no lseek needed,  */
        if (pread(fd, &b, 1, (off_t)offs[i]) == 1) s1 += b;   /* but still 1 system call per byte!          */
    t_rw = now() - t0;

    s2 = 0; f0 = minflt_now(); t0 = now();
    if (!(p = bmap(fd, size, PROT_READ, MADV_RANDOM))) goto done;       /* hint: no read-ahead */
    for (size_t i = 0; i < N; i++) s2 += p[offs[i]];          /* no system call at all - just a memory load */
    munmap(p, size);
    t_mm = now() - t0;
    bench_row("Random read (1 byte)", t_rw, t_mm, minflt_now() - f0);
    if (s1 != s2) printf("  WARNING: checksums differ!\n");

    /* 3. SEQUENTIAL WRITE (including flush to disk) -------------------------- */
    lseek(fd, 0, SEEK_SET); t0 = now();
    for (unsigned long long left = size; left;) {
        size_t c = left < BUF ? (size_t)left : BUF;
        if (write(fd, buf, c) != (ssize_t)c) break;           /* write(): copy user buffer -> kernel */
        left -= c;
    }
    fsync(fd);                                                /* force to disk so both sides do equal work */
    t_rw = now() - t0;

    f0 = minflt_now(); t0 = now();
    if (!(p = bmap(fd, size, PROT_READ | PROT_WRITE, -1))) goto done;
    for (unsigned long long off = 0; off < size; off += BUF) {
        size_t c = (size - off) < BUF ? (size_t)(size - off) : BUF;
        memcpy(p + off, buf, c);                              /* memory store into the mapping */
    }
    msync(p, size, MS_SYNC);                                  /* msync(): same "force to disk" step */
    munmap(p, size);
    t_mm = now() - t0;
    bench_row("Sequential write+sync", t_rw, t_mm, minflt_now() - f0);

    /* 4. RANDOM WRITE (including flush to disk) ------------------------------ */
    t0 = now();
    for (size_t i = 0; i < N; i++) {
        b = (unsigned char)i;
        if (pwrite(fd, &b, 1, (off_t)offs[i]) != 1) break;    /* pwrite(): 1 system call per byte */
    }
    fsync(fd);
    t_rw = now() - t0;

    f0 = minflt_now(); t0 = now();
    if (!(p = bmap(fd, size, PROT_READ | PROT_WRITE, MADV_RANDOM))) goto done;
    for (size_t i = 0; i < N; i++) p[offs[i]] = (unsigned char)i;      /* plain memory store */
    msync(p, size, MS_SYNC);
    munmap(p, size);
    t_mm = now() - t0;
    bench_row("Random write+sync", t_rw, t_mm, minflt_now() - f0);

    printf("--------------------------------------------------------------------------\n");
    printf("speed-up = read/write time / mmap time  (>1 means mmap is faster).\n");
    printf("Random access gains most: read()/pread() pays one system call and one\n");
    printf("kernel->user copy per access; mmap pays only a page fault the first time\n");
    printf("each page is touched. For big sequential reads with a large buffer the\n");
    printf("two are close, because both are bounded by memory/disk bandwidth.\n");

done:
    free(buf); free(offs);
    close(fd);
    unlink(path);                                /* unlink(): delete the temporary file */
}

static void cmd_bench(char *args)
{
    run_bench(*args ? parse_size(args) : DEFAULT_BENCH_SIZE);
}

/* =========================================================================
 *  COMMAND TABLE  (one row per command; 'help' is generated from it)
 *  raw = 1 means the argument text is taken literally (append / search).
 * ========================================================================= */
typedef void (*handler_t)(char *args);
typedef struct {
    const char *group, *name, *usage, *desc;
    handler_t   fn;
    int         raw;
} Cmd;

static void cmd_help(char *args);
static void cmd_quit(char *args) { (void)args; quit_flag = 1; }

static const Cmd CMDS[] = {
 {"FILE / MAPPING", "create",  "create <file> <size>",   "create a test text file (size: 4096, 10K, 5M, 1G)", cmd_create,  0},
 {"FILE / MAPPING", "open",    "open <file> [ro|rw]",    "open() + mmap() the file (default rw)",             cmd_open,    0},
 {"FILE / MAPPING", "close",   "close",                  "msync() + munmap() + close()",                      cmd_close,   0},
 {"FILE / MAPPING", "info",    "info",                   "mapping details, resident pages, /proc/self/maps",  cmd_info,    0},
 {"ACCESS",         "head",    "head [n]",               "sequential read: first n lines (default 10)",       cmd_head,    0},
 {"ACCESS",         "read",    "read <offset> [len]",    "random access: hex dump (offset may be 0x..)",      cmd_read,    0},
 {"ACCESS",         "text",    "text <offset> [len]",    "random access: print as text",                      cmd_text,    0},
 {"ACCESS",         "search",  "search <text>",          "find all occurrences directly in mapped memory",    cmd_search,  1},
 {"MODIFY",         "write",   "write <offset> <text>",  "overwrite bytes in place through the mapping",      cmd_write,   0},
 {"MODIFY",         "append",  "append <text>",          "grow the file and mapping (mremap), add text",      cmd_append,  1},
 {"MODIFY",         "sync",    "sync [async]",           "msync(): flush changes to disk (MS_SYNC/MS_ASYNC)", cmd_sync,    0},
 {"OS INTERNALS",   "faults",  "faults",                 "minor/major page-fault counters (getrusage)",       cmd_faults,  0},
 {"OS INTERNALS",   "madvise", "madvise <seq|rand|normal|willneed|dontneed>", "give the kernel a paging hint", cmd_madvise, 0},
 {"PERFORMANCE",    "bench",   "bench [size]",           "mmap vs read()/write() benchmark (default 64M)",    cmd_bench,   0},
 {"OTHER",          "help",    "help",                   "show this list",                                    cmd_help,    0},
 {"OTHER",          "quit",    "quit",                   "close any open file and exit",                      cmd_quit,    0},
};
#define NCMDS (sizeof CMDS / sizeof CMDS[0])

static void cmd_help(char *args)
{
    (void)args;
    const char *grp = "";
    for (size_t i = 0; i < NCMDS; i++) {
        if (strcmp(grp, CMDS[i].group) != 0) { grp = CMDS[i].group; printf("\n%s\n", grp); }
        printf("  %-24s %s\n", CMDS[i].usage, CMDS[i].desc);
    }
    printf("\n  Note: \\n and \\t are understood inside write / append / search.\n\n");
}

/* =========================================================================
 *  MAIN - install the signal handler, then run the interactive shell
 * ========================================================================= */
int main(int argc, char **argv)
{
    /* sigaction(): register on_fault() for SIGSEGV and SIGBUS. SA_SIGINFO gives the handler
       the faulting address (si->si_addr). */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags     = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS,  &sa, NULL);

    /* Non-interactive modes:  ./mmfms bench [size]   and   ./mmfms --help */
    if (argc >= 2 && strcmp(argv[1], "bench") == 0) {
        run_bench(argc >= 3 ? parse_size(argv[2]) : DEFAULT_BENCH_SIZE);
        return 0;
    }
    if (argc >= 2 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("Usage: %s [file | bench [size]]\n", argv[0]);
        return 0;
    }

    printf("=====================================================================\n"
           "  Linux Memory-Mapped File Management System\n"
           "  OSSP (25CS2104E) - Section 4, Team 2\n"
           "  mmap() / msync() / munmap()  -  type 'help' for commands\n"
           "=====================================================================\n");
    if (argc >= 2) cmd_open(argv[1]);            /* ./mmfms file.txt  =>  open it immediately */

    char line[PATH_MAX + 4096];
    while (!quit_flag) {
        printf("mmfms> ");
        fflush(stdout);
        if (!fgets(line, sizeof line, stdin)) { putchar('\n'); break; }   /* Ctrl-D (EOF) also exits */

        size_t L = strlen(line);
        while (L && (line[L - 1] == '\n' || line[L - 1] == '\r')) line[--L] = '\0';

        char *cmd = line;
        while (isspace((unsigned char)*cmd)) cmd++;          /* skip leading spaces */
        if (!*cmd) continue;                                 /* empty line */
        char *args = cmd;
        while (*args && !isspace((unsigned char)*args)) args++;
        if (*args) *args++ = '\0';                           /* split "command" from "arguments" */

        if (!strcmp(cmd, "?")) cmd = "help";

        const Cmd *c = NULL;                                 /* look the command up in the table */
        for (size_t i = 0; i < NCMDS; i++)
            if (!strcmp(cmd, CMDS[i].name)) { c = &CMDS[i]; break; }
        if (!c) { printf("Unknown command '%s'. Type 'help'.\n", cmd); continue; }

        /* Normal commands ignore extra spaces; append/search keep the text exactly as typed. */
        if (!c->raw) while (isspace((unsigned char)*args)) args++;
        c->fn(args);
    }

    if (M.fd >= 0) cmd_close(NULL);              /* never leave a mapping / descriptor behind */
    printf("Goodbye.\n");
    return 0;
}
