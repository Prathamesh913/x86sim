// x86Code: does LaunchServices know widget extensions? (chronod: "Extension not found")
#import <Foundation/Foundation.h>
#import <CoreServices/CoreServices.h>
extern int dyld_get_active_platform(void);
@interface LSApplicationExtensionRecord : NSObject
- (instancetype)initWithBundleIdentifier:(NSString*)b error:(NSError**)e;
+ (id)enumeratorWithExtensionPointRecord:(id)r options:(NSUInteger)o;
@property (readonly) NSString* bundleIdentifier;
@end
@interface LSApplicationRecord : NSObject
- (instancetype)initWithBundleIdentifier:(NSString*)b allowPlaceholder:(BOOL)p error:(NSError**)e;
@property (readonly) NSArray* applicationExtensionRecords;
@property (readonly) NSURL* URL;
@end
int main(void) {
	if (!getenv("NOSLEEP")) sleep(25);
	printf("active platform %d\n", dyld_get_active_platform());
	@autoreleasepool {
		for (NSString* b in @[@"com.apple.news.widget", @"com.apple.Maps.GeneralMapsWidget", @"com.apple.mobilecal.CalendarWidgetExtension"]) {
			NSError* e = nil;
			id r = [[NSClassFromString(@"LSApplicationExtensionRecord") alloc] initWithBundleIdentifier:b error:&e];
			printf("%s: %s %s\n", b.UTF8String, [[r description] UTF8String] ?: "nil", e ? e.description.UTF8String : "");
		}
		for (NSString* b in @[@"com.apple.news", @"com.apple.Maps"]) {
			NSError* e = nil;
			LSApplicationRecord* a = [[NSClassFromString(@"LSApplicationRecord") alloc] initWithBundleIdentifier:b allowPlaceholder:NO error:&e];
			{ id pl = nil; @try { pl = [a valueForKey:@"platform"]; } @catch (id e) {} id sdk = nil; @try { sdk = [a valueForKey:@"SDKVersion"]; } @catch (id e) {} printf("app platform=%s sdk=%s exec=%s\n", [[pl description] UTF8String] ?: "?", [[sdk description] UTF8String] ?: "?", [[[a valueForKey:@"executableURL"] description] UTF8String] ?: "?"); }
			printf("%s: %s exts %lu %s\n", b.UTF8String, a.URL.path.UTF8String ?: "nil", (unsigned long)a.applicationExtensionRecords.count, e ? e.description.UTF8String : "");
			for (id x in a.applicationExtensionRecords) { id pl = nil, pi = nil; @try { pl = [x valueForKey:@"platform"]; } @catch (id e) {} @try { pi = [x valueForKey:@"executableURL"]; } @catch (id e) {}
				printf("  %s platform=%s ep=%s\n", [[x valueForKey:@"bundleIdentifier"] UTF8String], [[pl description] UTF8String] ?: "?", [[pi description] UTF8String] ?: "?"); }
		}
	}
	return 0;
}
