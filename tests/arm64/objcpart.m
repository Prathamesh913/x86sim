// Stage 0 test (c), ObjC half: objc_msgSend dispatch on NSObject subclasses (libobjc only, no Foundation).
#import <objc/NSObject.h>
#include <stdint.h>
#include <mach/mach_time.h>
@interface Acc : NSObject { @public uint64_t v; }
- (void)add:(uint64_t)x;
@end
@implementation Acc
- (void)add:(uint64_t)x { v += x ^ (v >> 3); }
@end
@interface Acc2 : Acc
@end
@implementation Acc2
- (void)add:(uint64_t)x { v += x * 3; }
@end
uint64_t x86c_now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
uint64_t x86c_objc_msgsend(uint32_t n) {
    Acc *a = [Acc new], *b = [Acc2 new];
    for (uint32_t i = 0; i < n; i++) [(i & 1 ? a : b) add:i];
    uint64_t r = a->v + b->v;
    [a release]; [b release];
    return r;
}
uint64_t x86c_objc_alloc(uint32_t n) {
    uint64_t r = 0;
    for (uint32_t i = 0; i < n; i++) { Acc *a = [Acc new]; [a add:i]; r += a->v; [a release]; }
    return r;
}
