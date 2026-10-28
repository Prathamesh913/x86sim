// Spike 3.3 probe: IOKit and IOSurface from a simulator process
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <stdio.h>
extern kern_return_t host_get_io_master(mach_port_t host, mach_port_t* io_master);
int main(void)
{
	mach_port_t master = MACH_PORT_NULL;
	kern_return_t kr = host_get_io_master(mach_host_self(), &master);
	printf("host_get_io_master kr=0x%x port=0x%x\n", kr, master);
	io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOSurfaceRoot"));
	printf("IOSurfaceRoot service=0x%x\n", s);
	int w = 64, h = 64, bpe = 4;
	CFMutableDictionaryRef d = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef nw = CFNumberCreate(NULL, kCFNumberIntType, &w), nh = CFNumberCreate(NULL, kCFNumberIntType, &h), nb = CFNumberCreate(NULL, kCFNumberIntType, &bpe);
	CFDictionarySetValue(d, kIOSurfaceWidth, nw); CFDictionarySetValue(d, kIOSurfaceHeight, nh); CFDictionarySetValue(d, kIOSurfaceBytesPerElement, nb);
	IOSurfaceRef surf = IOSurfaceCreate(d);
	printf("IOSurfaceCreate -> %p id=%u base=%p\n", surf, surf ? IOSurfaceGetID(surf) : 0, surf ? IOSurfaceGetBaseAddress(surf) : NULL);
	fflush(stdout);
	return 0;
}
