// x86Code simulator shim, injected into every simulator process (mksimdevice.py).
// Stands in for host services a Linux host does not provide:
//
// - System power: IORegisterForSystemPower needs IOPMrootDomain, which Darling's
//   iokitd lacks. Return a working registration whose notification port never fires
//   (the "machine" never sleeps). Interposition only reaches callers outside the dyld
//   shared cache (plugins, daemons' own code), which is where these calls come from.
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#define DYLD_INTERPOSE(_replacement, _replacee) \
	__attribute__((used)) static struct { const void* replacement; const void* replacee; } _interpose_##_replacee \
	__attribute__((section("__DATA,__interpose"))) = { (const void*) (unsigned long) &_replacement, (const void*) (unsigned long) &_replacee };

typedef void (*IOServiceInterestCallback)(void* refcon, io_service_t service, uint32_t messageType, void* messageArgument);
extern io_connect_t IORegisterForSystemPower(void* refcon, IONotificationPortRef* thePortRef,
	IOServiceInterestCallback callback, io_object_t* notifier);
extern IOReturn IODeregisterForSystemPower(io_object_t* notifier);
extern IOReturn IOAllowPowerChange(io_connect_t kernelPort, intptr_t notificationID);
extern IOReturn IOCancelPowerChange(io_connect_t kernelPort, intptr_t notificationID);

static mach_port_t fake_port(void)
{
	mach_port_t port = MACH_PORT_NULL;
	if (mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &port) == KERN_SUCCESS)
		mach_port_insert_right(mach_task_self(), port, port, MACH_MSG_TYPE_MAKE_SEND);
	return port;
}

static io_connect_t shim_IORegisterForSystemPower(void* refcon, IONotificationPortRef* thePortRef,
	IOServiceInterestCallback callback, io_object_t* notifier)
{
	io_connect_t real = IORegisterForSystemPower(refcon, thePortRef, callback, notifier);
	if (real != MACH_PORT_NULL)
		return real;
	if (thePortRef)
		*thePortRef = IONotificationPortCreate(MACH_PORT_NULL);
	if (notifier)
		*notifier = fake_port();
	return fake_port();
}
DYLD_INTERPOSE(shim_IORegisterForSystemPower, IORegisterForSystemPower)

static IOReturn shim_IODeregisterForSystemPower(io_object_t* notifier) { return kIOReturnSuccess; }
DYLD_INTERPOSE(shim_IODeregisterForSystemPower, IODeregisterForSystemPower)

static IOReturn shim_IOAllowPowerChange(io_connect_t kernelPort, intptr_t id) { return kIOReturnSuccess; }
DYLD_INTERPOSE(shim_IOAllowPowerChange, IOAllowPowerChange)

// libsystem_sim_kernel's csops_audittoken serves entitlement blobs itself but fails a short
// buffer with ENOENT instead of ERANGE (Darling's csops syscall gets it right), so libxpc gives up on
// any client whose entitlements exceed its first 0x408-byte guess (lsd: containermanagerd
// then refuses it). Route those ops through csops; the audit token names the same pid.
extern int syscall(int, ...);
extern int csops_audittoken(pid_t pid, unsigned int ops, void* useraddr, size_t usersize, audit_token_t* token);
static int shim_csops_audittoken(pid_t pid, unsigned int ops, void* useraddr, size_t usersize, audit_token_t* token)
{
	if (ops == 7 /* CS_OPS_ENTITLEMENTS_BLOB */ || ops == 16 /* CS_OPS_DER_ENTITLEMENTS_BLOB */)
	{
		// the raw syscall: csops() in the simulator is libsystem_sim_kernel's own version
		int r = syscall(169 /* SYS_csops */, pid, ops, useraddr, usersize);
		if (getenv("X86CODE_SHIMLOG"))
		{
			char line[128];
			int n = snprintf(line, sizeof(line), "simshim: csops_audittoken(%d, %u, %zu) -> %d errno %d\n", pid, ops, usersize, r, r ? errno : 0);
			write(2, line, n);
		}
		return r;
	}
	return csops_audittoken(pid, ops, useraddr, usersize, token);
}
DYLD_INTERPOSE(shim_csops_audittoken, csops_audittoken)

// x86Code: the xpc_init_services() skip that was here is gone. launchd_sim's pid-domain
// uncork reply was lost to a workqueue wakeup race in Darling's sem_down/sem_up
// (libsystem_kernel emulation, workq_kernreturn.c), now fixed.

// Same for plain csops() (Security's SecTask uses it): entitlement blobs via the syscall
extern int csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize);
static int shim_csops(pid_t pid, unsigned int ops, void* useraddr, size_t usersize)
{
	if (ops == 7 /* CS_OPS_ENTITLEMENTS_BLOB */ || ops == 16 /* CS_OPS_DER_ENTITLEMENTS_BLOB */)
		return syscall(169 /* SYS_csops */, pid, ops, useraddr, usersize);
	return csops(pid, ops, useraddr, usersize);
}
DYLD_INTERPOSE(shim_csops, csops)

// Diagnostics: print every Objective-C exception's name and reason to stderr (os_log may be
// off), then throw it as usual
#include <objc/runtime.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <string.h>
#include <objc/message.h>
#include <CoreFoundation/CoreFoundation.h>
extern void objc_exception_throw(id exception) __attribute__((noreturn));
static void shim_print_cfstring(const char* label, CFStringRef s)
{
	char buf[1024] = "(null)";
	if (s)
		CFStringGetCString(s, buf, sizeof(buf), kCFStringEncodingUTF8);
	char line[1200];
	int n = snprintf(line, sizeof(line), "simshim: exception %s: %s\n", label, buf);
	write(2, line, n);
}
static void shim_objc_exception_throw(id exception) __attribute__((noreturn));
static void shim_objc_exception_throw(id exception)
{
	CFStringRef (*msg)(id, SEL) = (CFStringRef (*)(id, SEL)) objc_msgSend;
	shim_print_cfstring("name", msg(exception, sel_registerName("name")));
	shim_print_cfstring("reason", msg(exception, sel_registerName("reason")));
	void* frames[48];
	int n = backtrace(frames, 48);
	for (int i = 0; i < n; i++)
	{
		Dl_info info;
		char line[512];
		int len;
		if (dladdr(frames[i], &info) && info.dli_fname)
			len = snprintf(line, sizeof(line), "simshim:   %p %s+%#lx (%s+%#lx)\n", frames[i], strrchr(info.dli_fname, '/') ? strrchr(info.dli_fname, '/') + 1 : info.dli_fname,
				(unsigned long) ((char*) frames[i] - (char*) info.dli_fbase), info.dli_sname ? info.dli_sname : "?", info.dli_saddr ? (unsigned long) ((char*) frames[i] - (char*) info.dli_saddr) : 0);
		else
			len = snprintf(line, sizeof(line), "simshim:   %p\n", frames[i]);
		write(2, line, len);
	}
	objc_exception_throw(exception);
}
DYLD_INTERPOSE(shim_objc_exception_throw, objc_exception_throw)

// With os_log disabled (OS_ACTIVITY_MODE=disable, there is no logd), os_state_add_handler
// returns 0 and BaseBoard's BSLogAddStateCaptureBlockWithTitle returns nil, which SpringBoard
// adds to a set (PaperBoardUI) and crashes. Hand out dummy handles instead.
typedef uint64_t os_state_handle_t;
extern os_state_handle_t os_state_add_handler(dispatch_queue_t queue, void* block);
extern void os_state_remove_handler(os_state_handle_t handle);
#define SHIM_STATE_HANDLE_BASE 0x7a000000ULL
static os_state_handle_t shim_os_state_add_handler(dispatch_queue_t queue, void* block)
{
	static uint64_t next = SHIM_STATE_HANDLE_BASE;
	os_state_handle_t h = os_state_add_handler(queue, block);
	return h ? h : __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
}
DYLD_INTERPOSE(shim_os_state_add_handler, os_state_add_handler)
static void shim_os_state_remove_handler(os_state_handle_t handle)
{
	if (handle <= SHIM_STATE_HANDLE_BASE || handle > SHIM_STATE_HANDLE_BASE + 0x1000000)
		os_state_remove_handler(handle);
}
DYLD_INTERPOSE(shim_os_state_remove_handler, os_state_remove_handler)

// Diagnostics: log failing sqlite3_open_v2 calls (path, result, message, errno)
#include <sqlite3.h>
static int shim_sqlite3_open_v2(const char* filename, sqlite3** db, int flags, const char* vfs)
{
	int rc = sqlite3_open_v2(filename, db, flags, vfs);
	if (rc != SQLITE_OK)
	{
		char line[1200];
		int n = snprintf(line, sizeof(line), "simshim: sqlite3_open_v2(%s, flags %#x, vfs %s) -> %d %s errno %d\n",
			filename, flags, vfs ? vfs : "-", rc, db && *db ? sqlite3_errmsg(*db) : "?", db && *db ? sqlite3_system_errno(*db) : 0);
		write(2, line, n);
	}
	return rc;
}
DYLD_INTERPOSE(shim_sqlite3_open_v2, sqlite3_open_v2)

// Sandbox extensions: the simulator's libsystem_sandbox issues them through __mac_syscall("Sandbox"), which
// Darling does not implement (ENOSYS), and nothing is sandboxed here anyway. Issue opaque tokens and accept any
// token. Without them PosterBoard could not hand poster paths to its extensions ("cannot extend access ...
// sandbox errno=78") and drew no wallpaper.
#include <string.h>
#include <bsm/audit.h>
extern char* sandbox_extension_issue_file(const char*, const char*, uint32_t);
extern char* sandbox_extension_issue_file_to_process(const char*, const char*, uint32_t, audit_token_t);
extern char* sandbox_extension_issue_file_to_process_by_pid(const char*, const char*, uint32_t, pid_t);
extern char* sandbox_extension_issue_file_to_self(const char*, const char*, uint32_t);
extern char* sandbox_extension_issue_generic(const char*, uint32_t);
extern char* sandbox_extension_issue_generic_to_process(const char*, uint32_t, audit_token_t);
extern char* sandbox_extension_issue_generic_to_process_by_pid(const char*, uint32_t, pid_t);
extern char* sandbox_extension_issue_mach(const char*, const char*, uint32_t);
extern char* sandbox_extension_issue_mach_to_process(const char*, const char*, uint32_t, audit_token_t);
extern char* sandbox_extension_issue_mach_to_process_by_pid(const char*, const char*, uint32_t, pid_t);
extern char* sandbox_extension_issue_iokit_registry_entry_class(const char*, const char*, uint32_t);
extern char* sandbox_extension_issue_iokit_registry_entry_class_to_process(const char*, const char*, uint32_t, audit_token_t);
extern char* sandbox_extension_issue_iokit_registry_entry_class_to_process_by_pid(const char*, const char*, uint32_t, pid_t);
extern int64_t sandbox_extension_consume(const char*);
extern int sandbox_extension_release(int64_t);
static char* fake_token(const char* cls, const char* what)
{
	char buf[1200];
	snprintf(buf, sizeof(buf), "x86code-sandbox-extension;%s;%s", cls ? cls : "", what ? what : "");
	return strdup(buf);
}
#define SB_FILE(name, ...) static char* shim_##name(const char* cls, const char* path, uint32_t flags, ##__VA_ARGS__) { return fake_token(cls, path); } DYLD_INTERPOSE(shim_##name, name)
#define SB_GEN(name, ...) static char* shim_##name(const char* cls, uint32_t flags, ##__VA_ARGS__) { return fake_token(cls, NULL); } DYLD_INTERPOSE(shim_##name, name)
SB_FILE(sandbox_extension_issue_file)
SB_FILE(sandbox_extension_issue_file_to_process, audit_token_t t)
SB_FILE(sandbox_extension_issue_file_to_process_by_pid, pid_t p)
SB_FILE(sandbox_extension_issue_file_to_self)
SB_GEN(sandbox_extension_issue_generic)
SB_GEN(sandbox_extension_issue_generic_to_process, audit_token_t t)
SB_GEN(sandbox_extension_issue_generic_to_process_by_pid, pid_t p)
SB_FILE(sandbox_extension_issue_mach)
SB_FILE(sandbox_extension_issue_mach_to_process, audit_token_t t)
SB_FILE(sandbox_extension_issue_mach_to_process_by_pid, pid_t p)
SB_FILE(sandbox_extension_issue_iokit_registry_entry_class)
SB_FILE(sandbox_extension_issue_iokit_registry_entry_class_to_process, audit_token_t t)
SB_FILE(sandbox_extension_issue_iokit_registry_entry_class_to_process_by_pid, pid_t p)
static int64_t shim_sandbox_extension_consume(const char* token)
{
	if (!token) { errno = EINVAL; return -1; }
	static int64_t next = 1;
	return __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
}
DYLD_INTERPOSE(shim_sandbox_extension_consume, sandbox_extension_consume)
static int shim_sandbox_extension_release(int64_t handle) { return 0; }
DYLD_INTERPOSE(shim_sandbox_extension_release, sandbox_extension_release)

// x86Code: no Metal here. IconFoundation's -[IFConcreteImage ICRIconLayer] (and CALayer, which calls it) builds the icon layer natively
// (ICRIconLayer: needs an RBDevice, i.e. Metal, and the image's layer data, which iconservicesagent cannot
// produce with its software renderer: "layer data size zero", TextureIOError.usingSoftwareRenderer), else nil.
// SpringBoard's home screen asks for native layers only (SBHGetIconLayerFromIconServicesImage without the
// CGImage-fallback option), so every icon loaded asynchronously after a cache miss stayed a gray placeholder
// (only icons served synchronously from a warm cache, which go the UIImage path, had colour). Fall back to a
// plain CALayer with the flat CGImage as contents, as SBHGetIconLayerFromIconServicesImage itself does with
// that option.
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdbool.h>
static IMP shim_ifimage_calayer_orig;
static id shim_ifimage_calayer(id self, SEL sel)
{
	id layer = ((id (*)(id, SEL)) shim_ifimage_calayer_orig)(self, sel);
	static int n;
	if (n++ < 3) { char b[160]; int k = snprintf(b, sizeof b, "simshim: IFConcreteImage ICRIconLayer -> %p\n", (void*) layer); write(2, b, k); }
	if (layer)
		return layer;
	void* image = ((void* (*)(id, SEL)) objc_msgSend)(self, sel_registerName("CGImage"));
	Class ca = objc_getClass("CALayer");
	if (!image || !ca)
		return nil;
	// off the main thread CA aborts on an implicit transaction: use an explicit background one, as
	// SBHGetIconLayerFromIconServicesImage does
	Class tx = objc_getClass("CATransaction");
	bool bg = !pthread_main_np() && tx;
	if (bg) {
		((void (*)(Class, SEL)) objc_msgSend)(tx, sel_registerName("begin"));
		if (class_respondsToSelector(object_getClass((id) tx), sel_registerName("activateBackground:")))
			((void (*)(Class, SEL, BOOL)) objc_msgSend)(tx, sel_registerName("activateBackground:"), YES);
	}
	layer = ((id (*)(Class, SEL)) objc_msgSend)(ca, sel_registerName("layer"));
	((void (*)(id, SEL, id)) objc_msgSend)(layer, sel_registerName("setContents:"), (id) image);
	double scale = ((double (*)(id, SEL)) objc_msgSend)(self, sel_registerName("scale"));
	if (scale > 0)
		((void (*)(id, SEL, double)) objc_msgSend)(layer, sel_registerName("setContentsScale:"), scale);
	if (bg)
		((void (*)(Class, SEL)) objc_msgSend)(tx, sel_registerName("commit"));
	return layer;
}
static void shim_ifimage_added(const struct mach_header* mh, intptr_t slide)
{
	if (shim_ifimage_calayer_orig)
		return;
	Class c = objc_getClass("IFConcreteImage");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("ICRIconLayer")) : NULL;
	if (m)
		shim_ifimage_calayer_orig = method_setImplementation(m, (IMP) shim_ifimage_calayer);
	if (m)
		write(2, "simshim: hooked IFConcreteImage ICRIconLayer\n", 45);
}
__attribute__((constructor)) static void shim_ifimage_init(void)
{
	_dyld_register_func_for_add_image(shim_ifimage_added);
}

// x86Code: the Metal stand-in (host/metalhostd). MTLSimDriver marks its connections to com.apple.metal.simulator[.<proc>]
// as sim-to-host (xpc_connection_enable_sim2host_4sim: messages packed in the host's macOS XPC format, for the
// Mac-side SimMetalHost). Our service, metalhostd, runs inside the simulator on the simulator's libxpc, which drops
// such messages (the handshake failed with an XPC error), so those connections stay plain simulator connections.
// iOS 27 MTLSimDriver may call enable_sim2host before the connection name is set; a NULL name used to fall through
// and enable sim2host framing, which hangs MTLCreateSystemDefaultDevice against in-sim metalhostd. When
// X86CODE_METAL_IN_SIM is set (mksimdevice with Metal), skip sim2host for unnamed connections too.
#include <xpc/xpc.h>
extern void xpc_connection_enable_sim2host_4sim(xpc_connection_t c);
static void shim_xpc_connection_enable_sim2host_4sim(xpc_connection_t c)
{
	const char* name = xpc_connection_get_name(c);
	static int logged;
	if (getenv("X86CODE_SIMSHIM_XPC_LOG") && logged < 32) {
		logged++;
		fprintf(stderr, "simshim: enable_sim2host name=%s\n", name ? name : "(null)");
	}
	if (name && strncmp(name, "com.apple.metal.simulator", 25) == 0)
		return;
	// x86Code: same for IOSurface's connection: our com.apple.IOSurface.Remote (host/iosurfaced/iosurfaced)
	// is a simulator process too
	if (name && strcmp(name, "com.apple.IOSurface.Remote") == 0)
		return;
	// x86Code: and FileProvider's host service, served in the simulator by host/fpsim2host
	if (name && strcmp(name, "com.apple.FileProvider.sim2host") == 0)
		return;
	// x86Code: iOS 27 may enable_sim2host before set_name (MTLSimDriver, and possibly IOSurface.Remote).
	// Our stand-ins (metalhostd, iosurfaced, fpsim2host) are in-sim and cannot take host framing; a null-name
	// enable hangs the first sync message. Skip all unnamed enables — named host-forwarded services set the
	// name first. X86CODE_METAL_IN_SIM remains set by mksimdevice for documentation / older shims.
	if (!name)
		return;
	xpc_connection_enable_sim2host_4sim(c);
}
DYLD_INTERPOSE(shim_xpc_connection_enable_sim2host_4sim, xpc_connection_enable_sim2host_4sim)

// x86Code: diagnostics, opt-in. Apps get no log file from launchd here; if the Darling file
// /private/var/tmp/x86code-stderr-progs (~/.darling/private/var/tmp/) lists a process name (comma-separated, e.g.
// "Preferences,Photos,Maps"), that process sends stderr, and its os_log (DT mode), to
// /private/var/tmp/x86code-<name>.log. Off by default: DT mode also streams to diagnosticd (see X86CODE_NODT).
static void shim_quiet_os_log(void)
{
	// x86Code: no logd and no diagnosticd here. Processes that launchd starts get OS_ACTIVITY_MODE=disable from
	// cli/libexec/mksimdevice.py, but apps and extensions that SpringBoard/RunningBoard start don't, and their os_log
	// looked up com.apple.diagnosticd for every message (Spotlight and the Maps widget: ~45,000 failed lookups in
	// a few minutes, darlingserver at 90% CPU and launchd_sim at 40%, the UI stalled). libtrace reads the
	// variable at its first log call, after this constructor.
	if (!getenv("OS_ACTIVITY_MODE") && !getenv("OS_ACTIVITY_DT_MODE"))
		setenv("OS_ACTIVITY_MODE", "disable", 1);
}

__attribute__((constructor)) static void shim_app_stderr_init(void)
{
	char buf[1024] = "";
	int lf = open("/private/var/tmp/x86code-stderr-progs", O_RDONLY);
	if (lf < 0) { shim_quiet_os_log(); return; }
	ssize_t got = read(lf, buf, sizeof buf - 1);
	close(lf);
	if (got <= 0) { shim_quiet_os_log(); return; }
	buf[got] = 0;
	for (char* q = buf; *q; q++) if (*q == '\n' || *q == ' ') *q = ',';
	const char* list = buf;
	const char* p = getprogname();
	size_t n = strlen(p);
	for (const char* s = list; s && *s; ) {
		const char* e = strchr(s, ',');
		size_t l = e ? (size_t) (e - s) : strlen(s);
		// x86Code: "name+" also turns on info/debug messages (OS_ACTIVITY_MODE=debug)
		int dbg = l > 0 && s[l - 1] == '+';
		if (dbg) l--;
		if (l == n && !strncmp(s, p, n)) {
			// x86Code: launchd jobs get OS_ACTIVITY_MODE=disable (mksimdevice.py), which also silences DT mode
			if (dbg) setenv("OS_ACTIVITY_MODE", "debug", 1);
			else if (getenv("OS_ACTIVITY_MODE") && !strcmp(getenv("OS_ACTIVITY_MODE"), "disable")) unsetenv("OS_ACTIVITY_MODE");
			char path[256];
			snprintf(path, sizeof path, "/private/var/tmp/x86code-%s.log", p);
			int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
			if (fd >= 0) { dup2(fd, 2); close(fd); setenv("OS_ACTIVITY_DT_MODE", "1", 1); }
			break;
		}
		s = e ? e + 1 : NULL;
	}
	shim_quiet_os_log();
}

