// x86Code: stand-in for the Mac host's fseventsd (com.apple.FSEvents) inside the iOS simulator.
//
// On a Mac, fseventsd reads the kernel's /dev/fsevents and serves FSEvents.framework's clients over MIG
// (subsystem 66000, "f2d" = framework to daemon; events go back on the client's port, subsystem 67000, "d2f").
// A simulator's FSEvents.framework looks the service up by name like a Mac process. Without it,
// FSEventsCopyUUIDForDevice() failed ("FSEvents_connect() => Unknown service name"): the On My iPhone
// FileProvider extension (LocalStorageFileProvider) had no volume UUID, no live updates ("can't create notifier
// ..., enumeration will have no live update") and failed item lookups.
//
// This job (cli/libexec/rundev.sh: com.x86code.fseventsd, MachServices com.apple.FSEvents) implements the
// protocol, reconstructed from the runtime's FSEvents.framework MIG stubs (docs/protocols/FSEVENTS.md), and
// watches the registered paths with Linux inotify (Darling-private syscall 598, x86code_inotify.c). There is no
// event history across boots: a stream that asks for events since an id before this daemon started gets
// MustScanSubDirs for each of its paths, then HistoryDone. Event ids and the volume UUID persist in
// $HOME/var/db/x86code-fseventsd so they don't go backwards between boots.
#include <mach/mach.h>
// bootstrap.h is not in the iOS SDK
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_check_in(mach_port_t bp, const char* name, mach_port_t* sp);
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <uuid/uuid.h>

extern int syscall(int, ...);

static FILE* logf_;
static int verbose;
static void LOG(const char* fmt, ...) {
	if (!logf_) return;
	struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
	struct tm tm; localtime_r(&ts.tv_sec, &tm);
	fprintf(logf_, "%02d:%02d:%02d.%03ld ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);
	va_list ap; va_start(ap, fmt); vfprintf(logf_, fmt, ap); va_end(ap);
	fputc('\n', logf_); fflush(logf_);
}

// ---- Linux inotify through Darling (syscall 598) ----
#define SYS_X86CODE_INOTIFY 598
#define L_IN_MODIFY 0x2
#define L_IN_ATTRIB 0x4
#define L_IN_CLOSE_WRITE 0x8
#define L_IN_MOVED_FROM 0x40
#define L_IN_MOVED_TO 0x80
#define L_IN_CREATE 0x100
#define L_IN_DELETE 0x200
#define L_IN_DELETE_SELF 0x400
#define L_IN_MOVE_SELF 0x800
#define L_IN_Q_OVERFLOW 0x4000
#define L_IN_IGNORED 0x8000
#define L_IN_ONLYDIR 0x01000000
#define L_IN_DONT_FOLLOW 0x02000000
#define L_IN_ISDIR 0x40000000
#define L_IN_CLOEXEC 0x80000
#define WATCH_MASK (L_IN_MODIFY | L_IN_ATTRIB | L_IN_CLOSE_WRITE | L_IN_MOVED_FROM | L_IN_MOVED_TO | L_IN_CREATE | \
	L_IN_DELETE | L_IN_DELETE_SELF | L_IN_MOVE_SELF | L_IN_ONLYDIR | L_IN_DONT_FOLLOW)
struct linux_inotify_event { int wd; uint32_t mask, cookie, len; char name[]; };

static int ino_fd = -1;

// ---- raw event flags as the daemon sends them (FSEvents.framework converts them to FSEventStreamEventFlags in
// implementation_callback_rpc) ----
#define R_CREATED (1u << 0)        // kFSEventStreamEventFlagItemCreated
#define R_REMOVED (1u << 1)        // ItemRemoved
#define R_INODEMETA (1u << 2)      // ItemInodeMetaMod
#define R_RENAMED (1u << 3)        // ItemRenamed
#define R_MODIFIED (1u << 4)       // ItemModified
#define R_XATTR (1u << 9)          // ItemXattrMod
#define R_ISSYMLINK (1u << 22)
#define R_ISFILE (1u << 23)
#define R_ISDIR (1u << 24)
#define R_HISTORYDONE (1u << 27)
#define R_MUSTSCAN (1u << 29)      // MustScanSubDirs
#define R_USERDROPPED (1u << 30)

// stream create flags
#define F_FILEEVENTS 0x10

// ---- state ----
struct batch;
struct stream {
	mach_port_t priv;       // our receive right, the client's handle for flush/unregister
	mach_port_t client;     // send right to the client's d2f port
	int dev;
	uint32_t flags;
	int npaths, nexcl;
	char** paths;           // as the client gave them (relative to the device's mount point if dev != 0)
	char** roots;           // absolute
	char** excl;
	char mount[1024];       // dev != 0: the mount point paths are relative to
	int dropped;
	// x86Code: latency, as fseventsd applies it: events wait in `pend` until `due` (first event + latency, or with
	// kFSEventStreamCreateFlagNoDefer the first event of a quiet period goes out at once); flush sends them now
	double latency;
	struct batch* pend;
	double due, last_sent;
	uint64_t last_id;       // id of the last event sent to this stream (flush answers it: FlushSync waits for it)
	struct stream* next;
};
static struct stream* streams;
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static mach_port_t service, portset;
static uint64_t cur_id, lease_id, start_id;
static char uuid_str[37];
static char state_dir[1024];

// inotify watch descriptors are small increasing numbers: the table is indexed by wd
static char** wdpath;
static int capwd, nwatches;
// directories watched with their whole tree (a new stream path inside one needs nothing more)
static char** trees;
static int ntrees;
static char device_home[1024];  // the device's data directory ($HOME)
#define MAX_WATCHES 200000

// ---- persistent ids ----
static void save_lease(void) {
	char p[1100]; snprintf(p, sizeof p, "%s/eventid", state_dir);
	char t[1100]; snprintf(t, sizeof t, "%s.tmp", p);
	FILE* f = fopen(t, "w");
	if (!f) return;
	fprintf(f, "%llu\n", (unsigned long long) lease_id);
	fclose(f);
	rename(t, p);
}
static uint64_t next_id(void) {
	cur_id++;
	if (cur_id >= lease_id) { lease_id = cur_id + 10000; save_lease(); }
	return cur_id;
}
static void load_state(void) {
	const char* home = getenv("HOME");
	snprintf(state_dir, sizeof state_dir, "%s/var/db/x86code-fseventsd", home ? home : "/tmp");
	char p[1100];
	snprintf(p, sizeof p, "%s/var/db", home ? home : "/tmp"); mkdir(p, 0755);
	mkdir(state_dir, 0755);
	snprintf(p, sizeof p, "%s/eventid", state_dir);
	FILE* f = fopen(p, "r");
	unsigned long long v = 0;
	if (f) { if (fscanf(f, "%llu", &v) != 1) v = 0; fclose(f); }
	cur_id = v ? v : 1000;
	start_id = cur_id;
	lease_id = cur_id;
	next_id();
	snprintf(p, sizeof p, "%s/uuid", state_dir);
	f = fopen(p, "r");
	if (f) { if (!fgets(uuid_str, sizeof uuid_str, f)) uuid_str[0] = 0; fclose(f); }
	if (strlen(uuid_str) != 36) {
		uuid_t u; uuid_generate_random(u); uuid_unparse_upper(u, uuid_str);
		f = fopen(p, "w");
		if (f) { fputs(uuid_str, f); fclose(f); }
	}
}

// ---- watches ----
static const char* watch_path(int wd) {
	return wd >= 0 && wd < capwd ? wdpath[wd] : NULL;
}
static void watch_forget(int wd) {
	if (wd >= 0 && wd < capwd && wdpath[wd]) { free(wdpath[wd]); wdpath[wd] = NULL; nwatches--; }
}
static void watch_add(const char* path) {
	if (ino_fd < 0 || nwatches >= MAX_WATCHES) return;
	long wd = syscall(SYS_X86CODE_INOTIFY, 1, ino_fd, path, WATCH_MASK);
	if (wd < 0) { if (verbose) LOG("  watch %s: errno %d", path, errno); return; }
	if (wd >= capwd) {
		int n = capwd ? capwd : 1024;
		while (n <= wd) n *= 2;
		wdpath = realloc(wdpath, n * sizeof *wdpath);
		memset(wdpath + capwd, 0, (n - capwd) * sizeof *wdpath);
		capwd = n;
	}
	if (wdpath[wd]) { if (strcmp(wdpath[wd], path)) { free(wdpath[wd]); wdpath[wd] = strdup(path); } return; }
	wdpath[wd] = strdup(path);
	nwatches++;
}
static void watch_tree(const char* path, int depth) {
	watch_add(path);
	if (depth > 64) return;
	DIR* d = opendir(path);
	if (!d) return;
	struct dirent* e;
	while ((e = readdir(d))) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
		if (e->d_type != DT_DIR) continue;
		char sub[2048];
		snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
		watch_tree(sub, depth + 1);
	}
	closedir(d);
}

// ---- d2f: callback_rpc (67000) ----
#pragma pack(push, 4)
typedef struct {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_ool_descriptor_t offsets, flags, ids, paths;
	NDR_record_t ndr;
	int n;
	mach_msg_type_number_t offsetsCnt, flagsCnt, idsCnt, pathsCnt;
} callback_msg_t;
#pragma pack(pop)

struct batch { int n, cap; uint32_t* offs; uint32_t* flags; uint64_t* ids; char* blob; size_t blen, bcap; };
static void batch_add(struct batch* b, const char* path, uint32_t flags, uint64_t id) {
	// coalesce with an earlier event for the same path in this batch
	for (int i = 0; i < b->n; i++) if (!strcmp(b->blob + b->offs[i], path)) { b->flags[i] |= flags; b->ids[i] = id; return; }
	if (b->n == b->cap) {
		b->cap = b->cap ? b->cap * 2 : 16;
		b->offs = realloc(b->offs, b->cap * 4); b->flags = realloc(b->flags, b->cap * 4); b->ids = realloc(b->ids, b->cap * 8);
	}
	size_t l = strlen(path) + 1;
	if (b->blen + l > b->bcap) { b->bcap = (b->blen + l) * 2 + 256; b->blob = realloc(b->blob, b->bcap); }
	memcpy(b->blob + b->blen, path, l);
	b->offs[b->n] = (uint32_t) b->blen; b->flags[b->n] = flags; b->ids[b->n] = id; b->n++;
	b->blen += l;
}
static void batch_free(struct batch* b) { free(b->offs); free(b->flags); free(b->ids); free(b->blob); memset(b, 0, sizeof *b); }
static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static pthread_cond_t due_cv = PTHREAD_COND_INITIALIZER;
#define F_NODEFER 0x02

// returns the mach_msg result
static kern_return_t send_batch(struct stream* s, struct batch* b) {
	if (!b->n || !s->client) return KERN_SUCCESS;
	callback_msg_t m;
	memset(&m, 0, sizeof m);
	m.head.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
	m.head.msgh_size = sizeof m;
	m.head.msgh_remote_port = s->client;
	m.head.msgh_id = 67000;
	m.body.msgh_descriptor_count = 4;
	mach_msg_ool_descriptor_t* d[4] = { &m.offsets, &m.flags, &m.ids, &m.paths };
	void* a[4] = { b->offs, b->flags, b->ids, b->blob };
	mach_msg_size_t sz[4] = { b->n * 4, b->n * 4, b->n * 8, (mach_msg_size_t) b->blen };
	for (int i = 0; i < 4; i++) {
		d[i]->address = a[i]; d[i]->size = sz[i]; d[i]->deallocate = FALSE;
		d[i]->copy = MACH_MSG_VIRTUAL_COPY; d[i]->type = MACH_MSG_OOL_DESCRIPTOR;
	}
	m.ndr = NDR_record;
	m.n = b->n;
	m.offsetsCnt = b->n; m.flagsCnt = b->n; m.idsCnt = b->n; m.pathsCnt = (mach_msg_type_number_t) b->blen;
	kern_return_t kr = mach_msg(&m.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, sizeof m, 0, MACH_PORT_NULL, 200, MACH_PORT_NULL);
	if (verbose || kr) LOG("  -> stream %#x: %d event(s), first %s flags %#x: kr %#x", s->priv, b->n, b->blob, b->flags[0], kr);
	return kr;
}

// path relative to the device's mount point for dev-relative streams
static const char* report_path(struct stream* s, const char* abs) {
	if (!s->dev) return abs;
	size_t ml = strlen(s->mount);
	if (!strncmp(abs, s->mount, ml)) { abs += ml; while (*abs == '/') abs++; }
	return abs;
}
static int under(const char* path, const char* root) {
	size_t rl = strlen(root);
	while (rl > 1 && root[rl - 1] == '/') rl--;
	if (rl == 1 && root[0] == '/') return path[0] == '/';
	return !strncmp(path, root, rl) && (path[rl] == 0 || path[rl] == '/');
}

static void remove_stream_locked(struct stream* s);

// a new directory is watched if it is in a stream's tree or on the way to a stream path that doesn't exist yet
static int wanted(const char* dir) {
	for (struct stream* s = streams; s; s = s->next)
		for (int k = 0; k < s->npaths; k++)
			if (under(dir, s->roots[k]) || under(s->roots[k], dir)) return 1;
	return 0;
}
static struct batch hist;   // history answer of the last register, sent after its reply (server loop only)

// one Linux event: queue it for every stream that watches the path
struct pending { struct stream* s; struct batch b; };
static void dispatch_event(struct pending* pend, int np, const char* full, uint32_t raw, int isdir) {
	char parent[2048];
	snprintf(parent, sizeof parent, "%s", full);
	char* sl = strrchr(parent, '/');
	if (sl && sl != parent) *sl = 0;
	for (int i = 0; i < np; i++) {
		struct stream* s = pend[i].s;
		int match = 0;
		for (int k = 0; k < s->npaths && !match; k++) match = under(full, s->roots[k]) || (!(s->flags & F_FILEEVENTS) && under(parent, s->roots[k]));
		for (int k = 0; k < s->nexcl && match; k++) if (under(full, s->excl[k])) match = 0;
		if (!match) continue;
		uint64_t id = next_id();
		if (s->flags & F_FILEEVENTS) {
			batch_add(&pend[i].b, report_path(s, full), raw | (isdir ? R_ISDIR : R_ISFILE), id);
		} else {
			char dir[2100];
			snprintf(dir, sizeof dir, "%s/", report_path(s, parent));
			batch_add(&pend[i].b, dir, 0, id);
		}
	}
}

// x86Code: send a stream's pending events (mu held)
static void send_pending(struct stream* st) {
	st->due = 0;
	if (!st->pend || !st->pend->n) return;
	st->last_id = st->pend->ids[st->pend->n - 1];
	kern_return_t kr = send_batch(st, st->pend);
	if (kr == MACH_SEND_INVALID_DEST) { mach_port_deallocate(mach_task_self(), st->client); st->client = MACH_PORT_NULL; }
	else if (kr) st->dropped = 1;
	batch_free(st->pend);
	st->last_sent = now_s();
}

static void* latency_thread(void* arg) {
	pthread_mutex_lock(&mu);
	for (;;) {
		double t = now_s(), next = 0;
		for (struct stream* st = streams; st; st = st->next) {
			if (!st->due) continue;
			if (st->due <= t) send_pending(st);
			else if (!next || st->due < next) next = st->due;
		}
		if (!next) pthread_cond_wait(&due_cv, &mu);
		else {
			struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
			double w = next - t, sec = ts.tv_sec + ts.tv_nsec / 1e9 + w;
			ts.tv_sec = (time_t) sec; ts.tv_nsec = (long) ((sec - ts.tv_sec) * 1e9);
			pthread_cond_timedwait(&due_cv, &mu, &ts);
		}
	}
	return NULL;
}

static void* inotify_thread(void* arg) {
	static char buf[64 * 1024] __attribute__((aligned(8)));
	for (;;) {
		ssize_t n = read(ino_fd, buf, sizeof buf);
		if (n < 0) { if (errno == EINTR) continue; LOG("inotify read: errno %d", errno); sleep(1); continue; }
		pthread_mutex_lock(&mu);
		int np = 0;
		for (struct stream* s = streams; s; s = s->next) np++;
		struct pending* pend = calloc(np ? np : 1, sizeof *pend);
		int i = 0;
		for (struct stream* s = streams; s; s = s->next) pend[i++].s = s;
		for (char* p = buf; p < buf + n; ) {
			struct linux_inotify_event* e = (void*) p;
			p += sizeof *e + e->len;
			if (e->mask & L_IN_Q_OVERFLOW) {
				LOG("inotify queue overflow");
				for (int k = 0; k < np; k++)
					for (int r = 0; r < pend[k].s->npaths; r++) {
						char rp[2100]; snprintf(rp, sizeof rp, "%s/", report_path(pend[k].s, pend[k].s->roots[r]));
						batch_add(&pend[k].b, rp, R_MUSTSCAN | R_USERDROPPED, next_id());
					}
				continue;
			}
			const char* dir = watch_path(e->wd);
			if (!dir) continue;
			if (e->mask & L_IN_IGNORED) { watch_forget(e->wd); continue; }
			char full[2048];
			if (e->len && e->name[0]) snprintf(full, sizeof full, "%s/%s", dir, e->name);
			else snprintf(full, sizeof full, "%s", dir);
			int isdir = (e->mask & L_IN_ISDIR) != 0;
			uint32_t raw = 0;
			if (e->mask & L_IN_CREATE) raw |= R_CREATED;
			if (e->mask & (L_IN_DELETE | L_IN_DELETE_SELF)) raw |= R_REMOVED;
			if (e->mask & (L_IN_MOVED_FROM | L_IN_MOVED_TO | L_IN_MOVE_SELF)) raw |= R_RENAMED;
			if (e->mask & (L_IN_MODIFY | L_IN_CLOSE_WRITE)) raw |= R_MODIFIED;
			if (e->mask & L_IN_ATTRIB) raw |= R_INODEMETA;
			if (e->mask & (L_IN_DELETE_SELF | L_IN_MOVE_SELF)) {
				isdir = 1;
				if (e->len) continue;   // reported with the name in the parent as well
			}
			if (verbose) LOG("inotify %s mask %#x", full, e->mask);
			// new directories get watches of their own (and what's already in them)
			if (isdir && (e->mask & (L_IN_CREATE | L_IN_MOVED_TO)) && wanted(full)) watch_tree(full, 0);
			if (np) dispatch_event(pend, np, full, raw, isdir);
		}
		double t = now_s();
		for (int k = 0; k < np; k++) {
			struct stream* st = pend[k].s;
			if (pend[k].b.n) {
				// x86Code: into the stream's pending batch; sent by the latency thread when due
				if (!st->pend) st->pend = calloc(1, sizeof *st->pend);
				for (int e = 0; e < pend[k].b.n; e++)
					batch_add(st->pend, pend[k].b.blob + pend[k].b.offs[e], pend[k].b.flags[e], pend[k].b.ids[e]);
				if (!st->due) {
					st->due = (st->flags & F_NODEFER) && t - st->last_sent >= st->latency ? t : t + st->latency;
					pthread_cond_signal(&due_cv);
				}
			}
			batch_free(&pend[k].b);
		}
		free(pend);
		pthread_mutex_unlock(&mu);
	}
	return NULL;
}

// ---- f2d server ----
#pragma pack(push, 4)
typedef struct {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t client;
	mach_msg_ool_descriptor_t offsets, paths, exclOffsets, exclPaths;
	NDR_record_t ndr;
	int dev;
	int npaths;
	mach_msg_type_number_t offsetsCnt, pathsCnt;
	int nexcl;
	mach_msg_type_number_t exclOffsetsCnt, exclPathsCnt;
	uint64_t latency_us;    // x86Code: microseconds (1.5 s = 1500000); was taken for an f64 and read as 0
	uint32_t flags;
	uint64_t since;
} register_req_t;
typedef struct {
	mach_msg_header_t head;
	mach_msg_body_t body;
	mach_msg_port_descriptor_t port;
} register_rep_t;
typedef struct { mach_msg_header_t head; NDR_record_t ndr; kern_return_t ret; } rc_rep_t;
typedef struct { mach_msg_header_t head; NDR_record_t ndr; kern_return_t ret; uint64_t id; } id_rep_t;
typedef struct { mach_msg_header_t head; NDR_record_t ndr; kern_return_t ret; char uuid[36]; } uuid_rep_t;
typedef struct { mach_msg_header_t head; NDR_record_t ndr; int dev; } dev_req_t;
typedef struct { mach_msg_header_t head; NDR_record_t ndr; int dev; uint64_t v; } devv_req_t;
typedef struct { mach_msg_header_t head; NDR_record_t ndr; kern_return_t ret; uint32_t v; } u32_rep_t;
#pragma pack(pop)

static char** unpack(const char* blob, size_t blen, const uint32_t* offs, int n) {
	char** v = calloc(n ? n : 1, sizeof *v);
	for (int i = 0; i < n; i++)
		v[i] = strndup(offs[i] < blen ? blob + offs[i] : "", offs[i] < blen ? blen - offs[i] : 0);
	return v;
}

static int mount_for_dev(int dev, char* out, size_t outlen) {
	struct statfs* m;
	int n = getmntinfo(&m, MNT_NOWAIT);
	for (int i = 0; i < n; i++) {
		struct stat st;
		if (stat(m[i].f_mntonname, &st) == 0 && (int) st.st_dev == dev) { snprintf(out, outlen, "%s", m[i].f_mntonname); return 0; }
	}
	struct stat st;
	if (stat("/", &st) == 0 && (int) st.st_dev == dev) { snprintf(out, outlen, "/"); return 0; }
	return -1;
}

static void free_stream(struct stream* s) {
	for (int i = 0; i < s->npaths; i++) { free(s->paths[i]); free(s->roots[i]); }
	for (int i = 0; i < s->nexcl; i++) free(s->excl[i]);
	if (s->pend) { batch_free(s->pend); free(s->pend); }
	free(s->paths); free(s->roots); free(s->excl); free(s);
}
static void remove_stream_locked(struct stream* s) {
	for (struct stream** pp = &streams; *pp; pp = &(*pp)->next) if (*pp == s) { *pp = s->next; break; }
	LOG("stream %#x removed", s->priv);
	mach_port_move_member(mach_task_self(), s->priv, MACH_PORT_NULL);
	mach_port_mod_refs(mach_task_self(), s->priv, MACH_PORT_RIGHT_RECEIVE, -1);
	if (s->client) mach_port_deallocate(mach_task_self(), s->client);
	free_stream(s);
}
static struct stream* find_stream(mach_port_t priv) {
	for (struct stream* s = streams; s; s = s->next) if (s->priv == priv) return s;
	return NULL;
}

static void handle_register(register_req_t* r, mach_msg_header_t* rep) {
	struct stream* s = calloc(1, sizeof *s);
	s->client = r->client.name;
	s->dev = r->dev;
	s->flags = r->flags;
	s->latency = r->latency_us < 3600000000ULL ? r->latency_us / 1e6 : 0;
	s->npaths = r->npaths;
	s->nexcl = r->nexcl;
	s->paths = unpack(r->paths.address, r->paths.size, r->offsets.address, r->npaths);
	s->excl = unpack(r->exclPaths.address, r->exclPaths.size, r->exclOffsets.address, r->nexcl);
	s->roots = calloc(s->npaths ? s->npaths : 1, sizeof *s->roots);
	if (s->dev && mount_for_dev(s->dev, s->mount, sizeof s->mount) < 0) LOG("  no mount point for dev %d", s->dev);
	for (int i = 0; i < s->npaths; i++) {
		char abs[2048];
		if (s->dev) snprintf(abs, sizeof abs, "%s/%s", strcmp(s->mount, "/") ? s->mount : "", s->paths[i]);
		else snprintf(abs, sizeof abs, "%s", s->paths[i]);
		size_t l = strlen(abs);
		while (l > 1 && abs[l - 1] == '/') abs[--l] = 0;
		s->roots[i] = strdup(abs);
	}
	vm_deallocate(mach_task_self(), (vm_address_t) r->paths.address, r->paths.size);
	vm_deallocate(mach_task_self(), (vm_address_t) r->offsets.address, r->offsets.size);
	vm_deallocate(mach_task_self(), (vm_address_t) r->exclPaths.address, r->exclPaths.size);
	vm_deallocate(mach_task_self(), (vm_address_t) r->exclOffsets.address, r->exclOffsets.size);
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &s->priv);
	mach_port_move_member(mach_task_self(), s->priv, portset);
	// the client dying: dead-name notification on its d2f port, to the service port
	mach_port_t prev = MACH_PORT_NULL;
	mach_port_request_notification(mach_task_self(), s->client, MACH_NOTIFY_DEAD_NAME, 0, service,
		MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
	if (prev) mach_port_deallocate(mach_task_self(), prev);
	// and the stream port losing its last sender (the client gone without unregistering), to the stream port
	prev = MACH_PORT_NULL;
	mach_port_request_notification(mach_task_self(), s->priv, MACH_NOTIFY_NO_SENDERS, 1, s->priv,
		MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
	if (prev) mach_port_deallocate(mach_task_self(), prev);

	LOG("register stream %#x dev %d flags %#x latency %.3f since %llu, %d path(s), %d excluded", s->priv, s->dev,
		s->flags, s->latency, (unsigned long long) r->since, s->npaths, s->nexcl);
	for (int i = 0; i < s->npaths; i++) LOG("  path %s", s->roots[i]);

	pthread_mutex_lock(&mu);
	for (int i = 0; i < s->npaths; i++) {
		// a path that doesn't exist yet: watch its nearest existing ancestor, whose new directories are watched in
		// turn (fseventsd sees the whole volume, so a stream gets the creation of its path)
		struct stat st;
		char a[2048];
		snprintf(a, sizeof a, "%s", s->roots[i]);
		while (stat(a, &st) != 0) {
			char* sl = strrchr(a, '/');
			if (!sl || sl == a) { strcpy(a, "/"); break; }
			*sl = 0;
		}
		if (strcmp(a, s->roots[i])) { watch_add(a); continue; }
		// x86Code: a stream on "/" (or another ancestor of the device) would watch all of Linux through
		// /Volumes/SystemRoot; what the simulator changes is in its data directory, so that tree is watched instead
		if (device_home[0] && under(device_home, a) && strcmp(device_home, a)) { watch_add(a); snprintf(a, sizeof a, "%s", device_home); }
		int covered = 0;
		for (int t = 0; t < ntrees && !covered; t++) covered = under(a, trees[t]);
		if (covered) continue;
		watch_tree(a, 0);
		trees = realloc(trees, (ntrees + 1) * sizeof *trees);
		trees[ntrees++] = strdup(a);
	}
	LOG("  %d inotify watches", nwatches);
	s->next = streams;
	streams = s;
	pthread_mutex_unlock(&mu);

	register_rep_t* p = (register_rep_t*) rep;
	p->head.msgh_bits |= MACH_MSGH_BITS_COMPLEX;
	p->head.msgh_size = sizeof *p;
	p->body.msgh_descriptor_count = 1;
	p->port.name = s->priv;
	p->port.disposition = MACH_MSG_TYPE_MAKE_SEND;
	p->port.type = MACH_MSG_PORT_DESCRIPTOR;

	// history: none kept. A stream that starts in the past is told to rescan, then that history is done.
	if (r->since != UINT64_MAX) {
		struct batch b = { 0 };
		pthread_mutex_lock(&mu);
		if (r->since < start_id)
			for (int i = 0; i < s->npaths; i++) {
				char rp[2100]; snprintf(rp, sizeof rp, "%s/", report_path(s, s->roots[i]));
				batch_add(&b, rp, R_MUSTSCAN, next_id());
			}
		batch_add(&b, "", R_HISTORYDONE, cur_id);
		pthread_mutex_unlock(&mu);
		// sent after the reply (the client registers its port in its stream table once the RPC returns)
		batch_free(&hist);
		hist = b;
	}
}

int main(int argc, char** argv) {
	const char* home = getenv("HOME");
	char p[1024];
	snprintf(p, sizeof p, "%s/var/log/fseventsd.log", home ? home : "/tmp");
	logf_ = fopen(p, "a");
	verbose = getenv("X86CODE_FSEVENTS_LOG") != NULL;
	load_state();
	if (home) {
		// the device's data directory as simulator processes name it (FSEvents clients give real paths)
		char rp[1024];
		snprintf(device_home, sizeof device_home, "%s", realpath(home, rp) ? rp : home);
	}
	// X86CODE_FSEVENTS_LOG or a file "verbose" in the state directory: log every event and send
	snprintf(p, sizeof p, "%s/verbose", state_dir);
	if (access(p, F_OK) == 0) verbose = 1;
	LOG("fseventsd: uuid %s, event id %llu", uuid_str, (unsigned long long) cur_id);

	ino_fd = (int) syscall(SYS_X86CODE_INOTIFY, 0, L_IN_CLOEXEC, 0, 0);
	if (ino_fd < 0) LOG("inotify_init1: errno %d (no live events)", errno);

	kern_return_t kr = bootstrap_check_in(bootstrap_port, "com.apple.FSEvents", &service);
	if (kr) { LOG("bootstrap_check_in com.apple.FSEvents: %#x", kr); return 1; }
	mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_PORT_SET, &portset);
	mach_port_move_member(mach_task_self(), service, portset);

	if (ino_fd >= 0) { pthread_t t; pthread_create(&t, NULL, inotify_thread, NULL); }
	{ pthread_t t; pthread_create(&t, NULL, latency_thread, NULL); }

	union { mach_msg_header_t head; char buf[8192]; } req;
	union { mach_msg_header_t head; char buf[512]; } rep;
	for (;;) {
		kr = mach_msg(&req.head, MACH_RCV_MSG | MACH_RCV_LARGE, 0, sizeof req, portset, MACH_MSG_TIMEOUT_NONE, MACH_PORT_NULL);
		if (kr == MACH_RCV_TOO_LARGE) {
			LOG("message too large (%u)", req.head.msgh_size);
			mach_msg(&req.head, MACH_RCV_MSG, 0, sizeof req, portset, 0, MACH_PORT_NULL);  // not reached usually
			continue;
		}
		if (kr) { LOG("mach_msg receive: %#x", kr); continue; }
		mach_msg_header_t* h = &req.head;
		int id = h->msgh_id;

		if (id == MACH_NOTIFY_DEAD_NAME) {
			mach_port_t dead = ((mach_dead_name_notification_t*) h)->not_port;
			// the client's d2f port is gone: no more events. The stream itself stays until its port has no senders
			// or the client unregisters (an unregister already on its way must get an answer: the client logs
			// "f2d_unregister_rpc() => server died" otherwise)
			pthread_mutex_lock(&mu);
			for (struct stream* s = streams; s; s = s->next)
				if (s->client == dead) { mach_port_deallocate(mach_task_self(), s->client); s->client = MACH_PORT_NULL; }
			pthread_mutex_unlock(&mu);
			mach_port_deallocate(mach_task_self(), dead);
			continue;
		}
		if (id == MACH_NOTIFY_NO_SENDERS) {
			pthread_mutex_lock(&mu);
			struct stream* s = find_stream(h->msgh_local_port);
			if (s) remove_stream_locked(s);
			pthread_mutex_unlock(&mu);
			continue;
		}

		memset(&rep, 0, sizeof rep);
		rc_rep_t* rc = (rc_rep_t*) &rep;
		rc->head.msgh_bits = MACH_MSGH_BITS(MACH_MSGH_BITS_REMOTE(h->msgh_bits), 0);
		rc->head.msgh_remote_port = h->msgh_remote_port;
		rc->head.msgh_id = id + 100;
		rc->head.msgh_size = sizeof *rc;
		rc->ndr = NDR_record;
		rc->ret = 0;
		struct stream* hist_stream = NULL;

		switch (id) {
		case 66000:
			if (!(h->msgh_bits & MACH_MSGH_BITS_COMPLEX) || h->msgh_size < sizeof(register_req_t)) { rc->ret = MIG_BAD_ARGUMENTS; break; }
			handle_register((register_req_t*) h, &rep.head);
			hist_stream = streams;
			break;
		case 66001: {  // flush (stream port): the stream's pending events go out now; answer the current id
			id_rep_t* r = (id_rep_t*) &rep;
			r->head.msgh_size = sizeof *r;
			pthread_mutex_lock(&mu);
			struct stream* fs = find_stream(h->msgh_local_port);
			if (fs) send_pending(fs);
			// x86Code: the stream's own last event: FSEventStreamFlushSync waits until its callback has seen this
			// id; the global current id (events of other streams included) made it wait forever
			r->id = fs ? fs->last_id : cur_id;
			pthread_mutex_unlock(&mu);
			if (verbose) LOG("flush %#x", h->msgh_local_port);
			break;
		}
		case 66002: {  // get_server_uuid(dev)
			uuid_rep_t* r = (uuid_rep_t*) &rep;
			r->head.msgh_size = sizeof *r;
			memcpy(r->uuid, uuid_str, 36);
			LOG("get_server_uuid dev %d -> %s", ((dev_req_t*) h)->dev, uuid_str);
			break;
		}
		case 66003: {  // get_current_event_id
			id_rep_t* r = (id_rep_t*) &rep;
			r->head.msgh_size = sizeof *r;
			pthread_mutex_lock(&mu); r->id = cur_id; pthread_mutex_unlock(&mu);
			break;
		}
		case 66004: {  // get_last_event_for_device_before_time(dev, time): no history, the current id
			id_rep_t* r = (id_rep_t*) &rep;
			r->head.msgh_size = sizeof *r;
			pthread_mutex_lock(&mu); r->id = cur_id; pthread_mutex_unlock(&mu);
			LOG("get_last_event_for_device_before_time dev %d -> %llu", ((devv_req_t*) h)->dev, (unsigned long long) r->id);
			break;
		}
		case 66005: {  // purge_events_for_device_up_to_event_id: nothing kept
			u32_rep_t* r = (u32_rep_t*) &rep;
			r->head.msgh_size = sizeof *r;
			LOG("purge dev %d up to %llu", ((devv_req_t*) h)->dev, (unsigned long long) ((devv_req_t*) h)->v);
			break;
		}
		case 66006: {  // unregister (stream port)
			pthread_mutex_lock(&mu);
			struct stream* s = find_stream(h->msgh_local_port);
			if (s) remove_stream_locked(s);
			pthread_mutex_unlock(&mu);
			break;
		}
		default:
			LOG("unknown message id %d on %#x", id, h->msgh_local_port);
			rc->ret = MIG_BAD_ID;
			mach_msg_destroy(h);
			break;
		}
		if (rep.head.msgh_remote_port) {
			kr = mach_msg(&rep.head, MACH_SEND_MSG | MACH_SEND_TIMEOUT, rep.head.msgh_size, 0, MACH_PORT_NULL, 1000, MACH_PORT_NULL);
			if (kr) { LOG("reply to %d: %#x", id, kr); if (kr != MACH_SEND_INVALID_DEST) mach_msg_destroy(&rep.head); }
		}
		if (hist_stream && hist.n) {
			pthread_mutex_lock(&mu);
			send_batch(hist_stream, &hist);
			pthread_mutex_unlock(&mu);
			batch_free(&hist);
		}
	}
	return 0;
}
