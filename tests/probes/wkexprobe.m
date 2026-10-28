// x86Code: what LaunchServices says about WebKit's extensions and Safari's entitlements (run here and on a Mac's
// simulator with `xcrun simctl spawn`, compare): ExtensionFoundation's launch check reads these.
#import <Foundation/Foundation.h>
@interface LSRecordX : NSObject
- (instancetype)initWithBundleIdentifier:(NSString*)b allowPlaceholder:(BOOL)p error:(NSError**)e;
- (instancetype)initWithBundleIdentifier:(NSString*)b error:(NSError**)e;
@end
static void dump(const char* what, id o) { printf("%s: %s\n", what, [[o description] UTF8String] ?: "nil"); }
int main(int argc, char** argv) {
	if (getenv("WKEX_SLEEP")) sleep(atoi(getenv("WKEX_SLEEP")));
	@autoreleasepool {
		NSError* e = nil;
		id app = [[NSClassFromString(@"LSApplicationRecord") alloc] initWithBundleIdentifier:@"com.apple.mobilesafari" allowPlaceholder:NO error:&e];
		dump("safari record", app); if (e) dump("error", e);
		id ents = [app valueForKey:@"entitlements"];
		for (NSString* k in @[@"com.apple.developer.web-browser-engine.host", @"com.apple.private.extensionkit.host-requirement-exemption",
				@"get-task-allow", @"com.apple.developer.web-browser", @"application-identifier"])
			printf("  ent %s = %s\n", k.UTF8String, [[[ents performSelector:@selector(objectForKey:ofClass:) withObject:k withObject:[NSObject class]] description] UTF8String] ?: "nil");
		for (NSString* b in @[@"com.apple.WebKit.WebContent", @"com.apple.WebKit.Networking", @"com.apple.WebKit.GPU"]) {
			e = nil;
			id x = [[NSClassFromString(@"LSApplicationExtensionRecord") alloc] initWithBundleIdentifier:b error:&e];
			printf("== %s: %s %s\n", b.UTF8String, x ? "found" : "MISSING", e ? e.description.UTF8String : "");
			if (!x) continue;
			id ep = [x valueForKey:@"extensionPointRecord"];
			dump("  extensionPoint", [ep valueForKey:@"identifier"]);
			@try { dump("  extensionPointType", [ep valueForKey:@"extensionPointType"]); } @catch (id ex) {}
			dump("  SDKDictionary", [ep valueForKey:@"SDKDictionary"]);
			@try { dump("  parentAppRecord", [ep valueForKey:@"parentAppRecord"]); } @catch (id ex) {}
			@try { dump("  containingBundleRecord", [x valueForKey:@"containingBundleRecord"]); } @catch (id ex) {}
			id xe = [x valueForKey:@"entitlements"];
			dump("  ext entitlement host-requirement-exemption", [xe performSelector:@selector(objectForKey:ofClass:) withObject:@"com.apple.private.extensionkit.host-requirement-exemption" withObject:[NSObject class]]);
			@try { dump("  infoDictionary EXAppExtensionAttributes", [[x valueForKey:@"infoDictionary"] performSelector:@selector(objectForKey:ofClass:) withObject:@"EXAppExtensionAttributes" withObject:[NSObject class]]); } @catch (id ex) {}
		}
	}
	return 0;
}
