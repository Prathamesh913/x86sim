// x86Code: what LaunchServices answers for the File Provider extension point (fileproviderd's FPDExtensionManager
// enumerates LSExtensionPointRecord "com.apple.fileprovider-nonui"). Run beside a boot with cli/libexec/run-sim.sh, and on a
// Mac with `xcrun simctl spawn`, and compare.
#import <Foundation/Foundation.h>
@interface NSObject (LSX)
- (instancetype)initWithIdentifier:(NSString*)i platform:(unsigned)p parentAppRecord:(id)a error:(NSError**)e;
- (instancetype)initWithBundleIdentifier:(NSString*)b error:(NSError**)e;
+ (id)enumeratorWithExtensionPointRecord:(id)r options:(unsigned long)o;
@end
int main(int argc, char** argv) {
	if (getenv("FPPROBE_SLEEP")) sleep(atoi(getenv("FPPROBE_SLEEP")));
	else sleep(60); // as a launchd job at boot: let lsd come up first
	@autoreleasepool {
		NSString* bid = argc > 2 ? @(argv[2]) : @"com.apple.FileProvider.LocalStorage";
		NSError* e = nil;
		id x = [[NSClassFromString(@"LSApplicationExtensionRecord") alloc] initWithBundleIdentifier:bid error:&e];
		printf("record %s: %s %s\n", bid.UTF8String, x ? [[x description] UTF8String] : "MISSING", e ? e.description.UTF8String : "");
		if (x) printf("  point: %s\n", [[[x valueForKey:@"extensionPointRecord"] description] UTF8String]);
		NSString* pt = argc > 1 ? @(argv[1]) : @"com.apple.fileprovider-nonui";
		Class EP = NSClassFromString(@"LSExtensionPointRecord");
		id ep = nil;
		@try { ep = [x valueForKey:@"extensionPointRecord"]; } @catch (id ex) {}
		printf("point %s: %s\n", pt.UTF8String, [[ep description] UTF8String] ?: "nil");
		Class EN = NSClassFromString(@"LSApplicationExtensionRecord");
		if (ep) {
			id en = [EN enumeratorWithExtensionPointRecord:ep options:0];
			int n = 0;
			for (id r in en) { printf("  ext %s\n", [[r valueForKey:@"bundleIdentifier"] UTF8String]); n++; }
			printf("  %d extensions\n", n);
		}
		(void) EP;
	}
	return 0;
}
