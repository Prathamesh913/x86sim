// x86Code: opens URLs in the simulator for tests (as `xcrun simctl openurl`). Run as a job:
//   openurl=host/openurl/openurl
// then write a URL (one per line) to <device>/data/tmp/x86code-openurl (the file is taken and removed);
// cli/libexec/simtouch.py openurl <url> does that. Log: <device>/data/var/log/x86code-openurl.log. (Not the prefix's
// /private/var/tmp: a file written there from Linux, into the overlay's upper directory, is not seen inside.)
//
// The app is asked to open the URL through FrontBoard (FBSOpenApplicationService, SpringBoard's open-application
// service, with the URL as the payload), which is what lsd itself does once it has picked the app. Going through
// lsd (LSApplicationWorkspace openURL:) made lsd abort: for a request from another process lsd passes the caller's
// RBSProcessHandle as clientHandle, and it had none for a launchd job (FINDINGS 3.10/3.11). A line starting with
// "ls " still takes that route. "app <bundle id>" opens an app without a URL (apps the home screen doesn't show). The app for a URL is the first LaunchServices claims for its scheme (http/https:
// Safari). SpringBoard checks com.apple.frontboard.launchapplications: the binary carries it in
// __TEXT,__entitlements (openurl.entitlements), which the simulator reads instead of a code signature.
#import <Foundation/Foundation.h>
#include <objc/message.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>

#define MSG(ret, ...) ((ret(*)(__VA_ARGS__))objc_msgSend)

static FILE* logf_;

static NSString* appForURL(NSURL* url) {
	id w = MSG(id, id, SEL)(NSClassFromString(@"LSApplicationWorkspace"), sel_getUid("defaultWorkspace"));
	NSArray* apps = MSG(id, id, SEL, id)(w, sel_getUid("applicationsAvailableForHandlingURLScheme:"), url.scheme);
	for (id proxy in apps) {
		NSString* bid = MSG(id, id, SEL)(proxy, sel_getUid("bundleIdentifier"));
		if (bid.length) return bid;
	}
	if ([url.scheme hasPrefix:@"http"]) return @"com.apple.mobilesafari";
	return nil;
}

static void openViaFrontBoard(NSURL* url, NSString* u, NSString* app) {
	NSString* bid = app ?: appForURL(url);
	if (!bid) { fprintf(logf_, "openurl %s: no app for the scheme\n", u.UTF8String); fflush(logf_); return; }
	Class svcClass = NSClassFromString(@"FBSOpenApplicationService");
	Class optClass = NSClassFromString(@"FBSOpenApplicationOptions");
	id svc = MSG(id, id, SEL)(svcClass, sel_getUid("serviceWithDefaultShellEndpoint"));
	// FBSOpenApplicationOptionKeyPayloadURL, ...KeyUnlockDevice, ...KeyActivateSuspended
	NSMutableDictionary* d = [@{ @"__UnlockDevice": @YES, @"__ActivateSuspended": @NO } mutableCopy];
	if (url) d[@"__PayloadURL"] = url;
	id opts = MSG(id, id, SEL, id)(optClass, sel_getUid("optionsWithDictionary:"), d);
	dispatch_semaphore_t done = dispatch_semaphore_create(0);
	__block NSString* result = @"(no reply in 30 s)";
	void (^completion)(id, NSError*) = ^(id process, NSError* err) {
		result = err ? [NSString stringWithFormat:@"error %@", err] : [NSString stringWithFormat:@"ok %@", process];
		dispatch_semaphore_signal(done);
	};
	MSG(void, id, SEL, id, id, id)(svc, sel_getUid("openApplication:withOptions:completion:"), bid, opts, completion);
	dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 30 * NSEC_PER_SEC));
	fprintf(logf_, "openurl %s -> %s: %s\n", u.UTF8String, bid.UTF8String, result.UTF8String); fflush(logf_);
}

static void openViaLS(NSURL* url, NSString* u) {
	id w = MSG(id, id, SEL)(NSClassFromString(@"LSApplicationWorkspace"), sel_getUid("defaultWorkspace"));
	NSError* err = nil;
	BOOL ok = MSG(BOOL, id, SEL, id, id, NSError**)(w, sel_getUid("openURL:withOptions:error:"), url, @{}, &err);
	fprintf(logf_, "openurl (ls) %s: %d %s\n", u.UTF8String, ok, err.description.UTF8String ?: ""); fflush(logf_);
}

int main(void) {
	@autoreleasepool {
		NSString* home = @(getenv("HOME") ?: "/private/var/tmp");
		logf_ = fopen([home stringByAppendingString:@"/var/log/x86code-openurl.log"].UTF8String, "a");
		dlopen("/System/Library/PrivateFrameworks/FrontBoardServices.framework/FrontBoardServices", RTLD_NOW);
		dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_NOW);
		fprintf(logf_, "openurl: ready\n"); fflush(logf_);
		const char* path = [home stringByAppendingString:@"/tmp/x86code-openurl"].UTF8String;
		for (;;) {
			@autoreleasepool {
				usleep(300000);
				NSString* s = [NSString stringWithContentsOfFile:@(path) encoding:NSUTF8StringEncoding error:nil];
				if (!s) continue;
				unlink(path);
				for (NSString* line in [s componentsSeparatedByString:@"\n"]) {
					NSString* u = [line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
					if ([u hasPrefix:@"app "]) {
						NSString* bid = [[u substringFromIndex:4] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
						openViaFrontBoard(nil, bid, bid);
						continue;
					}
					BOOL ls = [u hasPrefix:@"ls "];
					if (ls) u = [[u substringFromIndex:3] stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceCharacterSet];
					if (!u.length) continue;
					NSURL* url = [NSURL URLWithString:u];
					if (!url) { fprintf(logf_, "openurl %s: not a URL\n", u.UTF8String); fflush(logf_); continue; }
					if (ls) openViaLS(url, u); else openViaFrontBoard(url, u, nil);
				}
			}
		}
	}
}
