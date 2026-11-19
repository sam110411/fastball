// From TotK Recomp runtime/fs_host.c @ ae30a61 (MIT, LICENSE.totk-recomp), itself adapted from MacNativeGames
// games/totk/runtime/fs_host.c @ b75be8cf (MIT, Copyright (c) 2026 MacNativeGames contributors); the probe follows
// MacNativeGames' romfs.c runtime_fs_probe and fs_host.c runtime_save_probe.
//
// fastball changes: the RomFS is a read-only mount of the extracted directory (M_ROM; TotK's romfs.c served an
// indexed romfs.bin), so OpenDirectory works on it too; the title's id and storage sizes come from fs_title (fs.h);
// the overloads MLB imports beyond TotK's (GetFreeSpaceSize, Extend/GetSaveDataSize, MountCacheStorage(name),
// DeleteDirectory, GetDirectoryEntryCount, QueryMountRomCacheSize(size_t *, int), EnableGlobalFileDataCache) are
// added; the RomFS probe checks the mount against romfs_list.tsv.
// nn::fs host implementation of the functions main imports. RomFS paths and
// handles go to the read-only backend (romfs.c); save, temporary and cache storage are host directories under the
// profile directory (rt_config()->profile, default build/profile):
//
//   <profile>/save/<uid hi><uid lo>/   committed application save data, created on first use as the system launcher
//                                      does for titles that declare save data
//   <profile>/save-tx/                 the save mount's working copy and Commit's staging (section "save
//                                      transactions")
//   <profile>/save-backup/<time>/      the three committed saves before the current one
//   <profile>/temp/                    emptied at start
//   <profile>/cache/<index>/           cache storage
//   <profile>/save-size                the save data size and journal size after ExtendSaveData (two s64)
//   <profile>/save-before-import/<time>/  fastball: the committed save an --import-save replaced (fs_save_import)
//   <profile>/profile.lock             fastball: held open while a game runs on this profile
//   <romfs>/                           the game's RomFS, extracted by tools/romfs_dump.py --extract-all; MountRom
//                                      mounts it read-only once <romfs>.complete says the extraction finished
//
// The save mount is journaled as on Horizon: changes go to a working copy and become the committed save atomically at
// Commit; a crash or an Unmount before Commit leaves the previous committed save. Commit returns at once and the copy
// runs on a background thread (section "save transactions"); fs_save_flush lets the exit paths wait for it. Paths are
// UTF-8 (platform.h plat_fs_*).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"

#ifdef _WIN32
#define strdup _strdup   // fastball: the MSVC C library names it so
#endif
#include "host.h"
#include "platform/platform.h"

#define FILE_BASE 0x200000000ull
#define DIR_BASE 0x300000000ull
#define NFILES 1024
#define NDIRS 256
#define NMOUNTS 16
#define PATH_CAP 1100

enum { M_FREE, M_SAVE, M_TEMP, M_CACHE, M_SAVE_RO, M_ROM };   // M_SAVE: the working copy; M_SAVE_RO: the
                                                              // committed save; M_ROM: the RomFS (fastball)

typedef struct { int kind; char name[32]; char root[1024]; } Mount;
// fastball: trace (MLB_TRACE=fs only): the guest path, and reads and bytes read so far, logged at times and on close
typedef struct { PlatFile *h; int mode, save; char *trace; uint64_t reads, bytes; } File;   // h NULL = free; save: on a save-data mount
typedef struct { char *name; int kind; uint64_t size; } Listed;
typedef struct { Listed *e; int n, next, mode, used; } Dir;   // the directory's entries at open; used 0 = free

static Mount mounts[NMOUNTS];
static File files[NFILES];
static Dir dirs[NDIRS];
static PlatMutex lock = PLAT_MUTEX_INIT;
static char profile[1024], save_root[1100], work_root[1100];
static volatile int32_t storage_ready;

static void need(int c, const char *m) {
    if (!c) rt_fatal("fs: %s", m);
}

// ---- directory listings: every entry but "." and "..", taken whole before the caller changes the directory.
typedef struct { Listed *e; int n, cap; } Listing;

static int listing_add(const char *name, int kind, uint64_t size, void *ctx) {
    Listing *l = ctx;
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 16;
        l->e = realloc(l->e, (size_t)l->cap * sizeof *l->e);
        need(l->e != NULL, "directory listing");
    }
    l->e[l->n].name = strdup(name);
    need(l->e[l->n].name != NULL, "directory listing");
    l->e[l->n].kind = kind, l->e[l->n].size = size;
    l->n++;
    return 1;
}

static int list_dir(const char *dir, Listing *l) {
    *l = (Listing){0};
    return plat_fs_list(dir, listing_add, l);
}

static void listing_free(Listed *e, int n) {
    for (int i = 0; i < n; i++) free(e[i].name);
    free(e);
}

// dir/name in a malloc'd string.
static char *join(const char *dir, const char *name) {
    size_t n = strlen(dir), k = strlen(name);
    char *p = malloc(n + k + 2);
    need(p != NULL, "path allocation");
    memcpy(p, dir, n);
    p[n] = '/';
    memcpy(p + n + 1, name, k + 1);
    return p;
}

// Save in progress: a save-data file open for writing, or save data changed in the last 3 s and not committed yet
// (TotK writes a slot's files back to back and then commits; between the files nothing is open for a moment).
static volatile int32_t save_writers, save_dirty;
static volatile int64_t save_changed_ns;
static void save_wait_commit(void);
static void save_touched(void) {
    save_wait_commit();   // every change to save data comes here first: none lands in a commit's snapshot
    plat_xchg64(&save_changed_ns, (int64_t)plat_now_ns());
    plat_xchg32(&save_dirty, 1);
}
int fs_save_busy(void) {
    if (plat_cas32(&save_writers, 0, 0) > 0) return 1;
    return plat_cas32(&save_dirty, 0, 0) &&
           plat_now_ns() - (uint64_t)plat_cas64(&save_changed_ns, 0, 0) < 3000000000ull;
}

// ---- host directories
static int make_dir(const char *p) {
    int e = plat_fs_mkdir(p);
    return !e || (e == PLAT_FS_EXISTS && plat_fs_kind(p) == PLAT_FS_DIR);
}

// p and every missing parent.
static void make_dirs(const char *p) {
    char buf[1100];
    need(snprintf(buf, sizeof buf, "%s", p) < (int)sizeof buf, "profile path");
    for (char *q = buf + 1; *q; q++)
        if ((*q == '/' || *q == '\\') && q[-1] != ':') {
            char c = *q;
            *q = 0;
            make_dir(buf);
            *q = c;
        }
    if (!make_dir(buf)) rt_fatal("fs: cannot create directory %s", buf);
}

// The first path remove_tree could not delete and the OS error code, for the message of a fatal.
static char remove_failed[1100];
static int remove_error;
static void remove_note(const char *path) {
    if (*remove_failed) return;
    snprintf(remove_failed, sizeof remove_failed, "%s", path);
    remove_error = plat_fs_os_error();
}

// Remove a directory tree; a link (symbolic link, junction) is removed as a link, never followed. 0 also when
// nothing is there.
static int remove_tree(const char *path) {
    int kind = plat_fs_kind(path);
    if (kind == PLAT_FS_MISSING) return 0;
    if (kind == PLAT_FS_FILE) {
        if (!plat_fs_unlink(path)) return 0;
        remove_note(path);
        return -1;
    }
    if (kind == PLAT_FS_LINK) {   // a link to a file goes by unlink, a link to a directory by rmdir
        if (!plat_fs_unlink(path) || !plat_fs_rmdir(path)) return 0;
        remove_note(path);
        return -1;
    }
    int bad = 0;
    if (kind == PLAT_FS_DIR) {
        Listing l;
        list_dir(path, &l);
        for (int i = 0; i < l.n; i++) {
            char *child = join(path, l.e[i].name);
            bad |= remove_tree(child);
            free(child);
        }
        listing_free(l.e, l.n);
    }
    if (plat_fs_rmdir(path)) {
        remove_note(path);
        bad |= -1;
    }
    return bad;
}

// remove_tree, tried again every 50 ms for up to 2 s while another process (the previous run still exiting, a virus
// scanner, a sync client) holds a file in the tree. The failing path and its OS error stay in remove_failed.
#define MOVE_TRIES 40   // 2 s, also move_dir's
static int remove_tree_retry(const char *path) {
    for (int i = 0;; i++) {
        *remove_failed = 0;
        if (!remove_tree(path)) return 0;
        if (i == MOVE_TRIES) return -1;
        plat_sleep_ms(50);
    }
}

// ---- save transactions
// The save mount's root is a working copy, made from the newest save when the first save mount appears. Commit copies
// it to a staging directory, which a rename makes a finished commit, and swaps that in for the committed save:
//
//   <profile>/save/<uid>/          committed save: what the next start loads when no finished commit waits
//   <profile>/save-tx/work/        working copy, the save mount's root; deleted at the last Unmount and at start
//   <profile>/save-tx/new/         Commit's staging copy of the working copy
//   <profile>/save-tx/ready-<n>/   a finished commit not swapped in yet: the newest save
//   <profile>/save-tx/old/         the committed save a Commit replaced, on its way to save-backup
//
// Commit: (1) copy work to new and flush every file (FlushFileBuffers) and directory, a file equal to the newest
// save's as a hard link to it; (2) rename new to ready-<n>, the commit point, and delete the ready-<n> it supersedes;
// (3) rename save/<uid> to old; (4) rename ready-<n> to save/<uid>; (5) move old to save-backup/<UTC time> and keep
// the newest three. Every rename is durable (plat_fs_rename). Windows refuses (3) while any file under save/<uid> is
// open, in this or another process (a virus scanner, Explorer's preview, a sync tool), and a folder an elevated run
// made can refuse it too. ready-<n> then stays the newest save: later working copies are made from it, and the next
// Commit and the next start try (3) to (5) again. The game's Commit returns 0 once the steps are queued; a thread runs
// them (95-135 ms on the game thread before, in the 1.4.3 fight save) while every change to the working copy waits in
// save_touched, and Unmount and the next Commit wait too. A crash in that time loses this Commit only, and a failed
// one is logged instead of returned. The recovery rule at start (save_recover):
//   new present          an unfinished copy: deleted (rollback). An older build renamed save/<uid> away before its
//                        finished copy: with save/<uid> and ready-<n> absent, new is that copy and becomes save/<uid>;
//   ready-<n> present    the highest n is the newest save and (3) to (5) run (roll forward); the others are deleted;
//   save/<uid> absent    with no ready-<n> either, old goes back to save/<uid>.
// Then old, if present, goes to save-backup, and work (changes no Commit took) is deleted.
#define SAVE_BACKUPS 3
#define COPY_CHUNK (1u << 20)

static char tx_root[1100], backup_root[1100], new_root[1100], old_root[1100];
static char import_root[1100];   // fastball: <profile>/save-tx/import, fs_save_import's staging copy
static int profile_lock_busy;    // fastball: another process holds <profile>/profile.lock (a running game)
static PlatFile *profile_lock;   // fastball: this process's hold on it; the probe lets go to delete its profile
static char ready_root[1100];   // the newest finished commit not swapped in, empty when none
static unsigned ready_seq;      // the highest ready-<n> this run made or found
static PlatMutex tx_lock = PLAT_MUTEX_INIT;   // taken before lock, never inside it
static int save_mounts;                  // M_SAVE mounts: the working copy exists while this is above 0

// Probe hook: Commit stops at its tx_crash_at-th step (from 1) and leaves the disk as a process killed there would.
static int tx_crash_at, tx_step, tx_dead;
static int crashed(void) {
    if (tx_crash_at && ++tx_step == tx_crash_at) tx_dead = 1;
    return tx_dead;
}

static int exists(const char *p) { return plat_fs_kind(p) != PLAT_FS_MISSING; }

// Rename a directory, durably; retried every 50 ms, tries times in all, while another process (a virus scanner, the
// indexer) holds a file in it. 0 on success.
static int move_dir(const char *from, const char *to, int tries) {
    for (int i = 0; i < tries; i++) {
        int e = plat_fs_rename(from, to, 1);
        if (!e) return 0;
        if (e != PLAT_FS_BUSY) return -1;
        plat_sleep_ms(50);
    }
    return -1;
}

// Copy one file; the source may be open for writing (a save file the game has not closed). 0 on success.
static int copy_file(const char *src, const char *dst, int durable, uint64_t *bytes) {
    PlatFile *in = plat_fs_open(src, 0, NULL);
    if (!in) return -1;
    PlatFile *out = plat_fs_create(dst, NULL);
    if (!out) {
        plat_file_close(in);
        return -1;
    }
    uint8_t *buf = malloc(COPY_CHUNK);
    need(buf != NULL, "copy buffer");
    int ok = 1;
    for (uint64_t at = 0;;) {
        int64_t got = plat_file_pread(in, buf, COPY_CHUNK, at);
        if (got < 0) ok = 0;
        if (!ok || !got) break;
        if (plat_file_pwrite(out, buf, (uint32_t)got, at) != got) ok = 0;
        if (!ok) break;
        at += (uint64_t)got;
        *bytes += (uint64_t)got;
    }
    if (ok && durable) ok = plat_file_fsync(out);
    free(buf);
    plat_file_close(in);
    plat_file_close(out);
    return ok ? 0 : -1;
}

// Whether files a and b hold the same bytes; 0 when either cannot be read.
static int same_file(const char *a, const char *b) {
    PlatFile *h[2] = {plat_fs_open(a, 0, NULL), plat_fs_open(b, 0, NULL)};
    PlatFileId ia, ib;
    int same = h[0] && h[1] && plat_file_id(h[0], &ia) && plat_file_id(h[1], &ib) && ia.size == ib.size;
    uint8_t *buf = same ? malloc(2 * (size_t)COPY_CHUNK) : NULL;
    if (same) need(buf != NULL, "compare buffer");
    for (uint64_t at = 0; same;) {
        int64_t ga = plat_file_pread(h[0], buf, COPY_CHUNK, at), gb = plat_file_pread(h[1], buf + COPY_CHUNK, COPY_CHUNK, at);
        if (ga < 0 || ga != gb || memcmp(buf, buf + COPY_CHUNK, (size_t)ga)) same = 0;
        else if (!ga) break;
        at += (uint64_t)ga;
    }
    free(buf);
    for (int i = 0; i < 2; i++)
        if (h[i]) plat_file_close(h[i]);
    return same;
}

// Copy the tree src to dst, which must not exist; links are skipped. durable: every file is flushed, and every
// directory as far as the file system allows. base (or NULL): the committed save; a file whose bytes equal base's file
// at the same path becomes a hard link to it instead of a copy (*nlinked), as committed files are never written in
// place (the game writes the working copy). A save commit then writes the 3-4 files of the slot the game saved instead
// of all 82 (300-380 ms of flushes on the game thread). 0 on success.
static int copy_tree(const char *src, const char *dst, const char *base, int durable, int *nfiles, int *nlinked,
                     uint64_t *bytes) {
    if (plat_fs_mkdir(dst)) return -1;
    Listing l;
    if (list_dir(src, &l)) return -1;
    int bad = 0;
    for (int i = 0; i < l.n && !bad; i++) {
        if (l.e[i].kind == PLAT_FS_LINK) continue;
        char *s = join(src, l.e[i].name), *d = join(dst, l.e[i].name);
        char *b = base ? join(base, l.e[i].name) : NULL;
        if (b && !exists(b)) free(b), b = NULL;
        if (l.e[i].kind == PLAT_FS_DIR) bad = copy_tree(s, d, b, durable, nfiles, nlinked, bytes);
        else {
            if (b && same_file(s, b) && !plat_fs_link(b, d)) bad = 0, ++*nlinked;
            else bad = copy_file(s, d, durable, bytes);
            *nfiles += !bad;
            if (!bad && durable && crashed()) bad = -1;
        }
        free(s);
        free(d);
        free(b);
    }
    listing_free(l.e, l.n);
    if (!bad && durable) plat_fs_sync_dir(dst);
    return bad;
}

static int listed_cmp(const void *a, const void *b) {
    return strcmp(((const Listed *)a)->name, ((const Listed *)b)->name);
}

// Whether paths a and b are one file (hard links of it).
static int same_inode(const char *a, const char *b) {
    PlatFile *h[2] = {plat_fs_open(a, 0, NULL), plat_fs_open(b, 0, NULL)};
    PlatFileId ia, ib;
    int same = h[0] && h[1] && plat_file_id(h[0], &ia) && plat_file_id(h[1], &ib) && ia.volume == ib.volume &&
               ia.file_id == ib.file_id;
    for (int i = 0; i < 2; i++)
        if (h[i]) plat_file_close(h[i]);
    return same;
}

// Whether tree a holds the names and kinds of tree b and each file of it as a hard link: what copy_tree makes from a
// working copy whose every file equals base's.
static int same_tree(const char *a, const char *b) {
    Listing la, lb;
    int ea = list_dir(a, &la), eb = list_dir(b, &lb);
    int same = !ea && !eb && la.n == lb.n;
    if (same) {
        qsort(la.e, (size_t)la.n, sizeof *la.e, listed_cmp);
        qsort(lb.e, (size_t)lb.n, sizeof *lb.e, listed_cmp);
    }
    for (int i = 0; same && i < la.n; i++) {
        same = !strcmp(la.e[i].name, lb.e[i].name) && la.e[i].kind == lb.e[i].kind && la.e[i].kind != PLAT_FS_LINK;
        if (!same) break;
        char *pa = join(a, la.e[i].name), *pb = join(b, lb.e[i].name);
        same = la.e[i].kind == PLAT_FS_DIR ? same_tree(pa, pb) : same_inode(pa, pb);
        free(pa);
        free(pb);
    }
    listing_free(la.e, la.n);
    listing_free(lb.e, lb.n);
    return same;
}

static int name_cmp(const void *a, const void *b) { return strcmp(a, b); }

// The backups' folder names (digits first), sorted: oldest first, as rotate_backup names them in creation order.
static int backup_names(char (*names)[48]) {
    int n = 0;
    Listing l;
    list_dir(backup_root, &l);
    for (int i = 0; i < l.n; i++)
        if (l.e[i].kind == PLAT_FS_DIR && l.e[i].name[0] >= '0' && l.e[i].name[0] <= '9' && strlen(l.e[i].name) < 48 &&
            n < 64)
            strcpy(names[n++], l.e[i].name);
    listing_free(l.e, l.n);
    qsort(names, (size_t)n, sizeof *names, name_cmp);
    return n;
}

// Move old to save-backup/<UTC time> and delete all backups but the newest SAVE_BACKUPS. The name always sorts after
// every existing one: a commit in the same millisecond as the newest backup (or after the clock went back) takes its
// time and the next -NNN suffix. A name that sorted lower would be the first one pruned, losing the backup just made.
static void rotate_backup(void) {
    if (!exists(old_root)) return;
    static char names[64][48];
    int n = backup_names(names);
    uint64_t ns = plat_unix_time_ns();
    struct tm t;
    plat_gmtime((int64_t)(ns / 1000000000u), &t);
    char name[48];
    int len = snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d-%03u", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                       t.tm_hour, t.tm_min, t.tm_sec, (unsigned)(ns / 1000000u % 1000u));
    const char *newest = n ? names[n - 1] : "";
    if (strcmp(name, newest) <= 0 && strlen(newest) >= (size_t)len) {
        unsigned k = 0;
        sscanf(newest + len, "-%u", &k);   // no suffix: k stays 0
        do   // until it sorts after newest, even after an unpadded "-1" of older builds
            snprintf(name, sizeof name, "%.*s-%03u", len, newest, ++k);
        while (strcmp(name, newest) <= 0 && k < 100000);
    }
    char *dst = join(backup_root, name);
    if (move_dir(old_root, dst, MOVE_TRIES)) {
        rt_log("fs: cannot move the replaced save to save-backup; deleted");
        remove_tree(old_root);
    }
    free(dst);
    n = backup_names(names);
    for (int i = 0; i + SAVE_BACKUPS < n; i++) {
        char *p = join(backup_root, names[i]);
        remove_tree(p);
        free(p);
    }
}

// The newest save: the finished commit waiting to be swapped in, else the committed save.
static const char *newest_save(void) { return *ready_root ? ready_root : save_root; }

// Steps (3) to (5): swap the finished commit ready_root in for the committed save. 0 on success; on failure
// ready_root stays the newest save. (3) is tried for 200 ms only: the commit is safe in ready_root, the next Commit
// or start tries again, and the game's next change to its save data waits for this one.
#define SWAP_TRIES 4
static int swap_in(void) {
    if (!*ready_root) return 0;
    rotate_backup();   // an old left by a failed (5): (3) needs its name
    if (exists(save_root) && move_dir(save_root, old_root, SWAP_TRIES)) {
        char why[512];
        plat_fs_busy_reason(save_root, why, sizeof why);
        rt_log("fs: save commit: the committed save cannot be replaced (%s); the commit waits in %s and the next "
               "Commit or start retries", *why ? why : "a file in it is in use", ready_root);
        return -1;
    }
    if (crashed()) return -1;
    if (move_dir(ready_root, save_root, MOVE_TRIES)) {
        rt_log("fs: save commit: cannot move the finished commit to the committed save; it waits in %s", ready_root);
        move_dir(old_root, save_root, MOVE_TRIES);   // else save/<uid> stays absent and the next start rolls forward
        return -1;
    }
    *ready_root = 0;
    if (crashed()) return -1;
    rotate_backup();
    return 0;
}

// The number n of a "ready-<n>" name, 0 for any other name.
static unsigned ready_number(const char *name) {
    char *end;
    if (strncmp(name, "ready-", 6) || name[6] < '0' || name[6] > '9') return 0;
    unsigned long n = strtoul(name + 6, &end, 10);
    return *end || n > 0xffffffffu ? 0 : (unsigned)n;
}

// Startup: the newest finished commit into ready_root; the older ones, which it supersedes, are deleted.
static void ready_scan(void) {
    Listing l;
    list_dir(tx_root, &l);
    *ready_root = 0, ready_seq = 0;
    for (int i = 0; i < l.n; i++)
        if (l.e[i].kind == PLAT_FS_DIR && ready_number(l.e[i].name) > ready_seq) ready_seq = ready_number(l.e[i].name);
    for (int i = 0; i < l.n; i++) {
        unsigned n = l.e[i].kind == PLAT_FS_DIR ? ready_number(l.e[i].name) : 0;
        char *p = n ? join(tx_root, l.e[i].name) : NULL;
        if (n == ready_seq && p) snprintf(ready_root, sizeof ready_root, "%s", p);
        else if (p) remove_tree(p);
        free(p);
    }
    listing_free(l.e, l.n);
}

// Startup: finish or roll back an interrupted Commit and drop uncommitted changes (the rule above).
static void save_recover(void) {
    const char *did = "nothing to recover";
    ready_scan();
    int has_save = exists(save_root);
    // fastball: an --import-save that stopped (fs_save_import). Between its two moves save/<uid> is absent and
    // nothing else is in save-tx: the finished copy goes in. Any other time it is an unfinished copy: deleted.
    if (exists(import_root)) {
        if (!has_save && !*ready_root && !exists(new_root) && !exists(old_root)) {
            if (move_dir(import_root, save_root, MOVE_TRIES)) rt_fatal("fs: cannot complete an interrupted save import");
            did = "completed an interrupted save import";
            has_save = 1;
        } else {
            if (remove_tree(import_root)) rt_fatal("fs: cannot delete an unfinished save import");
            did = "deleted an unfinished save import";
        }
    }
    if (exists(new_root) && !has_save && !*ready_root) {
        if (move_dir(new_root, save_root, MOVE_TRIES)) rt_fatal("fs: cannot complete an interrupted save commit");
        did = "completed an interrupted commit";
        has_save = 1;
    } else if (exists(new_root)) {
        if (remove_tree(new_root)) rt_fatal("fs: cannot delete an unfinished save commit");
        did = "rolled back an unfinished commit";
    }
    if (!has_save && !*ready_root && exists(old_root)) {
        if (move_dir(old_root, save_root, MOVE_TRIES)) rt_fatal("fs: cannot roll back an interrupted save commit");
        did = "rolled back an interrupted commit";
    } else if (exists(old_root))
        did = "moved the save an interrupted commit replaced to save-backup";
    rotate_backup();
    if (*ready_root)
        did = swap_in() ? "a finished commit still waits in save-tx; the game loads it from there"
                        : "completed a finished commit";
    if (exists(work_root)) {
        if (remove_tree_retry(work_root))
            rt_fatal("fs: cannot delete the old save working copy: %s (OS error %d)", remove_failed, remove_error);
        if (!strcmp(did, "nothing to recover"))
            did = "deleted the last run's working copy (changes after its last Commit)";
    }
    rt_log("host save recovery: %s", did);
}

// The first save mount: a fresh working copy of the newest save.
static void work_create(void) {
    int n = 0, linked = 0;
    uint64_t bytes = 0;
    if (remove_tree(work_root) || copy_tree(newest_save(), work_root, NULL, 0, &n, &linked, &bytes))
        rt_fatal("fs: cannot make the save working copy");
    rt_log("host save working copy: %d files, %.1f MiB", n, bytes / 1048576.0);
}

// The last save unmount: changes no Commit took are dropped.
static void work_discard(void) {
    if (plat_xchg32(&save_dirty, 0)) rt_log("fs: save unmounted without Commit; changes discarded");
    if (remove_tree(work_root)) rt_log("fs: save working copy not fully deleted (a file still open?)");
}

// Commit of the save mount, tx_lock held. Steps (1) to (5) above; a failure before (2) leaves the newest save as it
// was, one after it leaves the commit waiting in ready_root.
static uint32_t save_commit_locked(void) {
    uint64_t t0 = plat_now_ns(), bytes = 0;
    int nfiles = 0, linked = 0;
    tx_step = tx_dead = 0;
    const char *base = exists(newest_save()) ? newest_save() : NULL;
    if (remove_tree(new_root) || copy_tree(work_root, new_root, base, 1, &nfiles, &linked, &bytes)) {
        if (tx_dead) return FS_R_INVALID_ARGUMENT;
        rt_log("fs: save commit: copying the working copy failed; the newest save is unchanged");
        remove_tree(new_root);
        return FS_R_INVALID_ARGUMENT;
    }
    if (crashed()) return FS_R_INVALID_ARGUMENT;
    if (base && linked == nfiles && same_tree(new_root, base)) {   // the game rewrote equal bytes: no save, no backup
        remove_tree(new_root);
        plat_xchg32(&save_dirty, 0);
        int waits = swap_in();
        if (tx_dead) return FS_R_INVALID_ARGUMENT;
        rt_log("fs commit save: %d files, none changed, in %.0f ms%s", nfiles, (plat_now_ns() - t0) / 1e6,
               waits ? "; the newest save waits in save-tx" : "");
        return 0;
    }
    char ready[1100];
    snprintf(ready, sizeof ready, "%s/ready-%010u", tx_root, ready_seq + 1);
    if (move_dir(new_root, ready, MOVE_TRIES)) {
        rt_log("fs: save commit: cannot finish the staging copy; the newest save is unchanged");
        remove_tree(new_root);
        return FS_R_INVALID_ARGUMENT;
    }
    ready_seq++;
    if (*ready_root) remove_tree(ready_root);   // superseded: the working copy it was made from held all of it
    snprintf(ready_root, sizeof ready_root, "%s", ready);
    plat_xchg32(&save_dirty, 0);
    if (crashed()) return FS_R_INVALID_ARGUMENT;
    int waits = swap_in();
    if (tx_dead) return FS_R_INVALID_ARGUMENT;
    rt_log("fs commit save: %d files, %d changed (%.1f MiB written), %d unchanged linked, in %.0f ms%s", nfiles,
           nfiles - linked, bytes / 1048576.0, linked, (plat_now_ns() - t0) / 1e6, waits ? "; waits in save-tx" : "");
    return 0;
}

// The background commit: commit_pending from the game's Commit until the thread has run save_commit_locked.
static PlatMutex pend_lock = PLAT_MUTEX_INIT;
static PlatCond pend_cv = PLAT_COND_INIT;
static volatile int32_t commit_pending;

static void save_wait_commit(void) {
    if (!plat_cas32(&commit_pending, 0, 0)) return;
    plat_mutex_lock(&pend_lock);
    while (commit_pending) plat_cond_wait(&pend_cv, &pend_lock);
    plat_mutex_unlock(&pend_lock);
}

void fs_save_flush(void) { save_wait_commit(); }

// fastball: --import-save (save_import.c). The committed save becomes a copy of the save data files in src as
// another host keeps them. A Switch save is the same files on every host: MLB lists the save's folders and reads them
// by name (CARR<id>/SAVEINFO, SAVE_000, ...), and a Ryujinx save loaded unchanged into its Road to the Show career.
// The save it replaces moves whole to <profile>/save-before-import/<UTC time>, which no Commit prunes. Steps: (1) copy
// src to save-tx/import, every file flushed; (2) move save/<uid> to that backup; (3) move import to save/<uid>. A
// crash before (2) leaves the old save, one between (2) and (3) is finished by save_recover. Refused while another
// process holds the profile lock (a running game) or a finished commit still waits in save-tx. Runs before boot: no
// save is mounted. 0 on success; else -1, with the reason in out->why and the save as it was.
static void ready(void);
static void profile_lock_take(void);
int fs_save_import(const char *src, FsImport *out) {
    *out = (FsImport){0};
    profile_lock_take();   // before ready(): its save_recover would delete a running game's working copy
    if (profile_lock_busy) {
        snprintf(out->why, sizeof out->why, "the game is running on this profile: close it first");
        return -1;
    }
    ready();
    if (*ready_root) {
        snprintf(out->why, sizeof out->why, "the last save commit still waits in %s (a file of the save is open in "
                 "another program): close that program and try again", ready_root);
        return -1;
    }
    if (plat_fs_kind(src) != PLAT_FS_DIR) {
        snprintf(out->why, sizeof out->why, "%s is not a folder", src);
        return -1;
    }
    plat_mutex_lock(&tx_lock);
    int linked = 0, r = -1;
    if (remove_tree(import_root) || copy_tree(src, import_root, NULL, 1, &out->files, &linked, &out->bytes)) {
        snprintf(out->why, sizeof out->why, "copying %s failed", src);
        remove_tree(import_root);
        goto done;
    }
    if (!out->files) {
        snprintf(out->why, sizeof out->why, "%s holds no files", src);
        remove_tree(import_root);
        goto done;
    }
    Listing l;
    int has_old = !list_dir(save_root, &l) && l.n > 0;
    listing_free(l.e, l.n);
    if (has_old) {
        char dir[PATH_CAP], name[48];
        uint64_t ns = plat_unix_time_ns();
        struct tm t;
        plat_gmtime((int64_t)(ns / 1000000000u), &t);
        snprintf(name, sizeof name, "%04d%02d%02d-%02d%02d%02d-%03u", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                 t.tm_hour, t.tm_min, t.tm_sec, (unsigned)(ns / 1000000u % 1000u));
        snprintf(dir, sizeof dir, "%s/save-before-import", profile);
        make_dirs(dir);
        snprintf(out->backup, sizeof out->backup, "%s/%s", dir, name);
        if (move_dir(save_root, out->backup, MOVE_TRIES)) {
            char busy[512];
            plat_fs_busy_reason(save_root, busy, sizeof busy);
            snprintf(out->why, sizeof out->why, "the current save cannot be moved aside (%s)",
                     *busy ? busy : "a file in it is open in another program");
            *out->backup = 0;
            remove_tree(import_root);
            goto done;
        }
    } else if (remove_tree(save_root)) {   // the empty folder of a profile that has no save yet
        snprintf(out->why, sizeof out->why, "cannot replace the empty save folder %s", save_root);
        remove_tree(import_root);
        goto done;
    }
    if (move_dir(import_root, save_root, MOVE_TRIES)) {
        size_t n = (size_t)snprintf(out->why, sizeof out->why, "cannot move the imported copy into %s", save_root);
        if (has_old && move_dir(out->backup, save_root, MOVE_TRIES)) {   // the old save could not go back either
            snprintf(out->why + n, sizeof out->why - n, "; the old save is in %s, the import in %s (the next start "
                     "puts the import in place)", out->backup, import_root);
            out->stuck = 1;
        } else {
            if (!has_old) make_dirs(save_root);
            remove_tree(import_root);
        }
        *out->backup = 0;
        goto done;
    }
    r = 0;
    rt_log("fs: save imported from %s: %d files, %.1f MiB%s%s", src, out->files, out->bytes / 1048576.0,
           *out->backup ? "; the previous save is in " : "", out->backup);
done:
    plat_mutex_unlock(&tx_lock);
    if (r) rt_log("fs: save import failed: %s", out->why);
    return r;
}

static uint32_t commit_run(void) {
    plat_mutex_lock(&tx_lock);
    uint32_t r = save_commit_locked();
    plat_dec32(&save_writers);
    plat_mutex_unlock(&tx_lock);
    plat_mutex_lock(&pend_lock);
    commit_pending = 0;
    plat_mutex_unlock(&pend_lock);
    plat_cond_broadcast(&pend_cv);
    return r;
}

static void commit_main(void *arg) {
    (void)arg;
    commit_run();
    return;
}

static uint32_t save_commit(void) {
    save_wait_commit();   // the previous Commit
    static int quiet;
    if (!plat_cas32(&save_dirty, 0, 0)) {
        if (!quiet++) rt_log("fs commit save: nothing changed (logged once)");
        return 0;
    }
    plat_inc32(&save_writers);   // fs_save_busy until the thread is done
    plat_xchg32(&commit_pending, 1);
    if (plat_thread_spawn(commit_main, NULL, "save commit", 0)) {
        return 0;
    }
    rt_log("fs: cannot start the save commit thread; committing on the game thread");
    return commit_run();
}

// ---- storage roots
// fastball: the profile lock, held until the process ends: --import-save must not replace the save under a
// running game, nor run save_recover, which deletes that game's working copy. A second game on the same profile still
// starts, as before, and logs it. Taken once, by storage_init or first by fs_save_import.
static void profile_lock_take(void) {
    static int taken;
    if (taken++) return;
    if (!*profile)
        need(snprintf(profile, sizeof profile, "%s", rt_config()->profile) < (int)sizeof profile, "profile path");
    make_dirs(profile);
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/profile.lock", profile);
    int err = 0;
    if (!(profile_lock = plat_fs_lock(p, &err))) {
        profile_lock_busy = err == PLAT_FS_BUSY;
        rt_log(profile_lock_busy ? "fs: another mlb.exe is running on this profile (%s is held)"
                                 : "fs: cannot make the profile lock %s; nothing is locked", p);
    }
}

static void storage_init(void *param) {
    (void)param;
    if (!*profile)
        need(snprintf(profile, sizeof profile, "%s", rt_config()->profile) < (int)sizeof profile, "profile path");
    uint64_t uid[2];
    char p[PATH_CAP];
    make_dirs(profile);
    profile_lock_take();
    account_uid(uid);
    snprintf(p, sizeof p, "%s/save", profile);
    make_dirs(p);
    need(snprintf(save_root, sizeof save_root, "%s/save/%016llx%016llx", profile, (unsigned long long)uid[1],
                  (unsigned long long)uid[0]) < (int)sizeof save_root, "save path");
    need(snprintf(tx_root, sizeof tx_root, "%s/save-tx", profile) < (int)sizeof tx_root, "save path");
    snprintf(work_root, sizeof work_root, "%s/work", tx_root);
    snprintf(backup_root, sizeof backup_root, "%s/save-backup", profile);
    make_dirs(tx_root);
    make_dirs(backup_root);
    snprintf(new_root, sizeof new_root, "%s/new", tx_root);
    snprintf(old_root, sizeof old_root, "%s/old", tx_root);
    snprintf(import_root, sizeof import_root, "%s/import", tx_root);
    save_recover();   // before save_root is created: an absent save_root means a commit to complete
    make_dirs(save_root);
    snprintf(p, sizeof p, "%s/temp", profile);
    remove_tree(p);
    make_dirs(p);
    snprintf(p, sizeof p, "%s/cache", profile);
    make_dirs(p);
    plat_xchg32(&storage_ready, 1);
    rt_log("host save storage: %s", save_root);
    return;
}

static void ready(void) {
    static PlatOnce once = PLAT_ONCE_INIT;
    plat_once(&once, storage_init, NULL);
}

void fs_host_set_profile(const char *dir) {
    if (plat_cas32(&storage_ready, 0, 0)) rt_fatal("fs_host_set_profile after the first storage use");
    need(dir && snprintf(profile, sizeof profile, "%s", dir) < (int)sizeof profile, "profile path");
}

// ---- mounts and paths
static uint32_t mount_add(const char *name, int kind, const char *root) {
    if (!name || !*name || strlen(name) >= 32 || strchr(name, ':') || strchr(name, '/')) return FS_R_INVALID_ARGUMENT;
    plat_mutex_lock(&lock);
    int slot = -1;
    for (int i = 0; i < NMOUNTS; i++) {
        if (mounts[i].kind && !strcmp(mounts[i].name, name)) {
            plat_mutex_unlock(&lock);
            return FS_R_ALREADY_EXISTS;
        }
        if (!mounts[i].kind && slot < 0) slot = i;
    }
    if (slot < 0) rt_fatal("fs mount table full");
    mounts[slot].kind = kind;
    strcpy(mounts[slot].name, name);
    snprintf(mounts[slot].root, sizeof mounts[slot].root, "%s", root);
    plat_mutex_unlock(&lock);
    rt_log("fs mount %s: -> %s", name, root);
    return 0;
}

// A path component every host stores under exactly this name: not empty, ".", "..", no reserved character, no trailing
// dot or space (Windows strips those, which would alias two names).
static int component_ok(const char *c, size_t n) {
    if (!n || (n == 1 && c[0] == '.') || (n == 2 && c[0] == '.' && c[1] == '.')) return 0;
    if (c[n - 1] == '.' || c[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)c[i] < 32 || strchr("\\:*?\"<>|", c[i])) return 0;
    return 1;
}

// "name:/a/b" -> host path; 0 on success; *kind = the mount's kind (M_SAVE ...) when kind is given.
static uint32_t host_path_kind(const char *path, char *out, size_t cap, int *kind) {
    if (!path) return FS_R_INVALID_PATH;
    const char *colon = strchr(path, ':');
    if (!colon || colon[1] != '/') return FS_R_INVALID_PATH;
    size_t n = (size_t)(colon - path);
    char root[1024];
    int k = M_FREE;
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (mounts[i].kind && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n)) {
            strcpy(root, mounts[i].root);
            k = mounts[i].kind;
        }
    plat_mutex_unlock_shared(&lock);
    if (k == M_FREE) return FS_R_PATH_NOT_FOUND;
    if (kind) *kind = k;
    // fastball: the path is normalised as nn::fs's PathNormalizer does: repeated separators collapse to one (MLB
    // opens "FS_Rom:/NSW//LIVECONTENT"), "." is dropped and ".." takes back the component before it; a ".." above the
    // mount's root is refused. MLB reaches shared data this way: "FS_Rom:/NSW/../CROSS/STADIUMS/AUDIO/LAD/
    // AUDIOEMITTER.BIN" (the stadium's sound emitters: crowd, organ, PA) and "FS_Rom:/NSW/STADIUMS/LAD/../COMMON/
    // SCOREBOARD/PITCHCLOCK.NSW". Refusing them left the ballpark without its ambient sound.
    char rest[PATH_CAP];
    size_t rn = 0;
    for (const char *q = colon + 1; *q;) {
        while (*q == '/') q++;
        if (!*q) break;
        const char *e = strchr(q, '/');
        size_t len = e ? (size_t)(e - q) : strlen(q);
        if (len == 1 && q[0] == '.') {
        } else if (len == 2 && q[0] == '.' && q[1] == '.') {
            if (!rn) return FS_R_INVALID_PATH;
            while (rest[rn - 1] != '/') rn--;
            rn--;
        } else {
            if (!component_ok(q, len) || rn + 1 + len >= sizeof rest) return FS_R_INVALID_PATH;
            rest[rn++] = '/';
            memcpy(rest + rn, q, len);
            rn += len;
        }
        q += len;
    }
    if (!rn) rest[rn++] = '/';
    rest[rn] = 0;
    if (snprintf(out, cap, "%s%s", root, rest) >= (int)cap) return FS_R_INVALID_PATH;
    size_t L = strlen(out);
    while (L > strlen(root) && out[L - 1] == '/') out[--L] = 0;
    return 0;
}

static uint32_t host_path(const char *path, char *out, size_t cap) { return host_path_kind(path, out, cap, NULL); }

// A change to save data through a path (create, delete, rename, directories) marks the save as in progress.
static uint32_t save_change_path(const char *path, char *out, size_t cap) {
    int kind = M_FREE;
    uint32_t r = host_path_kind(path, out, cap, &kind);
    if (!r && (kind == M_SAVE_RO || kind == M_ROM)) return FS_R_UNSUPPORTED_OPEN;
    if (!r && kind == M_SAVE) save_touched();
    return r;
}

static uint32_t fs_result(int e) {
    switch (e) {
    case PLAT_FS_OK: return 0;
    case PLAT_FS_NOT_FOUND: return FS_R_PATH_NOT_FOUND;
    case PLAT_FS_EXISTS: return FS_R_ALREADY_EXISTS;
    case PLAT_FS_NOT_EMPTY: return FS_R_NOT_EMPTY;
    default: return FS_R_INVALID_ARGUMENT;
    }
}

// ---- storage mounts
static int my_uid(const uint64_t *uid) {
    uint64_t me[2];
    account_uid(me);
    return uid && uid[0] == me[0] && uid[1] == me[1];
}

static uint32_t mount_save(const char *name, const uint64_t *uid) {
    ready();
    if (!my_uid(uid)) return FS_R_TARGET_NOT_FOUND;
    plat_mutex_lock(&tx_lock);
    if (!save_mounts) work_create();
    uint32_t r = mount_add(name, M_SAVE, work_root);
    if (!r) save_mounts++;
    else if (!save_mounts) remove_tree(work_root);
    plat_mutex_unlock(&tx_lock);
    return r;
}

static uint8_t save_exists(uint64_t application, const uint64_t *uid) {
    ready();
    return application == fs_title.program_id && my_uid(uid);
}

// Other titles' save data (data-link bonuses) is not present on this host.
static uint32_t mount_save_ro(const char *name, uint64_t application, const uint64_t *uid) {
    if (!save_exists(application, uid)) return FS_R_TARGET_NOT_FOUND;
    return mount_add(name, M_SAVE_RO, save_root);
}

static uint32_t mount_temp(const char *name) {
    ready();
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/temp", profile);
    return mount_add(name, M_TEMP, p);
}

static void cache_dir(int index, char *out, size_t cap) { snprintf(out, cap, "%s/cache/%d", profile, index); }

static uint32_t create_cache(int index, int64_t size, int64_t journal) {
    ready();
    if (index < 0 || size < 0 || journal < 0) return FS_R_INVALID_ARGUMENT;
    char p[PATH_CAP];
    cache_dir(index, p, sizeof p);
    return fs_result(plat_fs_mkdir(p));
}

static uint32_t delete_cache(int index) {
    ready();
    char p[PATH_CAP];
    cache_dir(index, p, sizeof p);
    return remove_tree(p) ? FS_R_TARGET_NOT_FOUND : 0;
}

static uint32_t mount_cache(const char *name, int index) {
    ready();
    char p[PATH_CAP];
    cache_dir(index, p, sizeof p);
    if (plat_fs_kind(p) != PLAT_FS_DIR) return FS_R_TARGET_NOT_FOUND;
    return mount_add(name, M_CACHE, p);
}

typedef struct { int32_t index; uint8_t reserved[0x1c]; } CacheStorageInfo;
_Static_assert(sizeof(CacheStorageInfo) == 0x20, "nn::fs::CacheStorageInfo");
typedef struct { int list[64]; int count, next; } CacheList;

static uint32_t open_cache_list(uint64_t *out) {
    ready();
    if (!out) return FS_R_INVALID_ARGUMENT;
    CacheList *l = calloc(1, sizeof *l);
    need(l != NULL, "cache list allocation");
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/cache", profile);
    Listing ls;
    list_dir(p, &ls);
    for (int i = 0; i < ls.n; i++) {
        char *end;
        long v = strtol(ls.e[i].name, &end, 10);
        if (ls.e[i].kind == PLAT_FS_DIR && *ls.e[i].name && !*end && v >= 0 && l->count < 64) l->list[l->count++] = (int)v;
    }
    listing_free(ls.e, ls.n);
    *out = (uint64_t)(uintptr_t)l;
    return 0;
}

static uint32_t read_cache_list(int *count, CacheStorageInfo *out, uint64_t h, int max) {
    CacheList *l = (CacheList *)(uintptr_t)h;
    if (!l || !count || max < 0 || (!out && max)) return FS_R_INVALID_ARGUMENT;
    int n = 0;
    while (n < max && l->next < l->count) {
        memset(&out[n], 0, sizeof *out);
        out[n++].index = l->list[l->next++];
    }
    *count = n;
    return 0;
}

static void close_cache_list(uint64_t h) { free((void *)(uintptr_t)h); }

static void unmount(const char *name) {
    save_wait_commit();   // else the working copy could go before a Commit queued just now has copied it
    plat_mutex_lock(&tx_lock);
    plat_mutex_lock(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (name && mounts[i].kind && !strcmp(mounts[i].name, name)) {
            int save = mounts[i].kind == M_SAVE;
            mounts[i].kind = M_FREE;
            plat_mutex_unlock(&lock);
            if (save && !--save_mounts) work_discard();
            plat_mutex_unlock(&tx_lock);
            rt_log("fs unmount %s", name);
            return;
        }
    plat_mutex_unlock(&lock);
    plat_mutex_unlock(&tx_lock);
    rt_fatal("fs: unmount of unknown mount %s", name ? name : "(null)");
}

static uint32_t commit(const char *name) {
    int found = 0, save = 0;
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (name && mounts[i].kind && !strcmp(mounts[i].name, name)) found = 1, save = mounts[i].kind == M_SAVE;
    plat_mutex_unlock_shared(&lock);
    if (!found) return FS_R_PATH_NOT_FOUND;
    if (save) return save_commit();   // durability comes from the staging copy, not the working copy's handles
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < NFILES; i++)
        if (files[i].h && (files[i].mode & FS_OPEN_WRITE)) plat_file_fsync(files[i].h);
    plat_mutex_unlock_shared(&lock);
    return 0;
}

// ---- files
static File *file_of(uint64_t h, PlatFile **fh, int *mode) {
    if (h < FILE_BASE || h >= FILE_BASE + NFILES) return NULL;
    plat_mutex_lock_shared(&lock);
    File *f = &files[h - FILE_BASE];
    PlatFile *v = f->h;
    if (fh) *fh = f->h;
    if (mode) *mode = f->mode;
    plat_mutex_unlock_shared(&lock);
    return v ? f : NULL;
}

// fastball: the graphics back end's record of guest memory the CPU wrote (vk_memory.c); a no-op without one.
__attribute__((weak)) void rt_host_wrote(uint64_t addr, uint64_t size) { (void)addr, (void)size; }

// Positional reads: bytes read at off, 0 at the end, -1 on an error.
static int64_t host_read_at(PlatFile *f, void *buf, uint32_t n, uint64_t off) {
    mem_thaw((uintptr_t)buf, n);   // the kernel's write would fail on a frozen page instead of faulting
    int64_t got = plat_file_pread(f, buf, n, off);
    // fastball: the kernel's writes are reported, not left to the write watch: textures and buffers the game
    // streams from disk into memory it already uses must be uploaded again
    if (got > 0) rt_host_wrote((uintptr_t)buf, (uint64_t)got);
    if (got == -2)   // a read-only page in the buffer: a frozen region missed its thaw
        rt_log("fs: read of %u bytes into %p refused: the buffer is not writable", n, buf);
    return got < 0 ? -1 : got;
}

static uint32_t open_file(uint64_t *out, const char *path, int mode) {
    fs_trace(mode & FS_OPEN_WRITE ? "write" : "read", path);
    if (!out || !(mode & (FS_OPEN_READ | FS_OPEN_WRITE)) || (mode & ~7)) return FS_R_INVALID_ARGUMENT;
    char p[PATH_CAP];
    int kind = M_FREE;
    uint32_t r = host_path_kind(path, p, sizeof p, &kind);
    if (r) return r;
    if (plat_fs_kind(p) != PLAT_FS_FILE) return FS_R_PATH_NOT_FOUND;
    if ((kind == M_SAVE_RO || kind == M_ROM) && (mode & FS_OPEN_WRITE)) return FS_R_UNSUPPORTED_OPEN;
    int save_writer = kind == M_SAVE && (mode & FS_OPEN_WRITE);
    int err = 0;
    PlatFile *f = plat_fs_open(p, (mode & FS_OPEN_WRITE) != 0, &err);
    if (!f) return fs_result(err);
    plat_mutex_lock(&lock);
    for (int i = 0; i < NFILES; i++)
        if (!files[i].h) {
            files[i] = (File){f, mode, save_writer, (rt_trace & RT_TRACE_FS) && path ? strdup(path) : NULL, 0, 0};
            *out = FILE_BASE + (uint64_t)i;
            plat_mutex_unlock(&lock);
            if (save_writer) {
                plat_inc32(&save_writers);
                save_touched();
            }
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("fs: host file handle table full (%d)", NFILES);
}

static uint32_t read_file(uint64_t *out, uint64_t h, int64_t offset, void *buffer, uint64_t size) {
    PlatFile *f;
    int mode;
    File *file;
    if (!out || offset < 0 || (!buffer && size) || !(file = file_of(h, &f, &mode))) return FS_R_INVALID_ARGUMENT;
    if (!(mode & FS_OPEN_READ)) return FS_R_OPEN_MODE;
    uint64_t done = 0;
    while (done < size) {
        uint32_t chunk = size - done > 0x40000000 ? 0x40000000 : (uint32_t)(size - done);
        int64_t n = host_read_at(f, (uint8_t *)buffer + done, chunk, (uint64_t)offset + done);
        if (n < 0) rt_fatal("fs: host save read failure");
        if (!n) break;
        done += (uint64_t)n;
    }
    *out = done;
    if (file->trace) {   // the first reads of each file, then every 64th
        uint64_t k = __atomic_add_fetch(&file->reads, 1, __ATOMIC_RELAXED);
        uint64_t b = __atomic_add_fetch(&file->bytes, done, __ATOMIC_RELAXED);
        if (k <= 3 || k % 64 == 0)
            rt_log("fs read #%llu of %s: %#llx bytes at %#llx (got %#llx; %.1f MiB so far)", (unsigned long long)k,
                   file->trace, (unsigned long long)size, (unsigned long long)offset, (unsigned long long)done,
                   b / 1048576.0);
    }
    return 0;
}

static uint32_t read_exact(uint64_t h, int64_t off, void *buf, uint64_t size) {
    uint64_t n = 0;
    uint32_t r = read_file(&n, h, off, buf, size);
    return r ? r : n == size ? 0 : FS_R_INVALID_ARGUMENT;
}

static uint32_t write_file(uint64_t h, int64_t offset, const void *buffer, uint64_t size, const int32_t *option) {
    PlatFile *f;
    int mode;
    File *file = offset < 0 || (!buffer && size) ? NULL : file_of(h, &f, &mode);
    if (!file) return FS_R_INVALID_ARGUMENT;
    if (!(mode & FS_OPEN_WRITE)) return FS_R_OPEN_MODE;
    PlatFileId id;
    if (!plat_file_id(f, &id)) rt_fatal("fs: host save size");
    if ((uint64_t)offset + size > id.size && !(mode & FS_OPEN_APPEND)) return FS_R_APPEND_REQUIRED;
    if (file->save) save_touched();
    for (uint64_t done = 0; done < size;) {
        uint32_t chunk = size - done > 0x40000000 ? 0x40000000 : (uint32_t)(size - done);
        int64_t put = plat_file_pwrite(f, (const uint8_t *)buffer + done, chunk, (uint64_t)offset + done);
        if (put <= 0) rt_fatal("fs: host save write failure");
        done += (uint64_t)put;
    }
    if (option && (*option & 1)) plat_file_fsync(f);   // WriteOptionFlag_Flush
    return 0;
}

static uint32_t file_size(int64_t *out, uint64_t h) {
    PlatFile *f;
    PlatFileId id;
    if (!out || !file_of(h, &f, NULL) || !plat_file_id(f, &id)) return FS_R_INVALID_ARGUMENT;
    *out = (int64_t)id.size;
    return 0;
}

static uint32_t set_file_size(uint64_t h, int64_t size) {
    PlatFile *f;
    int mode;
    File *file = size < 0 ? NULL : file_of(h, &f, &mode);
    if (!file) return FS_R_INVALID_ARGUMENT;
    if (!(mode & FS_OPEN_WRITE)) return FS_R_OPEN_MODE;
    if (file->save) save_touched();
    return fs_result(plat_file_set_size(f, (uint64_t)size));
}

static uint32_t flush_file(uint64_t h) {
    PlatFile *f;
    int mode;
    if (!file_of(h, &f, &mode)) return FS_R_INVALID_ARGUMENT;
    if (mode & FS_OPEN_WRITE) plat_file_fsync(f);
    return 0;
}

static void close_file(uint64_t h) {
    if (!file_of(h, NULL, NULL)) rt_fatal("fs: close of invalid file handle %#llx", (unsigned long long)h);
    plat_mutex_lock(&lock);
    PlatFile *f = files[h - FILE_BASE].h;
    int save = files[h - FILE_BASE].save;
    File closed = files[h - FILE_BASE];
    files[h - FILE_BASE] = (File){0};
    plat_mutex_unlock(&lock);
    plat_file_close(f);
    if (closed.trace) {
        rt_log("fs close %s: %llu reads, %.2f MiB", closed.trace, (unsigned long long)closed.reads,
               closed.bytes / 1048576.0);
        free(closed.trace);
    }
    if (save) {
        save_touched();
        plat_dec32(&save_writers);
    }
}

static uint32_t create_file(const char *path, int64_t size) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (size < 0) return FS_R_INVALID_ARGUMENT;
    int err = 0;
    PlatFile *f = plat_fs_create(p, &err);
    if (!f) return fs_result(err);
    err = size ? plat_file_set_size(f, (uint64_t)size) : 0;
    plat_file_close(f);
    return fs_result(err);
}

static uint32_t delete_file(const char *path) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (plat_fs_kind(p) == PLAT_FS_DIR) return FS_R_INVALID_ARGUMENT;   // DeleteFile on a directory
    return fs_result(plat_fs_unlink(p));
}

static uint32_t rename_file(const char *from, const char *to) {
    char a[PATH_CAP], b[PATH_CAP];
    uint32_t r = save_change_path(from, a, sizeof a);
    if (!r) r = host_path(to, b, sizeof b);
    if (r) return r;
    size_t n = strcspn(from, ":");
    if (strcspn(to, ":") != n || strncmp(from, to, n)) return FS_R_INVALID_PATH;   // across mounts
    if (exists(b)) return FS_R_ALREADY_EXISTS;
    return fs_result(plat_fs_rename(a, b, 0));
}

static uint32_t entry_type(int *out, const char *path) {
    char p[PATH_CAP];
    uint32_t r = host_path(path, p, sizeof p);
    if (r) return r;
    if (!out) return FS_R_INVALID_ARGUMENT;
    int kind = plat_fs_kind(p);
    if (kind == PLAT_FS_MISSING) return FS_R_PATH_NOT_FOUND;
    *out = kind == PLAT_FS_DIR ? FS_ENTRY_DIRECTORY : FS_ENTRY_FILE;
    return 0;
}

// ---- directories
typedef struct { char name[769]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "nn::fs::DirectoryEntry");

static uint32_t create_directory(const char *path) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    return fs_result(plat_fs_mkdir(p));
}

static uint32_t delete_recursive(const char *path) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (plat_fs_kind(p) != PLAT_FS_DIR) return FS_R_PATH_NOT_FOUND;
    return remove_tree(p) ? FS_R_INVALID_ARGUMENT : 0;
}

static uint32_t open_directory(uint64_t *out, const char *path, int mode) {
    char p[PATH_CAP];
    fs_trace("dir", path);
    uint32_t r = host_path(path, p, sizeof p);
    if (r) return r;
    if (!out || mode < 1 || mode > 3) return FS_R_INVALID_ARGUMENT;
    if (plat_fs_kind(p) != PLAT_FS_DIR) return FS_R_PATH_NOT_FOUND;
    Listing l;
    int e = list_dir(p, &l);
    if (e) return fs_result(e);
    plat_mutex_lock(&lock);
    for (int i = 0; i < NDIRS; i++)
        if (!dirs[i].used) {
            dirs[i] = (Dir){l.e, l.n, 0, mode, 1};
            *out = DIR_BASE + (uint64_t)i;
            plat_mutex_unlock(&lock);
            return 0;
        }
    plat_mutex_unlock(&lock);
    rt_fatal("fs: host directory handle table full (%d)", NDIRS);
}

static uint32_t read_directory(int64_t *count, DirectoryEntry *out, uint64_t h, int64_t max) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !count || max < 0 || (!out && max)) return FS_R_INVALID_ARGUMENT;
    Dir *d = &dirs[h - DIR_BASE];
    if (!d->used) return FS_R_INVALID_ARGUMENT;
    int64_t n = 0;
    while (n < max && d->next < d->n) {
        const Listed *e = &d->e[d->next++];
        size_t k = strlen(e->name) + 1;
        if (k > 769) continue;   // over 768 bytes: nn::fs cannot name it
        int is_dir = e->kind == PLAT_FS_DIR;
        if (!(d->mode & (is_dir ? 1 : 2))) continue;
        memset(&out[n], 0, sizeof *out);
        memcpy(out[n].name, e->name, k);
        out[n].type = is_dir ? FS_ENTRY_DIRECTORY : FS_ENTRY_FILE;
        out[n].size = is_dir ? 0 : (int64_t)e->size;
        n++;
    }
    *count = n;
    return 0;
}

static void close_directory(uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !dirs[h - DIR_BASE].used)
        rt_fatal("fs: close of invalid directory handle %#llx", (unsigned long long)h);
    plat_mutex_lock(&lock);
    Dir d = dirs[h - DIR_BASE];
    dirs[h - DIR_BASE] = (Dir){0};
    plat_mutex_unlock(&lock);
    listing_free(d.e, d.n);
}

// ---- fastball: the RomFS mount, save data size, free space, and the overloads MLB imports beyond TotK's
void fs_trace(const char *what, const char *path) {
    if (rt_trace & RT_TRACE_FS) rt_log("fs %s %s", what, path ? path : "(null)");
}

// The extracted RomFS: tools/romfs_dump.py writes <romfs>.complete once every listed file is there at its size.
static uint32_t mount_rom(const char *name) {
    const char *dir = rt_config()->romfs;
    char marker[PATH_CAP];
    need(snprintf(marker, sizeof marker, "%s.complete", dir) < (int)sizeof marker, "RomFS path");
    if (plat_fs_kind(dir) != PLAT_FS_DIR || plat_fs_kind(marker) != PLAT_FS_FILE)
        rt_fatal("fs: no complete RomFS at %s (tools/romfs_dump.py --extract-all writes it and %s)", dir, marker);
    return mount_add(name, M_ROM, dir);
}

static int64_t tree_bytes(const char *dir) {
    Listing l;
    if (list_dir(dir, &l)) return 0;
    int64_t n = 0;
    for (int i = 0; i < l.n; i++) {
        if (l.e[i].kind == PLAT_FS_DIR) {
            char *sub = join(dir, l.e[i].name);
            n += tree_bytes(sub);
            free(sub);
        } else n += (int64_t)l.e[i].size;
    }
    listing_free(l.e, l.n);
    return n;
}

// The user save's size and journal size: the NACP's, raised by ExtendSaveData and kept in <profile>/save-size.
static void save_size_get(int64_t v[2]) {
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/save-size", profile);
    v[0] = fs_title.save_size, v[1] = fs_title.save_journal;
    if (plat_file_size(p) <= 0) return;
    uint64_t got = 0;
    uint8_t *b = plat_file_read(p, &got);
    if (!b || got != 16) rt_fatal("fs: %s is not a save size record (16 bytes)", p);
    memcpy(v, b, 16);
    free(b);
}

static uint32_t get_save_size(int64_t *size, int64_t *journal, const uint64_t *uid) {
    ready();
    if (!size || !journal) return FS_R_INVALID_ARGUMENT;
    if (!my_uid(uid)) return FS_R_TARGET_NOT_FOUND;
    int64_t v[2];
    save_size_get(v);
    *size = v[0], *journal = v[1];
    return 0;
}

// Shrinking or growing past the NACP maximum: what Horizon answers is not known yet, so it stops here.
static uint32_t extend_save(const uint64_t *uid, int64_t size, int64_t journal) {
    ready();
    if (!my_uid(uid)) return FS_R_TARGET_NOT_FOUND;
    int64_t v[2];
    save_size_get(v);
    if (size < v[0] || journal < v[1] || size > fs_title.save_size_max || journal > fs_title.save_journal_max)
        rt_fatal("fs: ExtendSaveData to %#llx/%#llx from %#llx/%#llx (NACP max %#llx/%#llx)", (unsigned long long)size,
                 (unsigned long long)journal, (unsigned long long)v[0], (unsigned long long)v[1],
                 (unsigned long long)fs_title.save_size_max, (unsigned long long)fs_title.save_journal_max);
    int64_t nv[2] = {size, journal};
    char p[PATH_CAP];
    snprintf(p, sizeof p, "%s/save-size", profile);
    need(plat_file_write_atomic(p, nv, sizeof nv), "save size record");
    rt_log("fs: save data extended to %#llx (journal %#llx)", (unsigned long long)size, (unsigned long long)journal);
    return 0;
}

// Free space of a mount: its size less what its files take (the RomFS has none).
// fastball: a bare mount name counts as its root. MLB asks GetFreeSpaceSize("savedata") (main+0xd76904) and reads
// a failure as 0 bytes free; it then grows the save 8 MiB at a time and, once past 0x7E00000, unmounts it without
// mounting it again (main+0xd76940), so the next check unmounted an unknown mount and stopped [C, disassembly].
static uint32_t free_space(int64_t *out, const char *path) {
    char p[PATH_CAP], named[PATH_CAP];
    if (path && !strchr(path, ':') && strlen(path) + 3 <= sizeof named) {
        snprintf(named, sizeof named, "%s:/", path);
        path = named;
    }
    int kind = M_FREE;
    uint32_t r = host_path_kind(path, p, sizeof p, &kind);
    if (r) return r;
    if (!out) return FS_R_INVALID_ARGUMENT;
    size_t n = strcspn(path, ":");
    char root[PATH_CAP];
    root[0] = 0;
    plat_mutex_lock_shared(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (mounts[i].kind && strlen(mounts[i].name) == n && !strncmp(mounts[i].name, path, n))
            snprintf(root, sizeof root, "%s", mounts[i].root);
    plat_mutex_unlock_shared(&lock);
    int64_t total = 0, v[2];
    if (kind == M_SAVE || kind == M_SAVE_RO) save_size_get(v), total = v[0];
    else if (kind == M_TEMP) total = fs_title.temp_size;
    int64_t used = kind == M_ROM ? 0 : tree_bytes(root);
    *out = total > used ? total - used : 0;
    return 0;
}

static uint32_t delete_directory(const char *path) {
    char p[PATH_CAP];
    uint32_t r = save_change_path(path, p, sizeof p);
    if (r) return r;
    if (plat_fs_kind(p) != PLAT_FS_DIR) return FS_R_PATH_NOT_FOUND;
    return fs_result(plat_fs_rmdir(p));
}

static uint32_t directory_entry_count(int64_t *out, uint64_t h) {
    if (h < DIR_BASE || h >= DIR_BASE + NDIRS || !out) return FS_R_INVALID_ARGUMENT;
    plat_mutex_lock_shared(&lock);
    Dir *d = &dirs[h - DIR_BASE];
    int64_t n = -1;
    if (d->used) {
        n = 0;
        for (int i = 0; i < d->n; i++)
            if (strlen(d->e[i].name) < 769 && (d->mode & (d->e[i].kind == PLAT_FS_DIR ? 1 : 2))) n++;
    }
    plat_mutex_unlock_shared(&lock);
    if (n < 0) return FS_R_INVALID_ARGUMENT;
    *out = n;
    return 0;
}

// ---- overrides: AAPCS64 thunks. FileHandle, DirectoryHandle, CacheStorageListHandle
// and ApplicationId are 8-byte structs passed in one register; Uid, ReadOption and WriteOption by reference.
#define I32(i) ((int32_t)arg_x(c, i))
#define I64(i) ((int64_t)arg_x(c, i))
// fastball: MLB_FS_STACK=<text> (environment, with MLB_TRACE=fs): the game's call stack at each open, read and
// close of a file whose path contains <text>, on one line (frame records through x29). Followed why the audio engine
// read one header of each song and closed it again.
static void fs_stack(Cpu *c, const char *what, const char *path) {
    static const char *want;
    static int init;
    if (!init) want = getenv("MLB_FS_STACK"), init = 1;
    if (!want || !*want || !path || !strstr(path, want)) return;
    char line[1600], d[96];
    size_t n = (size_t)snprintf(line, sizeof line, "fs stack %s %s: %s", what, path, ldr_describe(c->x[30], d, sizeof d));
    uint64_t fp = c->x[29];
    for (int k = 0; k < 12 && fp && !(fp & 7) && plat_mem_readable(fp, 16) && n < sizeof line - 100; k++) {
        uint64_t next = LD64(fp), lr = LD64(fp + 8);
        n += (size_t)snprintf(line + n, sizeof line - n, " < %s", ldr_describe(lr, d, sizeof d));
        if (next <= fp || next - fp > (64u << 20)) break;
        fp = next;
    }
    rt_log("%s", line);
}
static const char *traced_path(uint64_t h) {
    File *f = file_of(h, NULL, NULL);
    return f ? f->trace : NULL;
}

// fastball: under MLB_TRACE=fs a failed open is logged with its result too (missing streamed audio was traced so)
static void ov_open_file(Cpu *c) {
    const char *path = arg_p(c, 1);
    if (rt_trace & RT_TRACE_FS) fs_stack(c, "open", path);
    uint32_t r = open_file(arg_p(c, 0), path, I32(2));
    if (r && (rt_trace & RT_TRACE_FS)) rt_log("fs open failed %#x: %s", r, path ? path : "(null)");
    ret_x(c, r);
}
static void ov_file_size(Cpu *c) { ret_x(c, file_size(arg_p(c, 0), arg_x(c, 1))); }
static void ov_close_file(Cpu *c) {
    if (rt_trace & RT_TRACE_FS) fs_stack(c, "close", traced_path(arg_x(c, 0)));
    close_file(arg_x(c, 0));
}
static void ov_entry_type(Cpu *c) {
    const char *path = arg_p(c, 1);
    uint32_t r = entry_type(arg_p(c, 0), path);
    if (rt_trace & RT_TRACE_FS) rt_log("fs entry type %#x: %s", r, path ? path : "(null)");
    ret_x(c, r);
}
static void ov_read_file(Cpu *c) {
    if (rt_trace & RT_TRACE_FS) fs_stack(c, "read", traced_path(arg_x(c, 1)));
    ret_x(c, read_file(arg_p(c, 0), arg_x(c, 1), I64(2), arg_p(c, 3), arg_x(c, 4)));
}
static void ov_read_options(Cpu *c) {
    if (rt_trace & RT_TRACE_FS) fs_stack(c, "read", traced_path(arg_x(c, 1)));
    ret_x(c, read_file(arg_p(c, 0), arg_x(c, 1), I64(2), arg_p(c, 3), arg_x(c, 4)));
}
static void ov_read_exact(Cpu *c) { ret_x(c, read_exact(arg_x(c, 0), I64(1), arg_p(c, 2), arg_x(c, 3))); }
static void ov_write_file(Cpu *c) { ret_x(c, write_file(arg_x(c, 0), I64(1), arg_p(c, 2), arg_x(c, 3), arg_p(c, 4))); }
static void ov_set_file_size(Cpu *c) { ret_x(c, set_file_size(arg_x(c, 0), I64(1))); }
static void ov_flush_file(Cpu *c) { ret_x(c, flush_file(arg_x(c, 0))); }
static void ov_create_file(Cpu *c) { ret_x(c, create_file(arg_p(c, 0), I64(1))); }
static void ov_delete_file(Cpu *c) { ret_x(c, delete_file(arg_p(c, 0))); }
static void ov_rename_file(Cpu *c) { ret_x(c, rename_file(arg_p(c, 0), arg_p(c, 1))); }
static void ov_create_directory(Cpu *c) { ret_x(c, create_directory(arg_p(c, 0))); }
static void ov_delete_recursive(Cpu *c) { ret_x(c, delete_recursive(arg_p(c, 0))); }
static void ov_open_directory(Cpu *c) { ret_x(c, open_directory(arg_p(c, 0), arg_p(c, 1), I32(2))); }
static void ov_read_directory(Cpu *c) { ret_x(c, read_directory(arg_p(c, 0), arg_p(c, 1), arg_x(c, 2), I64(3))); }
static void ov_close_directory(Cpu *c) { close_directory(arg_x(c, 0)); }
static void ov_mount_save(Cpu *c) { ret_x(c, mount_save(arg_p(c, 0), arg_p(c, 1))); }
static void ov_mount_save_ro(Cpu *c) { ret_x(c, mount_save_ro(arg_p(c, 0), arg_x(c, 1), arg_p(c, 2))); }
static void ov_save_exists(Cpu *c) { ret_x(c, save_exists(arg_x(c, 0), arg_p(c, 1))); }
static void ov_mount_temp(Cpu *c) { ret_x(c, mount_temp(arg_p(c, 0))); }
static void ov_unmount(Cpu *c) { unmount(arg_p(c, 0)); }
static void ov_commit(Cpu *c) { ret_x(c, commit(arg_p(c, 0))); }
static void ov_create_cache(Cpu *c) { ret_x(c, create_cache(I32(0), I64(1), I64(2))); }
static void ov_delete_cache(Cpu *c) { ret_x(c, delete_cache(I32(0))); }
static void ov_mount_cache(Cpu *c) { ret_x(c, mount_cache(arg_p(c, 0), I32(1))); }
static void ov_open_cache_list(Cpu *c) { ret_x(c, open_cache_list(arg_p(c, 0))); }
static void ov_read_cache_list(Cpu *c) { ret_x(c, read_cache_list(arg_p(c, 0), arg_p(c, 1), arg_x(c, 2), I32(3))); }
static void ov_close_cache_list(Cpu *c) { close_cache_list(arg_x(c, 0)); }
// The fs priority only orders I/O inside the system fs process; host I/O is unordered.
static void ov_set_priority(Cpu *c) { (void)c; }
// fastball
// nn::Result MountRom(const char *name, void *cache, size_t cache_size): the cache is not used
static void ov_mount_rom(Cpu *c) { ret_x(c, mount_rom(arg_p(c, 0))); }
// nn::Result QueryMountRomCacheSize(size_t *out, int): no cache needed, the host reads files directly
static void ov_query_cache(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    if (out) *out = 0;
    ret_x(c, out ? 0 : FS_R_INVALID_ARGUMENT);
}
// void EnableGlobalFileDataCache(void *buffer, size_t size): the host's file cache does this
static void ov_global_cache(Cpu *c) { (void)c; }
// void InitializeForApplication(): SDK-internal; the storage roots come up on first use
static void ov_initialize(Cpu *c) { (void)c; }
static void ov_get_save_size(Cpu *c) { ret_x(c, get_save_size(arg_p(c, 0), arg_p(c, 1), arg_p(c, 2))); }
static void ov_extend_save(Cpu *c) { ret_x(c, extend_save(arg_p(c, 0), I64(1), I64(2))); }
static void ov_free_space(Cpu *c) { ret_x(c, free_space(arg_p(c, 0), arg_p(c, 1))); }
static void ov_mount_cache0(Cpu *c) { ret_x(c, mount_cache(arg_p(c, 0), 0)); }
static void ov_delete_directory(Cpu *c) { ret_x(c, delete_directory(arg_p(c, 0))); }
static void ov_entry_count(Cpu *c) { ret_x(c, directory_entry_count(arg_p(c, 0), arg_x(c, 1))); }
#undef I32
#undef I64

#define S_OPEN_FILE "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci"
#define S_FILE_SIZE "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE"
#define S_CLOSE_FILE "_ZN2nn2fs9CloseFileENS0_10FileHandleE"
#define S_ENTRY_TYPE "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc"
#define S_READ_FILE "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm"
#define S_READ_OPTIONS "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE"
#define S_READ_EXACT "_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm"
#define S_WRITE_FILE "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE"
#define S_SET_FILE_SIZE "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl"
#define S_FLUSH_FILE "_ZN2nn2fs9FlushFileENS0_10FileHandleE"
#define S_CREATE_FILE "_ZN2nn2fs10CreateFileEPKcl"
#define S_DELETE_FILE "_ZN2nn2fs10DeleteFileEPKc"
#define S_RENAME_FILE "_ZN2nn2fs10RenameFileEPKcS2_"
#define S_CREATE_DIRECTORY "_ZN2nn2fs15CreateDirectoryEPKc"
#define S_DELETE_RECURSIVE "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc"
#define S_OPEN_DIRECTORY "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci"
#define S_READ_DIRECTORY "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl"
#define S_CLOSE_DIRECTORY "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE"
#define S_MOUNT_SAVE "_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE"
#define S_MOUNT_SAVE_RO "_ZN2nn2fs21MountSaveDataReadOnlyEPKcNS_13ApplicationIdERKNS_7account3UidE"
#define S_SAVE_EXISTS "_ZN2nn2fs18IsSaveDataExistingENS_13ApplicationIdERKNS_7account3UidE"
#define S_MOUNT_TEMP "_ZN2nn2fs21MountTemporaryStorageEPKc"
#define S_UNMOUNT "_ZN2nn2fs7UnmountEPKc"
#define S_COMMIT "_ZN2nn2fs6CommitEPKc"
#define S_CREATE_CACHE "_ZN2nn2fs18CreateCacheStorageEill"
#define S_DELETE_CACHE "_ZN2nn2fs18DeleteCacheStorageEi"
#define S_MOUNT_CACHE "_ZN2nn2fs17MountCacheStorageEPKci"
#define S_OPEN_CACHE_LIST "_ZN2nn2fs20OpenCacheStorageListEPNS0_22CacheStorageListHandleE"
#define S_READ_CACHE_LIST "_ZN2nn2fs20ReadCacheStorageListEPiPNS0_16CacheStorageInfoENS0_22CacheStorageListHandleEi"
#define S_CLOSE_CACHE_LIST "_ZN2nn2fs21CloseCacheStorageListENS0_22CacheStorageListHandleE"
#define S_SET_PRIORITY "_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE"
// fastball
#define S_MOUNT_ROM "_ZN2nn2fs8MountRomEPKcPvm"
#define S_QUERY_CACHE "_ZN2nn2fs22QueryMountRomCacheSizeEPmi"
#define S_INITIALIZE "_ZN2nn2fs24InitializeForApplicationEv"
#define S_GLOBAL_CACHE "_ZN2nn2fs25EnableGlobalFileDataCacheEPvm"
#define S_GET_SAVE_SIZE "_ZN2nn2fs15GetSaveDataSizeEPlS1_RKNS_7account3UidE"
#define S_EXTEND_SAVE "_ZN2nn2fs14ExtendSaveDataERKNS_7account3UidEll"
#define S_FREE_SPACE "_ZN2nn2fs16GetFreeSpaceSizeEPlPKc"
#define S_MOUNT_CACHE0 "_ZN2nn2fs17MountCacheStorageEPKc"
#define S_DELETE_DIRECTORY "_ZN2nn2fs15DeleteDirectoryEPKc"
#define S_ENTRY_COUNT "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE"

HOST_OVERRIDE(S_OPEN_FILE, ov_open_file);
HOST_OVERRIDE(S_FILE_SIZE, ov_file_size);
HOST_OVERRIDE(S_CLOSE_FILE, ov_close_file);
HOST_OVERRIDE(S_ENTRY_TYPE, ov_entry_type);
HOST_OVERRIDE(S_READ_FILE, ov_read_file);
HOST_OVERRIDE(S_READ_OPTIONS, ov_read_options);
HOST_OVERRIDE(S_READ_EXACT, ov_read_exact);
HOST_OVERRIDE(S_WRITE_FILE, ov_write_file);
HOST_OVERRIDE(S_SET_FILE_SIZE, ov_set_file_size);
HOST_OVERRIDE(S_FLUSH_FILE, ov_flush_file);
HOST_OVERRIDE(S_CREATE_FILE, ov_create_file);
HOST_OVERRIDE(S_DELETE_FILE, ov_delete_file);
HOST_OVERRIDE(S_RENAME_FILE, ov_rename_file);
HOST_OVERRIDE(S_CREATE_DIRECTORY, ov_create_directory);
HOST_OVERRIDE(S_DELETE_RECURSIVE, ov_delete_recursive);
HOST_OVERRIDE(S_OPEN_DIRECTORY, ov_open_directory);
HOST_OVERRIDE(S_READ_DIRECTORY, ov_read_directory);
HOST_OVERRIDE(S_CLOSE_DIRECTORY, ov_close_directory);
HOST_OVERRIDE(S_MOUNT_SAVE, ov_mount_save);
HOST_OVERRIDE(S_MOUNT_SAVE_RO, ov_mount_save_ro);
HOST_OVERRIDE(S_SAVE_EXISTS, ov_save_exists);
HOST_OVERRIDE(S_MOUNT_TEMP, ov_mount_temp);
HOST_OVERRIDE(S_UNMOUNT, ov_unmount);
HOST_OVERRIDE(S_COMMIT, ov_commit);
HOST_OVERRIDE(S_CREATE_CACHE, ov_create_cache);
HOST_OVERRIDE(S_DELETE_CACHE, ov_delete_cache);
HOST_OVERRIDE(S_MOUNT_CACHE, ov_mount_cache);
HOST_OVERRIDE(S_OPEN_CACHE_LIST, ov_open_cache_list);
HOST_OVERRIDE(S_READ_CACHE_LIST, ov_read_cache_list);
HOST_OVERRIDE(S_CLOSE_CACHE_LIST, ov_close_cache_list);
HOST_OVERRIDE(S_SET_PRIORITY, ov_set_priority);
HOST_OVERRIDE(S_MOUNT_ROM, ov_mount_rom);
HOST_OVERRIDE(S_QUERY_CACHE, ov_query_cache);
HOST_OVERRIDE(S_INITIALIZE, ov_initialize);
HOST_OVERRIDE(S_GLOBAL_CACHE, ov_global_cache);
HOST_OVERRIDE(S_GET_SAVE_SIZE, ov_get_save_size);
HOST_OVERRIDE(S_EXTEND_SAVE, ov_extend_save);
HOST_OVERRIDE(S_FREE_SPACE, ov_free_space);
HOST_OVERRIDE(S_MOUNT_CACHE0, ov_mount_cache0);
HOST_OVERRIDE(S_DELETE_DIRECTORY, ov_delete_directory);
HOST_OVERRIDE(S_ENTRY_COUNT, ov_entry_count);

// ---- --probe-fs
// Every check calls the overrides the way guest code does: by synthetic address through rt_call, with AAPCS64
// arguments in a Cpu, on this (probe) thread. Prints counts only, never RomFS paths.
static const char *const probe_symbols[] = {
    S_OPEN_FILE, S_FILE_SIZE, S_CLOSE_FILE, S_ENTRY_TYPE, S_READ_FILE, S_READ_OPTIONS, S_READ_EXACT, S_WRITE_FILE,
    S_SET_FILE_SIZE, S_FLUSH_FILE, S_CREATE_FILE, S_DELETE_FILE, S_RENAME_FILE, S_CREATE_DIRECTORY, S_DELETE_RECURSIVE,
    S_OPEN_DIRECTORY, S_READ_DIRECTORY, S_CLOSE_DIRECTORY, S_MOUNT_SAVE, S_MOUNT_SAVE_RO, S_SAVE_EXISTS, S_MOUNT_TEMP,
    S_UNMOUNT, S_COMMIT, S_CREATE_CACHE, S_DELETE_CACHE, S_MOUNT_CACHE, S_OPEN_CACHE_LIST, S_READ_CACHE_LIST,
    S_CLOSE_CACHE_LIST, S_SET_PRIORITY, S_MOUNT_ROM, S_QUERY_CACHE, S_INITIALIZE, S_GLOBAL_CACHE, S_GET_SAVE_SIZE,
    S_EXTEND_SAVE, S_FREE_SPACE, S_MOUNT_CACHE0, S_DELETE_DIRECTORY, S_ENTRY_COUNT};
#define NPROBE ((int)(sizeof probe_symbols / sizeof *probe_symbols))
#define SAMPLES 64
#define HEAD_BYTES (8u << 20)      // a sampled file is compared whole up to this size, else head and tail
#define TAIL_BYTES (64u << 10)

static uint64_t probe_addr[NPROBE];
static Cpu *pcpu;
static int failures, checks;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static uint64_t U(const void *p) { return (uint64_t)(uintptr_t)p; }

// The override registered for sym, called through its synthetic address with up to 6 arguments. Returns x0.
static uint64_t fs_call(const char *sym, const uint64_t a[6]) {
    for (int i = 0; i < NPROBE; i++)
        if (!strcmp(probe_symbols[i], sym)) {
            for (int r = 0; r < 19; r++) pcpu->x[r] = 0xdead0000 + r;   // caller-saved garbage
            return rt_call(pcpu, probe_addr[i], 6, a[0], a[1], a[2], a[3], a[4], a[5]);
        }
    rt_fatal("probe-fs: %s is not a probed symbol", sym);
}
#define CALL(sym, ...) fs_call(sym, (const uint64_t[6]){__VA_ARGS__})
#define RES(sym, ...) ((uint32_t)CALL(sym, __VA_ARGS__))

static int64_t independent_read(PlatFile *f, uint8_t *buf, uint64_t n, uint64_t off) {
    uint64_t done = 0;
    while (done < n) {
        int64_t r = host_read_at(f, buf + done, (uint32_t)(n - done), off + done);
        if (r <= 0) return -1;
        done += (uint64_t)r;
    }
    return (int64_t)done;
}

// The RomFS mount against romfs_list.tsv next to the extracted directory (size, offset, path per line): every listed
// file's entry type through the overrides; about 64 sampled files opened, sized and read (head up to 8 MiB) through
// the three ReadFile overloads and compared with an independent read; a root listing; write opens refused. Prints
// counts only.
static uint64_t probe_romfs(void) {
    char list[PATH_CAP];
    snprintf(list, sizeof list, "%s/../romfs_list.tsv", rt_config()->romfs);
    uint64_t got = 0;
    char *text = (char *)plat_file_read(list, &got);
    CHECK(text != NULL, "romfs_list.tsv not found next to the RomFS");
    if (!text) return 0;
    char *t = realloc(text, got + 1);
    need(t != NULL, "romfs list");
    text = t;
    text[got] = 0;
    uint64_t cache = ~0ull;
    CHECK(RES(S_QUERY_CACHE, U(&cache), 0) == 0 && cache == 0, "QueryMountRomCacheSize");
    CHECK(RES(S_MOUNT_ROM, U("rom"), 0, 0) == 0, "MountRom");
    uint64_t lines = 0;
    for (const char *q = text; *q; q++) lines += *q == '\n';
    uint64_t files = 0, typed = 0, sized = 0, sampled = 0, step = lines / SAMPLES ? lines / SAMPLES : 1;
    uint8_t *a = malloc(HEAD_BYTES), *b = malloc(HEAD_BYTES);
    need(a && b, "probe buffers");
    for (char *line = text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[len - 1] = 0;   // the list may have CRLF line ends
        char *t1 = strchr(line, '\t'), *t2 = t1 ? strchr(t1 + 1, '\t') : NULL;
        if (!t2) continue;
        uint64_t size = strtoull(line, NULL, 10);
        char path[PATH_CAP];
        snprintf(path, sizeof path, "rom:%s", t2 + 1);
        files++;
        int type = -1;
        typed += RES(S_ENTRY_TYPE, U(&type), U(path)) == 0 && type == FS_ENTRY_FILE;
        if (files % step) continue;
        uint64_t h = 0, n = size < HEAD_BYTES ? size : HEAD_BYTES, rd = ~0ull;
        int64_t fsize = -1;
        if (RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ)) continue;
        int ok = RES(S_FILE_SIZE, U(&fsize), h) == 0 && (uint64_t)fsize == size;
        sized += ok;
        int32_t option = 0;
        int v = (int)(files % 3);
        uint32_t r = v == 0 ? RES(S_READ_FILE, U(&rd), h, 0, U(a), n)
                   : v == 1 ? RES(S_READ_OPTIONS, U(&rd), h, 0, U(a), n, U(&option))
                            : (rd = n, RES(S_READ_EXACT, h, 0, U(a), n));
        char host[PATH_CAP];
        snprintf(host, sizeof host, "%s%s", rt_config()->romfs, t2 + 1);
        PlatFile *f = plat_file_open(host);
        sampled += ok && !r && rd == n && f && independent_read(f, b, n, 0) == (int64_t)n && !memcmp(a, b, n);
        if (f) plat_file_close(f);
        CALL(S_CLOSE_FILE, h);
    }
    free(a);
    free(b);
    free(text);
    CHECK(files && typed == files, "%llu of %llu listed files are files on the RomFS mount", (unsigned long long)typed,
          (unsigned long long)files);
    CHECK(sampled >= SAMPLES && sized == sampled, "sampled files: %llu sized, %llu read equal", (unsigned long long)sized,
          (unsigned long long)sampled);
    uint64_t d = 0, h = 0;
    int64_t count = -1, entries = -1;
    static DirectoryEntry de[64];
    CHECK(RES(S_OPEN_DIRECTORY, U(&d), U("rom:/"), 3) == 0 && RES(S_ENTRY_COUNT, U(&count), d) == 0 && count > 0 &&
              RES(S_READ_DIRECTORY, U(&entries), U(de), d, 64) == 0 && entries == (count < 64 ? count : 64),
          "RomFS root listing");
    if (d) CALL(S_CLOSE_DIRECTORY, d);
    CHECK(RES(S_OPEN_FILE, U(&h), U("rom:/CHANGELIST.TXT"), FS_OPEN_READ | FS_OPEN_WRITE) == FS_R_UNSUPPORTED_OPEN &&
              RES(S_CREATE_FILE, U("rom:/x"), 0) == FS_R_UNSUPPORTED_OPEN,
          "RomFS write refused");
    CALL(S_UNMOUNT, U("rom"));
    printf("romfs: %llu of %llu listed files typed; %llu sampled files read equal the extracted files (head up to %u "
           "MiB); root lists %lld entries\n", (unsigned long long)typed, (unsigned long long)files,
           (unsigned long long)sampled, HEAD_BYTES >> 20, (long long)count);
    return files;
}

// ---- save transactions: restarts, uncommitted and committed changes, a crash at every step of a Commit
// The process dies here: the save mount goes without Unmount, and the next start runs save_recover and mounts again.
static void probe_restart(const uint64_t *uid) {
    plat_mutex_lock(&lock);
    for (int i = 0; i < NMOUNTS; i++)
        if (mounts[i].kind == M_SAVE) mounts[i].kind = M_FREE;
    plat_mutex_unlock(&lock);
    save_mounts = tx_crash_at = tx_dead = 0;
    plat_xchg32(&save_dirty, 0);
    save_recover();
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0, "MountSaveData after a restart");
}

// Commit through the override, then wait for its background thread, as the checks read the disk next.
static uint32_t tx_commit(void) {
    uint32_t r = RES(S_COMMIT, U("save"));
    fs_save_flush();
    return r;
}

#define TX_SIZE 4096
static uint8_t tx_buf[TX_SIZE];

// Fill a save file, created when absent, with byte g through the overrides.
static int tx_write(const char *path, int g) {
    uint64_t h = 0;
    int32_t no_flush = 0;
    memset(tx_buf, g, sizeof tx_buf);
    uint32_t r = RES(S_CREATE_FILE, U(path), TX_SIZE);
    if ((r && r != FS_R_ALREADY_EXISTS) || RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_WRITE)) return 0;
    int ok = RES(S_WRITE_FILE, h, 0, U(tx_buf), TX_SIZE, U(&no_flush)) == 0;
    CALL(S_CLOSE_FILE, h);
    return ok;
}

static int tx_byte(int ok) {
    for (int i = 1; ok && i < TX_SIZE; i++) ok = tx_buf[i] == tx_buf[0];
    return ok ? tx_buf[0] : -2;
}

// The byte a save file is filled with, read through the overrides; -1 absent, -2 short or mixed.
static int tx_read(const char *path) {
    uint64_t h = 0, got = 0;
    if (RES(S_OPEN_FILE, U(&h), U(path), FS_OPEN_READ)) return -1;
    int ok = RES(S_READ_FILE, U(&got), h, 0, U(tx_buf), TX_SIZE) == 0 && got == TX_SIZE;
    CALL(S_CLOSE_FILE, h);
    return tx_byte(ok);
}

// The same, read from the committed save on disk.
static int tx_read_committed(const char *name) {
    char *p = join(save_root, name);
    PlatFile *f = plat_file_open(p);
    free(p);
    if (!f) return -1;
    int ok = independent_read(f, tx_buf, TX_SIZE, 0) == TX_SIZE;
    plat_file_close(f);
    return tx_byte(ok);
}

// Hard links of a file of the committed save on disk; 0 when absent.
static int tx_links_committed(const char *name) {
    char *p = join(save_root, name);
    int links = plat_fs_links(p);
    free(p);
    return links;
}

static int count_dirs(const char *dir) {
    Listing l;
    list_dir(dir, &l);
    int n = 0;
    for (int i = 0; i < l.n; i++) n += l.e[i].kind == PLAT_FS_DIR && l.e[i].name[0] != '.';
    listing_free(l.e, l.n);
    return n;
}

static void probe_save_tx(const uint64_t *uid) {
    const char *a = "save:/tx_a.sav", *b = "save:/tx_b.sav";
    uint64_t h = 0;
    CHECK(tx_write(a, 1) && tx_write(b, 1) && tx_commit() == 0 && tx_read_committed("tx_a.sav") == 1,
          "Commit of generation 1");

    // uncommitted changes: the game reads them; the committed save and the next start do not see them
    CHECK(tx_write(a, 2) && tx_write("save:/tx_c.sav", 2), "uncommitted writes");
    CHECK(tx_read(a) == 2 && tx_read("save:/tx_c.sav") == 2, "reads do not see the game's uncommitted writes");
    CHECK(tx_read_committed("tx_a.sav") == 1 && tx_read_committed("tx_c.sav") == -1,
          "an uncommitted write reached the committed save");
    probe_restart(uid);
    CHECK(tx_read(a) == 1 && tx_read("save:/tx_c.sav") == -1, "an uncommitted write survived a restart");

    // committed changes survive a restart; an Unmount without Commit drops changes
    CHECK(tx_write(a, 3) && tx_write(b, 3) && tx_commit() == 0, "Commit of generation 3");
    probe_restart(uid);
    CHECK(tx_read(a) == 3 && tx_read(b) == 3, "a commit lost in a restart");
    CHECK(tx_write(a, 4), "write before Unmount");
    CALL(S_UNMOUNT, U("save"));
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0 && tx_read(a) == 3, "Unmount without Commit kept the change");

    // a crash at step k of a Commit, k = 1, 2, ... until a Commit completes: the next start sees all of the old
    // generation or all of the new one
    int committed = 3, gen = 4, steps = 0, olds = 0, news = 0, mixed = 0;
    for (int k = 1; k < 64; k++) {
        int next = ++gen, ok = tx_write(a, next) && tx_write(b, next);
        tx_crash_at = k;
        uint32_t r = tx_commit();
        int died = tx_dead;
        tx_crash_at = 0;
        if (!died) {
            steps = k - 1;
            CHECK(ok && r == 0 && tx_read_committed("tx_a.sav") == next && tx_read_committed("tx_b.sav") == next,
                  "Commit after the crash points");
            committed = next;
            break;
        }
        probe_restart(uid);
        int va = tx_read(a), vb = tx_read(b);
        if (va == committed && vb == committed) olds++;
        else if (va == next && vb == next) news++, committed = next;
        else mixed++;
        CHECK(ok && (va == vb) && (va == committed), "crash at commit step %d: tx_a %d, tx_b %d (old %d, new %d)", k,
              va, vb, committed, next);
        CHECK(!exists(new_root) && !exists(old_root) && !*ready_root, "crash at commit step %d: staging left after "
              "recovery", k);
    }
    CHECK(steps >= 4 && olds && news && !mixed, "crash points: %d (%d old, %d new, %d mixed)", steps, olds, news,
          mixed);
    int backups = count_dirs(backup_root);
    CHECK(backups == SAVE_BACKUPS, "save-backup holds %d saves", backups);

    // a Commit writes the changed file and links the unchanged one to the committed save it replaces
    int kept = committed;
    CHECK(tx_write(a, ++gen) && tx_commit() == 0 && tx_read_committed("tx_a.sav") == gen &&
          tx_read_committed("tx_b.sav") == kept && tx_links_committed("tx_b.sav") >= 2 &&
          tx_links_committed("tx_a.sav") == 1, "Commit of one changed file: tx_a %d links, tx_b %d links",
          tx_links_committed("tx_a.sav"), tx_links_committed("tx_b.sav"));
    committed = gen;

    // a Commit that changed no byte (the game rewrites its files at start) makes no new save and no backup
    static char backups_before[64][48], backups_after[64][48];
    int nbefore = backup_names(backups_before);
    CHECK(tx_write(a, committed) && tx_commit() == 0 && tx_read_committed("tx_a.sav") == committed &&
          backup_names(backups_after) == nbefore && !memcmp(backups_before, backups_after, sizeof backups_before),
          "a Commit of unchanged bytes made a backup");

    // the background commit: a write right after Commit waits for it and stays out of it; an Unmount right after
    // Commit waits for it and keeps it
    int queued = ++gen;
    CHECK(tx_write(a, queued) && RES(S_COMMIT, U("save")) == 0 && tx_write(a, ++gen) && tx_read(a) == gen &&
          tx_read_committed("tx_a.sav") == queued, "a write right after Commit: committed tx_a %d, expected %d",
          tx_read_committed("tx_a.sav"), queued);
    CHECK(RES(S_COMMIT, U("save")) == 0, "Commit before Unmount");
    CALL(S_UNMOUNT, U("save"));
    CHECK(tx_read_committed("tx_a.sav") == gen && RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0 && tx_read(a) == gen,
          "an Unmount right after Commit lost it");
    committed = gen;

    // a file of the committed save held open by another program (a virus scanner, Explorer's preview, a sync tool):
    // Windows refuses to rename a directory with an open file under it, so the committed save cannot be swapped out.
    // The game and a restart still see every Commit, and the first start after the file is closed swaps them in.
    char *held_path = join(save_root, "tx_b.sav");
    PlatFile *held = plat_fs_open(held_path, 0, NULL);
    free(held_path);
    int pending = ++gen;
    CHECK(held && tx_write(a, pending) && tx_commit() == 0, "Commit with a committed file held open");
    probe_restart(uid);
    CHECK(tx_read(a) == pending, "a Commit made while a committed file was held open, after a restart: tx_a %d, "
          "expected %d", tx_read(a), pending);
    pending = ++gen;
    CHECK(tx_write(a, pending) && tx_commit() == 0 && tx_read(a) == pending, "a second Commit while held open");
    if (held) plat_file_close(held);
    probe_restart(uid);
    CHECK(tx_read(a) == pending && tx_read_committed("tx_a.sav") == pending && tx_read_committed("tx_b.sav") == kept,
          "the held-open Commits after the file was closed and a restart: tx_a %d, on disk %d, expected %d",
          tx_read(a), tx_read_committed("tx_a.sav"), pending);
    committed = pending;

    // the read-only mount shows the committed save and refuses changes
    CHECK(tx_write(a, 99) && RES(S_MOUNT_SAVE_RO, U("ro"), fs_title.program_id, U(uid)) == 0 &&
          tx_read("ro:/tx_a.sav") == committed && RES(S_OPEN_FILE, U(&h), U("ro:/tx_a.sav"), FS_OPEN_WRITE) ==
          FS_R_UNSUPPORTED_OPEN && RES(S_CREATE_FILE, U("ro:/x"), 0) == FS_R_UNSUPPORTED_OPEN &&
          RES(S_RENAME_FILE, U("save:/tx_a.sav"), U("ro:/y")) == FS_R_INVALID_PATH, "MountSaveDataReadOnly");
    CALL(S_UNMOUNT, U("ro"));
    CHECK(RES(S_DELETE_FILE, U(a)) == 0 && RES(S_DELETE_FILE, U(b)) == 0 && tx_commit() == 0 &&
          tx_read_committed("tx_a.sav") == -1, "Commit of deletions");
    // a run that left read-only folders and files in the working copy (attributes another emulator's save carried):
    // the next start deletes them and names the failing path if it cannot
    char *ro_dir = join(work_root, "ro_dir"), *ro_file = join(ro_dir, "ro.sav");
    CHECK(!plat_fs_mkdir(ro_dir) && plat_file_write(ro_file, "x", 1) && plat_file_set_readonly(ro_file, 1) &&
          plat_file_set_readonly(ro_dir, 1), "read-only leftovers in the working copy");
    probe_restart(uid);
    CHECK(!exists(ro_dir), "the next start deleted a working copy with read-only folders");
    free(ro_dir);
    free(ro_file);
    printf("save tx: uncommitted writes visible to the game and dropped by a restart and by Unmount, commits kept; a "
           "crash at each of %d commit steps recovers to the old save %d times, the new one %d times, mixed %d "
           "times; %d backups\n", steps, olds, news, mixed, backups);
}

static void probe_storage(void) {
    char tmp[PLAT_PATH_MAX * 3], dir[PLAT_PATH_MAX * 3 + 64];
    need(plat_temp_dir(tmp, sizeof tmp), "temporary directory");
    snprintf(dir, sizeof dir, "%s/totk-probe-fs-%u", tmp, plat_process_id());
    remove_tree(dir);
    fs_host_set_profile(dir);
    uint64_t uid[2], other[2], h = 0, h2 = 0, d = 0, l = 0;
    account_uid(uid);
    other[0] = uid[0] ^ 1, other[1] = uid[1];
    static uint8_t data[65536], back[65536];
    for (unsigned i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7 + (i >> 8));
    int32_t no_flush = 0, flush = 1;
    int64_t size = -1, count = -1;
    uint64_t got = 0;
    int type = -1;

    CHECK(RES(S_MOUNT_SAVE, U("save"), U(other)) == FS_R_TARGET_NOT_FOUND, "MountSaveData for another user");
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0, "MountSaveData");
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == FS_R_ALREADY_EXISTS, "MountSaveData twice");
    CHECK((CALL(S_SAVE_EXISTS, fs_title.program_id, U(uid)) & 0xff) == 1 &&
          (CALL(S_SAVE_EXISTS, fs_title.program_id + 0x1000, U(uid)) & 0xff) == 0, "IsSaveDataExisting");
    CHECK(RES(S_MOUNT_SAVE_RO, U("other"), fs_title.program_id + 0x1000, U(uid)) == FS_R_TARGET_NOT_FOUND,
          "MountSaveDataReadOnly of another title");
    CHECK(!fs_save_busy(), "idle at start");

    // create, write, commit
    CHECK(RES(S_CREATE_FILE, U("save:/probe.sav"), 0) == 0 && fs_save_busy(), "CreateFile");
    CHECK(RES(S_CREATE_FILE, U("save:/probe.sav"), 0) == FS_R_ALREADY_EXISTS, "CreateFile twice");
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_WRITE) == 0, "OpenFile for writing");
    CHECK(RES(S_WRITE_FILE, h, 0, U(data), 16, U(&no_flush)) == FS_R_APPEND_REQUIRED, "a growing write without append");
    CALL(S_CLOSE_FILE, h);
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_WRITE | FS_OPEN_APPEND) == 0, "OpenFile for append");
    CHECK(RES(S_WRITE_FILE, h, 0, U(data), sizeof data / 2, U(&no_flush)) == 0 &&
          RES(S_WRITE_FILE, h, sizeof data / 2, U(data + sizeof data / 2), sizeof data / 2, U(&flush)) == 0,
          "WriteFile");
    CHECK(RES(S_FILE_SIZE, U(&size), h) == 0 && size == sizeof data, "GetFileSize after writing");
    CHECK(RES(S_READ_FILE, U(&got), h, 0, U(back), 16) == FS_R_OPEN_MODE, "ReadFile on a write-only handle");
    CHECK(RES(S_FLUSH_FILE, h) == 0, "FlushFile");
    CHECK(fs_save_busy(), "a save file open for writing does not count as a save in progress");
    CALL(S_CLOSE_FILE, h);
    CHECK(fs_save_busy(), "closed but uncommitted save data does not count as a save in progress");
    CHECK(tx_commit() == 0 && !fs_save_busy(), "Commit");
    CHECK(RES(S_COMMIT, U("nomount")) == FS_R_PATH_NOT_FOUND, "Commit of an unknown mount");
    {   // fastball: GetFreeSpaceSize takes a bare mount name as MLB passes it ("savedata"), same as "name:/"
        int64_t bare = -1, rooted = -2;
        CHECK(RES(S_FREE_SPACE, U(&bare), U("save")) == 0 && RES(S_FREE_SPACE, U(&rooted), U("save:/")) == 0 &&
                  bare == rooted && bare > 0,
              "GetFreeSpaceSize of a bare mount name");
    }

    // paths
    CHECK(RES(S_CREATE_FILE, U("save:/../x"), 0) == FS_R_INVALID_PATH, "save:/../x");
    CHECK(RES(S_CREATE_FILE, U("save:/a\\b"), 0) == FS_R_INVALID_PATH, "save:/a\\b");
    CHECK(RES(S_CREATE_FILE, U("save:/a:b"), 0) == FS_R_INVALID_PATH, "save:/a:b");
    CHECK(RES(S_CREATE_FILE, U("save:/a."), 0) == FS_R_INVALID_PATH, "save:/a.");
    CHECK(RES(S_CREATE_FILE, U("nomount:/x"), 0) == FS_R_PATH_NOT_FOUND, "a path on no mount");

    // directories
    CHECK(RES(S_CREATE_DIRECTORY, U("save:/dir")) == 0 && RES(S_CREATE_FILE, U("save:/dir/inner.bin"), 8) == 0,
          "CreateDirectory");
    CHECK(RES(S_ENTRY_TYPE, U(&type), U("save:/dir")) == 0 && type == FS_ENTRY_DIRECTORY, "GetEntryType (directory)");
    static DirectoryEntry ents[8];
    CHECK(RES(S_OPEN_DIRECTORY, U(&d), U("save:/"), 3) == 0, "OpenDirectory");
    CHECK(RES(S_READ_DIRECTORY, U(&count), U(ents), d, 8) == 0 && count == 2, "ReadDirectory count %lld",
          (long long)count);
    int seen = 0;
    for (int i = 0; i < count && i < 8; i++) {
        if (!strcmp(ents[i].name, "dir") && ents[i].type == FS_ENTRY_DIRECTORY) seen |= 1;
        if (!strcmp(ents[i].name, "probe.sav") && ents[i].type == FS_ENTRY_FILE && ents[i].size == sizeof data)
            seen |= 2;
    }
    CHECK(seen == 3, "ReadDirectory entries");
    CHECK(RES(S_READ_DIRECTORY, U(&count), U(ents), d, 8) == 0 && count == 0, "ReadDirectory at the end");
    CALL(S_CLOSE_DIRECTORY, d);
    CHECK(RES(S_OPEN_DIRECTORY, U(&d), U("save:/"), 2) == 0 && RES(S_READ_DIRECTORY, U(&count), U(ents), d, 8) == 0 &&
          count == 1 && ents[0].type == FS_ENTRY_FILE, "ReadDirectory, files only");
    CALL(S_CLOSE_DIRECTORY, d);

    CHECK(tx_commit() == 0, "Commit of the directories");

    // reopen after unmount: the bytes come back through the overrides and are on disk
    CALL(S_UNMOUNT, U("save"));
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_READ) == FS_R_PATH_NOT_FOUND, "open after Unmount");
    CHECK(RES(S_MOUNT_SAVE, U("save"), U(uid)) == 0, "MountSaveData again");
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_READ) == 0, "reopen");
    memset(back, 0, sizeof back);
    CHECK(RES(S_FILE_SIZE, U(&size), h) == 0 && size == sizeof data &&
          RES(S_READ_EXACT, h, 0, U(back), sizeof back) == 0 && !memcmp(back, data, sizeof data), "reopened bytes");
    CHECK(RES(S_READ_FILE, U(&got), h, sizeof data - 10, U(back), 100) == 0 && got == 10, "short read at the end");
    CHECK(RES(S_WRITE_FILE, h, 0, U(data), 1, U(&no_flush)) == FS_R_OPEN_MODE, "WriteFile on a read-only handle");
    CALL(S_CLOSE_FILE, h);
    char host[1200];
    snprintf(host, sizeof host, "%s/save/%016llx%016llx/probe.sav", dir, (unsigned long long)uid[1],
             (unsigned long long)uid[0]);
    PlatFile *f = plat_file_open(host);
    memset(back, 0, sizeof back);
    CHECK(f && independent_read(f, back, sizeof back, 0) == sizeof back && !memcmp(back, data, sizeof data),
          "the save file on disk");
    if (f) plat_file_close(f);

    // resize, rename, delete
    CHECK(RES(S_OPEN_FILE, U(&h), U("save:/probe.sav"), FS_OPEN_READ | FS_OPEN_WRITE) == 0 &&
          RES(S_SET_FILE_SIZE, h, 100) == 0 && RES(S_FILE_SIZE, U(&size), h) == 0 && size == 100, "SetFileSize");
    CHECK(RES(S_OPEN_FILE, U(&h2), U("save:/probe.sav"), FS_OPEN_READ) == 0, "a second handle on one file");
    CALL(S_CLOSE_FILE, h2);
    CALL(S_CLOSE_FILE, h);
    CHECK(RES(S_RENAME_FILE, U("save:/probe.sav"), U("save:/dir/inner.bin")) == FS_R_ALREADY_EXISTS,
          "RenameFile over an existing file");
    CHECK(RES(S_RENAME_FILE, U("save:/probe.sav"), U("save:/renamed.sav")) == 0 &&
          RES(S_ENTRY_TYPE, U(&type), U("save:/renamed.sav")) == 0 && type == FS_ENTRY_FILE &&
          RES(S_ENTRY_TYPE, U(&type), U("save:/probe.sav")) == FS_R_PATH_NOT_FOUND, "RenameFile");
    CHECK(RES(S_DELETE_FILE, U("save:/renamed.sav")) == 0 &&
          RES(S_DELETE_FILE, U("save:/renamed.sav")) == FS_R_PATH_NOT_FOUND, "DeleteFile");
    CHECK(RES(S_DELETE_RECURSIVE, U("save:/dir")) == 0 &&
          RES(S_ENTRY_TYPE, U(&type), U("save:/dir")) == FS_R_PATH_NOT_FOUND, "DeleteDirectoryRecursively");
    CHECK(tx_commit() == 0 && !fs_save_busy(), "Commit after the changes");
    probe_save_tx(uid);

    // temporary and cache storage
    CHECK(RES(S_MOUNT_TEMP, U("temp")) == 0 && RES(S_CREATE_FILE, U("temp:/t.bin"), 4) == 0 && !fs_save_busy(),
          "temporary storage");
    CHECK(RES(S_CREATE_CACHE, 3, 1 << 20, 1 << 20) == 0 && RES(S_CREATE_CACHE, 3, 1 << 20, 1 << 20) ==
          FS_R_ALREADY_EXISTS, "CreateCacheStorage");
    CHECK(RES(S_MOUNT_CACHE, U("cache"), 3) == 0 && RES(S_MOUNT_CACHE, U("cache9"), 9) == FS_R_TARGET_NOT_FOUND,
          "MountCacheStorage");
    CacheStorageInfo infos[4];
    int ncache = -1;
    CHECK(RES(S_OPEN_CACHE_LIST, U(&l)) == 0 && RES(S_READ_CACHE_LIST, U(&ncache), U(infos), l, 4) == 0 &&
          ncache == 1 && infos[0].index == 3, "cache storage list");
    CALL(S_CLOSE_CACHE_LIST, l);
    CALL(S_SET_PRIORITY, 1);
    CALL(S_UNMOUNT, U("cache"));
    CALL(S_UNMOUNT, U("temp"));
    CHECK(RES(S_DELETE_CACHE, 3) == 0, "DeleteCacheStorage");
    CALL(S_UNMOUNT, U("save"));

    if (profile_lock) plat_file_close(profile_lock), profile_lock = NULL;
    int removed = !remove_tree(dir) && !exists(dir);
    CHECK(removed, "the temporary profile was not deleted");
    printf("storage: save create/write/commit/reopen, directories, rename/delete, temporary and cache storage in a "
           "temporary profile: %s; profile deleted: %s\n", failures ? "see FAIL lines" : "ok", removed ? "yes" : "no");
}

int probe_fs(void) {
    for (int i = 0; i < NPROBE; i++) {
        GuestFn fn = hostfn_override(probe_symbols[i]);
        if (!fn) rt_fatal("probe-fs: no HOST_OVERRIDE for %s", probe_symbols[i]);
        probe_addr[i] = hostfn_register(probe_symbols[i], fn);
    }
    printf("overrides: %d nn::fs overrides registered\n", NPROBE);
    pcpu = plat_aligned_alloc(sizeof(Cpu), 64);
    need(pcpu != NULL, "probe Cpu");
    memset(pcpu, 0, sizeof *pcpu);
    uint8_t *stack = mem_kernel_alloc(0x10000, 16);
    pcpu->sp = (uint64_t)(uintptr_t)stack + 0x10000;
    pcpu->excl_addr = ~0ull;
    for (int r = 19; r < 30; r++) pcpu->x[r] = 0x1900 + r;
    CALL(S_INITIALIZE, 0);   // nn::fs::InitializeForApplication: a no-op
    uint64_t files = probe_romfs();
    probe_storage();
    int kept = 1;
    for (int r = 19; r < 30; r++) kept &= pcpu->x[r] == 0x1900u + r;
    CHECK(kept && pcpu->sp == (uint64_t)(uintptr_t)stack + 0x10000, "an override lost a callee-saved register");
    mem_kernel_free(stack, 0x10000);
    plat_aligned_free(pcpu);
    if (failures) {
        printf("probe-fs: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-fs: passed (%llu RomFS files)\n", (unsigned long long)files);
    return 0;
}

// ---- fastball: the game's C stdio FILE calls on the host mounts
//
// MLB's audio stream library (Scream snd_stream: crowd, music, organ, commentary) opens its stream files with the C
// library's fopen ("FS_Rom:/NSW/AUDIO/MUSIC.SKX", "r"), not nn::fs. fopen and the other FILE functions are the SDK's
// own libc, which reaches files through the SDK's internal nn::fs: the overrides above only replace the game's imports,
// so the SDK never learned of these mounts and every fopen returned NULL. The streams were parsed (through nn::fs),
// then failed to open for reading and were retried forever: no streamed sound in the whole game.
//
// The FILE functions the game imports are observed here. fopen for reading on a host mount gets a host FILE (a slot
// in hfiles: its address is the handle the game keeps); every FILE function takes those and passes any other FILE
// (stdout, a write) to the SDK unchanged.
typedef struct {
    uint64_t h;       // fs handle; 0 = free slot
    int64_t pos;
    int eof, err, ungot;   // ungot: the character ungetc pushed back, or -1
} HostFile;
#define NHFILES 128
static HostFile hfiles[NHFILES];
static PlatMutex hf_lock = PLAT_MUTEX_INIT;

static HostFile *hf_of(uint64_t f) {
    uint64_t base = (uint64_t)(uintptr_t)hfiles;
    if (f < base || f >= base + sizeof hfiles || (f - base) % sizeof(HostFile)) return NULL;
    HostFile *h = (HostFile *)(uintptr_t)f;
    return h->h ? h : NULL;
}

// fopen semantics for a read-only open: the HostFile, NULL with *mine = 0 when the SDK should handle it.
static HostFile *hf_open(const char *path, const char *mode, int *mine) {
    *mine = 0;
    if (!path || !mode || mode[0] != 'r' || strchr(mode, '+')) return NULL;   // writes stay the SDK's
    char p[PATH_CAP];
    int kind = M_FREE;
    if (host_path_kind(path, p, sizeof p, &kind) == FS_R_PATH_NOT_FOUND && kind == M_FREE) return NULL;   // no such mount
    *mine = 1;
    uint64_t h = 0;
    if (open_file(&h, path, FS_OPEN_READ)) return NULL;
    plat_mutex_lock(&hf_lock);
    for (int i = 0; i < NHFILES; i++)
        if (!hfiles[i].h) {
            hfiles[i] = (HostFile){h, 0, 0, 0, -1};
            plat_mutex_unlock(&hf_lock);
            return &hfiles[i];
        }
    plat_mutex_unlock(&hf_lock);
    close_file(h);
    rt_log("fs: fopen: all %d host FILE slots are in use (%s)", NHFILES, path);
    return NULL;
}

static uint64_t hf_read(HostFile *f, uint8_t *dst, uint64_t n) {
    uint64_t done = 0;
    if (n && f->ungot >= 0) dst[done++] = (uint8_t)f->ungot, f->ungot = -1, f->pos++;
    if (done < n) {
        uint64_t got = 0;
        if (read_file(&got, f->h, f->pos, dst + done, n - done)) f->err = 1;
        f->pos += (int64_t)got;
        done += got;
    }
    if (done < n && !f->err) f->eof = 1;
    return done;
}

static int hf_getc(HostFile *f) {
    uint8_t b;
    return hf_read(f, &b, 1) == 1 ? b : -1;
}

static int hf_seek(HostFile *f, int64_t off, int whence) {
    int64_t base = 0, size = 0;
    if (whence == 1) base = f->pos;
    else if (whence == 2) {
        if (file_size(&size, f->h)) return -1;
        base = size;
    } else if (whence != 0) return -1;
    if (base + off < 0) return -1;
    f->pos = base + off;
    f->eof = 0;
    f->ungot = -1;
    return 0;
}

static int hf_close(HostFile *f) {
    uint64_t h = f->h;
    plat_mutex_lock(&hf_lock);
    *f = (HostFile){0};
    plat_mutex_unlock(&hf_lock);
    close_file(h);
    return 0;
}

static void obs_fopen(Cpu *c) {
    const char *path = (const char *)(uintptr_t)c->x[0], *mode = (const char *)(uintptr_t)c->x[1];
    int mine;
    HostFile *f = hf_open(path, mode, &mine);
    if (!mine) {
        c->x[0] = rt_original(c);
        return;
    }
    if (rt_trace & RT_TRACE_FS) rt_log("fs fopen \"%s\" \"%s\": %s", path, mode, f ? "host FILE" : "failed");
    c->x[0] = (uint64_t)(uintptr_t)f;
}
static void obs_freopen(Cpu *c) {   // freopen(path, mode, FILE)
    HostFile *old = hf_of(c->x[2]);
    if (!old) {
        c->x[0] = rt_original(c);
        return;
    }
    hf_close(old);
    int mine;
    HostFile *f = hf_open((const char *)(uintptr_t)c->x[0], (const char *)(uintptr_t)c->x[1], &mine);
    c->x[0] = (uint64_t)(uintptr_t)f;
}
// Each function below: the FILE argument's register, then what it does with a host FILE.
#define HF_ARG(reg)                                \
    HostFile *f = hf_of(c->x[reg]);                \
    if (!f) {                                      \
        c->x[0] = rt_original(c);                  \
        return;                                    \
    }
static void obs_fclose(Cpu *c) { HF_ARG(0) c->x[0] = (uint64_t)(int64_t)hf_close(f); }
static void obs_fread(Cpu *c) {   // fread(ptr, size, n, FILE)
    HF_ARG(3)
    uint64_t size = c->x[1], n = c->x[2];
    if (!size || !n) {
        c->x[0] = 0;
        return;
    }
    c->x[0] = hf_read(f, (uint8_t *)(uintptr_t)c->x[0], size * n) / size;
}
static void obs_fseek(Cpu *c) { HF_ARG(0) c->x[0] = (uint64_t)(int64_t)hf_seek(f, (int64_t)c->x[1], (int)c->x[2]); }
static void obs_ftell(Cpu *c) { HF_ARG(0) c->x[0] = (uint64_t)f->pos; }   // ungetc already stepped pos back
static void obs_feof(Cpu *c) { HF_ARG(0) c->x[0] = (uint64_t)(f->eof && f->ungot < 0); }
static void obs_ferror(Cpu *c) { HF_ARG(0) c->x[0] = (uint64_t)f->err; }
static void obs_clearerr(Cpu *c) { HF_ARG(0) f->eof = f->err = 0; }
static void obs_fgetc(Cpu *c) { HF_ARG(0) c->x[0] = (uint64_t)(int64_t)hf_getc(f); }
static void obs_ungetc(Cpu *c) {   // ungetc(ch, FILE)
    HF_ARG(1)
    int ch = (int)c->x[0];
    if (ch < 0 || f->ungot >= 0 || f->pos == 0) {
        c->x[0] = (uint64_t)(int64_t)-1;
        return;
    }
    f->ungot = ch & 0xff;
    f->pos--;
    f->eof = 0;
    c->x[0] = (uint64_t)(ch & 0xff);
}
static void obs_fgets(Cpu *c) {   // fgets(buf, n, FILE)
    HF_ARG(2)
    char *buf = (char *)(uintptr_t)c->x[0];
    int n = (int)c->x[1], k = 0;
    while (k < n - 1) {
        int ch = hf_getc(f);
        if (ch < 0) break;
        buf[k++] = (char)ch;
        if (ch == '\n') break;
    }
    if (n > 0) buf[k] = 0;
    c->x[0] = k ? c->x[0] : 0;
}
static void obs_fflush(Cpu *c) { HF_ARG(0) c->x[0] = 0; }
static void obs_setvbuf(Cpu *c) { HF_ARG(0) c->x[0] = 0; }
// Writes to a read-only host FILE fail as a read-only stream does.
static void obs_fwrite(Cpu *c) { HF_ARG(3) f->err = 1, c->x[0] = 0; }
static void obs_fputc(Cpu *c) { HF_ARG(1) f->err = 1, c->x[0] = (uint64_t)(int64_t)-1; }
static void obs_fputs(Cpu *c) { HF_ARG(1) f->err = 1, c->x[0] = (uint64_t)(int64_t)-1; }
static void obs_fprintf(Cpu *c) { HF_ARG(0) f->err = 1, c->x[0] = (uint64_t)(int64_t)-1; }
HOST_OBSERVER("fopen", obs_fopen);
HOST_OBSERVER("freopen", obs_freopen);
HOST_OBSERVER("fclose", obs_fclose);
HOST_OBSERVER("fread", obs_fread);
HOST_OBSERVER("fseek", obs_fseek);
HOST_OBSERVER("fseeko", obs_fseek);
HOST_OBSERVER("ftell", obs_ftell);
HOST_OBSERVER("ftello", obs_ftell);
HOST_OBSERVER("feof", obs_feof);
HOST_OBSERVER("ferror", obs_ferror);
HOST_OBSERVER("clearerr", obs_clearerr);
HOST_OBSERVER("fgetc", obs_fgetc);
HOST_OBSERVER("getc", obs_fgetc);
HOST_OBSERVER("ungetc", obs_ungetc);
HOST_OBSERVER("fgets", obs_fgets);
HOST_OBSERVER("fflush", obs_fflush);
HOST_OBSERVER("setvbuf", obs_setvbuf);
HOST_OBSERVER("fwrite", obs_fwrite);
HOST_OBSERVER("fputc", obs_fputc);
HOST_OBSERVER("fputs", obs_fputs);
HOST_OBSERVER("fprintf", obs_fprintf);
