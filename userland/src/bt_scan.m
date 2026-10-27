/* Native CoreBluetooth discovery probe. No entitlements or permission overrides. */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static NSString *const serviceUUID = @"86DBFDC4-50F4-4D5C-BD34-9672E9EF58F2";
static NSString *const valueUUID = @"86DBFDC4-50F4-4D5C-BD34-9672E9EF58F3";

@interface HKBTScan : NSObject
@property(nonatomic, strong) id central;
@property(nonatomic) NSUInteger reports;
@property(nonatomic) BOOL scanning;
@property(nonatomic) BOOL finished;
@property(nonatomic) BOOL unauthorized;
@property(nonatomic) BOOL link;
@property(nonatomic) BOOL all;
@property(nonatomic) BOOL connected;
@property(nonatomic) BOOL passed;
@property(nonatomic, strong) id peripheral;
@property(nonatomic, strong) NSData *payload;
@end

@interface NSObject (HKBTProbe)
- (instancetype)initWithDelegate:(id)delegate queue:(id)queue options:(NSDictionary *)options;
- (NSInteger)state;
- (void)scanForPeripheralsWithServices:(NSArray *)services options:(NSDictionary *)options;
- (void)stopScan;
+ (instancetype)UUIDWithString:(NSString *)string;
- (void)connectPeripheral:(id)peripheral options:(NSDictionary *)options;
- (void)cancelPeripheralConnection:(id)peripheral;
- (void)setDelegate:(id)delegate;
- (void)discoverServices:(NSArray *)services;
- (NSArray *)services;
- (id)UUID;
- (void)discoverCharacteristics:(NSArray *)characteristics forService:(id)service;
- (NSArray *)characteristics;
- (void)readValueForCharacteristic:(id)characteristic;
- (void)writeValue:(NSData *)value forCharacteristic:(id)characteristic type:(NSInteger)type;
- (NSData *)value;
@end

@implementation HKBTScan
- (void)centralManagerDidUpdateState:(id)central
{
	NSInteger state = [central state];
	printf("bt-scan: CoreBluetooth state %ld\n", (long)state);
	if (state == 3) {
		self.unauthorized = YES;
		self.finished = YES;
	} else if (state == 5 && !self.scanning) {
		self.scanning = YES;
		Class uuid = NSClassFromString(@"CBUUID");
		[central scanForPeripheralsWithServices:self.all ? nil : @[[uuid UUIDWithString:serviceUUID]] options:nil];
		dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
			[central stopScan];
			if (!self.link) self.finished = YES;
		});
	}
}
- (void)centralManager:(id)central didDiscoverPeripheral:(id)peripheral advertisementData:(NSDictionary *)data RSSI:(NSNumber *)rssi
{
	(void)data; (void)rssi;
	self.reports++;
	if (self.link && !self.peripheral) {
		self.peripheral = peripheral;
		[central stopScan];
		[peripheral setDelegate:self];
		[central connectPeripheral:peripheral options:nil];
	}
}
- (void)centralManager:(id)central didConnectPeripheral:(id)peripheral
{
	(void)central;
	self.connected = YES;
	printf("bt-scan: test peer connected; discovering GATT service\n");
	[peripheral discoverServices:@[[NSClassFromString(@"CBUUID") UUIDWithString:serviceUUID]]];
}
- (void)centralManager:(id)central didFailToConnectPeripheral:(id)peripheral error:(NSError *)error
{
	(void)central; (void)peripheral;
	fprintf(stderr, "bt-scan: connect failed: %s\n", error.description.UTF8String);
	self.finished = YES;
}
- (void)peripheral:(id)peripheral didDiscoverServices:(NSError *)error
{
	if (!error) for (id service in [peripheral services]) {
		if ([[service UUID] isEqual:[NSClassFromString(@"CBUUID") UUIDWithString:serviceUUID]]) {
			[peripheral discoverCharacteristics:@[[NSClassFromString(@"CBUUID") UUIDWithString:valueUUID]] forService:service];
			return;
		}
	}
	fprintf(stderr, "bt-scan: test service discovery failed\n");
	self.finished = YES;
}
- (void)peripheral:(id)peripheral didDiscoverCharacteristicsForService:(id)service error:(NSError *)error
{
	if (!error) for (id characteristic in [service characteristics]) {
		if ([[characteristic UUID] isEqual:[NSClassFromString(@"CBUUID") UUIDWithString:valueUUID]]) {
			[peripheral readValueForCharacteristic:characteristic];
			return;
		}
	}
	fprintf(stderr, "bt-scan: test characteristic discovery failed\n");
	self.finished = YES;
}
- (void)peripheral:(id)peripheral didUpdateValueForCharacteristic:(id)characteristic error:(NSError *)error
{
	if (error || ![[characteristic value] length]) {
		fprintf(stderr, "bt-scan: GATT read failed: %s\n", error ? error.description.UTF8String : "empty value");
		self.finished = YES;
	} else if (!self.payload) {
		self.payload = [[NSString stringWithFormat:@"HKBT-%d-%llu", getpid(),
		    (unsigned long long)(NSDate.date.timeIntervalSince1970 * 1000)] dataUsingEncoding:NSUTF8StringEncoding];
		[peripheral writeValue:self.payload forCharacteristic:characteristic type:0];
	} else {
		self.passed = [self.payload isEqualToData:[characteristic value]];
		self.finished = YES;
	}
}
- (void)peripheral:(id)peripheral didWriteValueForCharacteristic:(id)characteristic error:(NSError *)error
{
	if (error) {
		fprintf(stderr, "bt-scan: GATT write failed: %s\n", error.description.UTF8String);
		self.finished = YES;
	} else {
		[peripheral readValueForCharacteristic:characteristic];
	}
}
@end

int main(int argc, char **argv)
{
	@autoreleasepool {
		setbuf(stdout, NULL);
		void *framework = dlopen("/System/Library/Frameworks/CoreBluetooth.framework/CoreBluetooth", RTLD_NOW);
		Class centralClass = NSClassFromString(@"CBCentralManager");
		if (!framework || !centralClass) {
			fprintf(stderr, "bt-scan: CoreBluetooth unavailable\n");
			return 1;
		}
		HKBTScan *probe = [HKBTScan new];
		if (argc > 2 || (argc == 2 && strcmp(argv[1], "--link") != 0 && strcmp(argv[1], "--all") != 0)) {
			fprintf(stderr, "usage: bt_scan [--link|--all]\n");
			return 2;
		}
		probe.link = argc == 2 && strcmp(argv[1], "--link") == 0;
		probe.all = argc == 2 && strcmp(argv[1], "--all") == 0;
		probe.central = [[centralClass alloc] initWithDelegate:probe queue:nil options:nil];
		NSDate *deadline = [NSDate dateWithTimeIntervalSinceNow:25];
		while (!probe.finished && [deadline timeIntervalSinceNow] > 0) {
			[[NSRunLoop mainRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
		}
		if (probe.link) {
			printf("bt-scan: native GATT %s; connected %d; roundtrip %lu bytes; probe-pid %d\n",
			    probe.passed ? "PASS" : "FAIL", probe.connected, (unsigned long)probe.payload.length, getpid());
			if (probe.peripheral) [probe.central cancelPeripheralConnection:probe.peripheral];
		} else printf("bt-scan: native discovery %s; reports %lu; probe-pid %d; filter %s%s\n", probe.scanning && probe.reports ? "PASS" : "FAIL",
		    (unsigned long)probe.reports, getpid(), probe.all ? "all" : "test-peer", probe.unauthorized ? "; Bluetooth permission required" : "");
		[probe.central stopScan];
		return (probe.link ? probe.passed : probe.scanning && probe.reports) ? 0 : 1;
	}
}
