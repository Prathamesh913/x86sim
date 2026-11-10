// Stage 2 (arm64 plan): CoreFoundation/Foundation basics that daemons need at start (main bundle, preferences,
// bundle for a class, file manager).
#import <Foundation/Foundation.h>
#include <mach-o/dyld.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        char p[1024]; uint32_t n = sizeof p;
        _NSGetExecutablePath(p, &n);
        printf("exe %s\n", p);
        CFBundleRef mb = CFBundleGetMainBundle();
        printf("main bundle %p\n", mb);
        if (mb) { CFURLRef u = CFBundleCopyBundleURL(mb); NSLog(@"bundle url %@", u); }
        NSLog(@"bundleForClass %@", [NSBundle bundleForClass:[NSString class]].bundlePath);
        NSLog(@"home %@ tmp %@", NSHomeDirectory(), NSTemporaryDirectory());
        NSUserDefaults *d = [[NSUserDefaults alloc] initWithSuiteName:@"com.x86sim.cfprobe"];
        [d setObject:@"v" forKey:@"k"];
        NSLog(@"defaults %@", [d objectForKey:@"k"]);
        NSLog(@"ls / %lu", (unsigned long)[[NSFileManager defaultManager] contentsOfDirectoryAtPath:@"/" error:nil].count);
    }
    return 0;
}
