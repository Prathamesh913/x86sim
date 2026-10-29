// Stage 2 (arm64 plan): binary property list round trip (lsd crashed in __CFBinaryPlistCreateObject)
#import <Foundation/Foundation.h>
int main(int argc, char **argv) {
    @autoreleasepool {
        NSMutableDictionary *d = [NSMutableDictionary dictionary];
        for (int i = 0; i < 3000; i++)
            d[[NSString stringWithFormat:@"key-%d-%@", i, i % 7 ? @"x" : @"üñî"]] = @[@(i), @(i * 1.5), [NSDate dateWithTimeIntervalSince1970:i],
                [@"data" dataUsingEncoding:NSUTF8StringEncoding], @{@"n": @(-i), @"b": @(i % 2 == 0)}, @(1ull << (i % 64))];
        NSError *e = nil;
        NSData *bin = [NSPropertyListSerialization dataWithPropertyList:d format:NSPropertyListBinaryFormat_v1_0 options:0 error:&e];
        id back = [NSPropertyListSerialization propertyListWithData:bin options:0 format:NULL error:&e];
        fprintf(stderr, "binary %lu bytes, round trip %s (%s)\n", (unsigned long)bin.length, [back isEqual:d] ? "equal" : "DIFFERENT", e ? e.description.UTF8String : "no error");
        // argv[1]: a file listing plist paths, one per line
        if (argc > 1) {
            NSString *list = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:nil];
            int n = 0, bad = 0;
            for (NSString *path in [list componentsSeparatedByString:@"\n"]) {
                if (!path.length) continue;
                fprintf(stderr, "> %s\n", path.UTF8String);
                NSData *f = [NSData dataWithContentsOfFile:path];
                id p = f ? [NSPropertyListSerialization propertyListWithData:f options:0 format:NULL error:&e] : nil;
                n++; if (!p) { bad++; fprintf(stderr, "FAILED %s\n", path.UTF8String); }
            }
            fprintf(stderr, "files %d, failed %d\n", n, bad);
        }
    }
    return 0;
}
