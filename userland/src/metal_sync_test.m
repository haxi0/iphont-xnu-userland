/*
 * metal_sync_test: fences, events, shared events and heaps of the system's Metal device
 * (src/hkmetal.m), through Metal's public API only, as an application would use them. Boot with
 * metal=1 (and metal_props on) or metal=2. Every check is made with blits on the GPU and each one
 * would come out differently if the thing it checks did nothing:
 *
 *   fence      a fill, updateFence; then, in another encoder, waitForFence and a copy of it
 *   order      a buffer that WAITS for an event is committed BEFORE the buffer that signals it,
 *              and both fill the same memory: the waiter's fill must be the one left
 *   cpu        a buffer waiting for a shared event stays undone until the processor signals it
 *   visible    a buffer's signal reaches signaledValue and a listener once it is complete
 *   heap       sizes the device answers are what the heap counts; a buffer that does not fit is
 *              refused; given back on release and on makeAliasable; a heap buffer works in a blit
 *
 * Prints one line per check, "PASS" or "FAIL", and exits 1 if any failed.
 */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static int failures;

static void
check(const char *name, BOOL pass, const char *what)
{
	printf("metal_sync_test: %-8s %s: %s\n", name, pass ? "PASS" : "FAIL", what);
	failures += !pass;
}

static BOOL
all(id<MTLBuffer> buffer, uint8_t value)
{
	const uint8_t *bytes = [buffer contents];

	for (NSUInteger i = 0; i < [buffer length]; i++) {
		if (bytes[i] != value) {
			return NO;
		}
	}
	return YES;
}

static id<MTLCommandBuffer>
fill(id<MTLCommandQueue> queue, id<MTLBuffer> buffer, uint8_t value)
{
	id<MTLCommandBuffer> cb = [queue commandBuffer];
	id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];

	[blit fillBuffer:buffer range:NSMakeRange(0, [buffer length]) value:value];
	[blit endEncoding];
	return cb;
}

int
main(void)
{
	@autoreleasepool {
		id<MTLDevice> device = MTLCreateSystemDefaultDevice();
		id<MTLCommandQueue> queue = [device newCommandQueue];
		const NSUInteger n = 4096;

		printf("metal_sync_test: device %s\n", device ? [[device name] UTF8String] : "none");
		if (device == nil || queue == nil) {
			return 1;
		}

		/* fence */
		id<MTLFence> fence = [device newFence];
		id<MTLBuffer> a = [device newBufferWithLength:n options:0], b = [device newBufferWithLength:n options:0];
		id<MTLCommandBuffer> cb = [queue commandBuffer];
		id<MTLBlitCommandEncoder> blit = [cb blitCommandEncoder];

		[blit fillBuffer:a range:NSMakeRange(0, n) value:0x5a];
		[blit updateFence:fence];
		[blit endEncoding];
		blit = [cb blitCommandEncoder];
		[blit waitForFence:fence];
		[blit copyFromBuffer:a sourceOffset:0 toBuffer:b destinationOffset:0 size:n];
		[blit endEncoding];
		[cb commit];
		[cb waitUntilCompleted];
		check("fence", fence != nil && all(b, 0x5a), "a fence exists, and the copy after its wait saw the fill");

		/* order: the waiter is committed first */
		id<MTLEvent> event = [device newEvent];
		id<MTLBuffer> x = [device newBufferWithLength:n options:0];
		id<MTLCommandBuffer> waiter = [queue commandBuffer];

		[waiter encodeWaitForEvent:event value:1];
		blit = [waiter blitCommandEncoder];
		[blit fillBuffer:x range:NSMakeRange(0, n) value:0x22];
		[blit endEncoding];
		[waiter commit];
		id<MTLCommandBuffer> signaller = fill(queue, x, 0x11);
		[signaller encodeSignalEvent:event value:1];
		[signaller commit];
		[waiter waitUntilCompleted];
		check("order", event != nil && all(x, 0x22) && [waiter status] == MTLCommandBufferStatusCompleted,
		    "the buffer waiting for the event ran after the one that signals it");

		/* cpu */
		id<MTLSharedEvent> shared = [device newSharedEvent];
		id<MTLBuffer> y = [device newBufferWithLength:n options:0];
		id<MTLCommandBuffer> held = [queue commandBuffer];

		[held encodeWaitForEvent:shared value:5];
		blit = [held blitCommandEncoder];
		[blit fillBuffer:y range:NSMakeRange(0, n) value:0x33];
		[blit endEncoding];
		[held commit];
		usleep(100000);
		BOOL stayed = [held status] < MTLCommandBufferStatusCompleted && all(y, 0);
		[shared setSignaledValue:5];
		[held waitUntilCompleted];
		check("cpu", shared != nil && stayed && all(y, 0x33),
		    "held until the processor signalled the shared event, then done");

		/* visible */
		MTLSharedEventListener *listener = [[MTLSharedEventListener alloc] init];
		__block uint64_t heard = 0;
		dispatch_semaphore_t told = dispatch_semaphore_create(0);

		[shared notifyListener:listener atValue:7 block:^(id<MTLSharedEvent> e, uint64_t value) {
			(void)e;
			heard = value;
			dispatch_semaphore_signal(told);
		}];
		cb = fill(queue, y, 0x44);
		[cb encodeSignalEvent:shared value:7];
		[cb commit];
		[cb waitUntilCompleted];
		BOOL in_time = dispatch_semaphore_wait(told, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC)) == 0;
		check("visible", [shared signaledValue] == 7 && in_time && heard >= 7 && all(y, 0x44),
		    "a buffer's signal reached signaledValue and the listener once it was complete");

		/* heap */
		MTLHeapDescriptor *hd = [[MTLHeapDescriptor alloc] init];

		hd.size = 1 << 20;
		id<MTLHeap> heap = [device newHeapWithDescriptor:hd];
		MTLSizeAndAlign sa = [device heapBufferSizeAndAlignWithLength:100000 options:0];
		@autoreleasepool {
			id<MTLBuffer> h = [heap newBufferWithLength:100000 options:0];
			id<MTLBuffer> big = [heap newBufferWithLength:(1 << 20) options:0];

			check("heap", heap != nil && h != nil && [h heap] == heap && [heap usedSize] == sa.size && big == nil,
			    "a heap buffer counts what the device said it would, and one that does not fit is refused");
			cb = fill(queue, h, 0x66);
			[cb commit];
			[cb waitUntilCompleted];
			check("heap", ((const uint8_t *)[h contents])[99999] == 0x66, "a heap buffer works in a blit");
			[h makeAliasable];
			check("heap", [h isAliasable] && [heap usedSize] == 0, "makeAliasable gave its size back");
			h = nil;
			big = nil;
		}
		MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
		    width:64 height:64 mipmapped:NO];
		MTLSizeAndAlign ta = [device heapTextureSizeAndAlignWithDescriptor:td];
		@autoreleasepool {
			id<MTLTexture> t = [heap newTextureWithDescriptor:td];

			check("heap", t != nil && [heap usedSize] == ta.size, "a heap texture counts what the device said");
			t = nil;
		}
		check("heap", [heap usedSize] == 0, "released resources gave their sizes back");

		printf("metal_sync_test: %s\n", failures ? "FAILED" : "all passed");
	}
	return failures != 0;
}
