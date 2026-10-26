/* Root-only /dev/iphont bridge. Enabled only by iphont=1 iphont_control=1 in a development boot. */
#import <Foundation/Foundation.h>
#include <IOKit/IOKitLib.h>
#include <ifaddrs.h>
#include <netdb.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/param.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);
#include <time.h>
#include <spawn.h>
#include <signal.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dlfcn.h>
#include <objc/message.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
extern char **environ;
struct reply { uint32_t version, sequence; int32_t error; uint32_t length; uint64_t device_ns; };
static NSDictionary *failure_domain(NSString *message, int code, NSString *domain)
{
    return @{ @"ok": @NO, @"error": message ?: @"failed", @"code": @(code), @"domain": domain };
}
static NSDictionary *failure(NSString *message, int code)
{
    return @{ @"ok": @NO, @"error": message ?: @"failed", @"code": @(code), @"errno": @(code), @"domain": NSPOSIXErrorDomain };
}
static NSString *text(NSData *d) { return [[NSString alloc] initWithData:d encoding:NSUTF8StringEncoding] ?: @"<non-UTF8 output; see base64>"; }

static double monotonic_now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static BOOL valid_request(id value, unsigned depth)
{
    if (depth > 16) return NO;
    if ([value isKindOfClass:[NSString class]]) {
        const char *bytes = [value UTF8String];
        return bytes && strlen(bytes) == [value lengthOfBytesUsingEncoding:NSUTF8StringEncoding];
    }
    if ([value isKindOfClass:[NSArray class]]) {
        for (id item in value) if (!valid_request(item, depth + 1)) return NO;
    } else if ([value isKindOfClass:[NSDictionary class]]) {
        for (id key in value) if (!valid_request(key, depth + 1) || !valid_request(value[key], depth + 1)) return NO;
    }
    return YES;
}
static NSDictionary *execute(NSDictionary *request)
{
    NSArray *args = request[@"argv"];
    if (![args isKindOfClass:[NSArray class]] || !args.count || args.count > 64) return failure(@"argv must contain 1..64 strings", EINVAL);
    char *argv[65] = {0};
    for (NSUInteger i = 0; i < args.count; i++) {
        if (![args[i] isKindOfClass:[NSString class]]) return failure(@"invalid argument", EINVAL);
        argv[i] = (char *)[args[i] UTF8String];
    }
    if (argv[0][0] != '/') return failure(@"executable must use an absolute path", EINVAL);
    double timeout = request[@"timeout"] ? [request[@"timeout"] doubleValue] : 10;
    if (timeout <= 0 || timeout > 15) return failure(@"timeout must be in (0,15] seconds", EINVAL);
    int pipefd[2];
    if (pipe(pipefd)) return failure(@"pipe failed", errno);
    fcntl(pipefd[0], F_SETFD, FD_CLOEXEC); fcntl(pipefd[1], F_SETFD, FD_CLOEXEC);
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    posix_spawn_file_actions_addclose(&actions, pipefd[1]);
    posix_spawnattr_t attrs;
    posix_spawnattr_init(&attrs); posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&attrs, 0);
    pid_t pid;
    int error = posix_spawn(&pid, argv[0], &actions, &attrs, argv, environ);
    posix_spawnattr_destroy(&attrs); posix_spawn_file_actions_destroy(&actions); close(pipefd[1]);
    if (error) { close(pipefd[0]); return failure(@"posix_spawn failed", error); }
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    NSMutableData *output = [NSMutableData data];
    NSTimeInterval deadline = monotonic_now() + timeout;
    BOOL timedOut = NO, truncated = NO;
    int status = 0;
    for (;;) {
        char buf[2048]; ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (output.length + n <= 24000) [output appendBytes:buf length:n]; else truncated = YES;
        }
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
                if (output.length + n <= 24000) [output appendBytes:buf length:n]; else truncated = YES;
            }
            break;
        }
        if (done < 0) { close(pipefd[0]); return failure(@"waitpid failed", errno); }
        if (monotonic_now() >= deadline) { timedOut = YES; kill(-pid, SIGKILL); waitpid(pid, &status, 0); break; }
        usleep(10000);
    }
    close(pipefd[0]);
    return @{ @"ok": @(!timedOut && WIFEXITED(status) && WEXITSTATUS(status) == 0), @"pid": @(pid),
        @"exit_code": WIFEXITED(status) ? @(WEXITSTATUS(status)) : [NSNull null],
        @"signal": WIFSIGNALED(status) ? @(WTERMSIG(status)) : [NSNull null], @"timed_out": @(timedOut),
        @"truncated": @(truncated), @"output": text(output), @"base64": [output base64EncodedStringWithOptions:0] };
}
static id property_value(id obj, unsigned depth)
{
    if (depth > 8) return @{ @"truncated": @YES, @"reason": @"property nesting exceeds 8" };
    if ([obj isKindOfClass:[NSString class]] || [obj isKindOfClass:[NSNumber class]]) return obj;
    if ([obj isKindOfClass:[NSData class]]) return @{ @"base64": [obj base64EncodedStringWithOptions:0] };
    if ([obj isKindOfClass:[NSArray class]]) {
        NSMutableArray *items = [NSMutableArray array];
        for (id item in obj) [items addObject:property_value(item, depth + 1)];
        return items;
    }
    if ([obj isKindOfClass:[NSDictionary class]]) {
        NSMutableDictionary *items = [NSMutableDictionary dictionary];
        for (id key in obj) items[[key description]] = property_value(obj[key], depth + 1);
        return items;
    }
    return [obj description] ?: [NSNull null];
}
static NSDictionary *handle(NSDictionary *r)
{
    NSString *op = r[@"op"];
    NSFileManager *fm = [NSFileManager defaultManager];
    if ([op isEqual:@"info"]) return @{ @"ok": @YES, @"pid": @(getpid()), @"os": [[NSProcessInfo processInfo] operatingSystemVersionString] };
    if ([op isEqual:@"exec"]) return execute(r);
    if ([op isEqual:@"network"]) {
        struct ifaddrs *list;
        if (getifaddrs(&list)) return failure(@"getifaddrs failed", errno);
        NSMutableArray *interfaces = [NSMutableArray array];
        for (struct ifaddrs *i = list; i; i = i->ifa_next) {
            char host[NI_MAXHOST] = "";
            if (i->ifa_addr && (i->ifa_addr->sa_family == AF_INET || i->ifa_addr->sa_family == AF_INET6))
                getnameinfo(i->ifa_addr, i->ifa_addr->sa_len, host, sizeof(host), NULL, 0, NI_NUMERICHOST);
            [interfaces addObject:@{ @"name": [NSString stringWithUTF8String:i->ifa_name], @"flags": @(i->ifa_flags),
                @"family": @(i->ifa_addr ? i->ifa_addr->sa_family : 0), @"address": [NSString stringWithUTF8String:host] }];
        }
        freeifaddrs(list);
        return @{ @"ok": @YES, @"interfaces": interfaces };
    }
    if ([op isEqual:@"system"]) {
        mach_port_t host = mach_host_self();
        host_cpu_load_info_data_t cpu; mach_msg_type_number_t count = HOST_CPU_LOAD_INFO_COUNT;
        kern_return_t kr = host_statistics(host, HOST_CPU_LOAD_INFO, (host_info_t)&cpu, &count);
        if (kr) { mach_port_deallocate(mach_task_self(), host); return failure_domain(@"host_statistics CPU failed", kr, @"Mach"); }
        vm_statistics64_data_t vm; count = HOST_VM_INFO64_COUNT;
        kr = host_statistics64(host, HOST_VM_INFO64, (host_info64_t)&vm, &count);
        mach_port_deallocate(mach_task_self(), host);
        if (kr) return failure_domain(@"host_statistics64 VM failed", kr, @"Mach");
        return @{ @"ok": @YES, @"cpu_ticks": @{ @"user": @(cpu.cpu_ticks[CPU_STATE_USER]), @"system": @(cpu.cpu_ticks[CPU_STATE_SYSTEM]),
             @"idle": @(cpu.cpu_ticks[CPU_STATE_IDLE]), @"nice": @(cpu.cpu_ticks[CPU_STATE_NICE]) },
             @"vm_pages": @{ @"free": @(vm.free_count), @"active": @(vm.active_count), @"inactive": @(vm.inactive_count),
                 @"wired": @(vm.wire_count), @"compressed": @(vm.compressor_page_count) }, @"page_size": @(getpagesize()) };
    }
    if ([op isEqual:@"properties"]) {
        uint64_t entryID = [r[@"id"] unsignedLongLongValue];
        io_service_t entry = IOServiceGetMatchingService(0, IORegistryEntryIDMatching(entryID));
        if (!entry) return failure(@"IORegistry entry ID not found", ENOENT);
        CFMutableDictionaryRef properties = NULL;
        kern_return_t kr = IORegistryEntryCreateCFProperties(entry, &properties, kCFAllocatorDefault, 0);
        IOObjectRelease(entry);
        if (kr || !properties) return failure_domain(@"IORegistryEntryCreateCFProperties failed", kr, @"IOKit");
        NSDictionary *values = CFBridgingRelease(properties);
        return @{ @"ok": @YES, @"id": @(entryID), @"properties": property_value(values, 0) };
    }
    if ([op isEqual:@"mach"]) {
        NSString *name = r[@"name"];
        if (![name isKindOfClass:[NSString class]] || !name.length || name.length > 127) return failure(@"invalid Mach service name", EINVAL);
        mach_port_t port = MACH_PORT_NULL; mach_port_type_t type = 0;
        kern_return_t kr = bootstrap_look_up(bootstrap_port, name.UTF8String, &port);
        if (kr) return failure_domain(@"bootstrap_look_up failed", kr, @"Mach");
        kr = mach_port_type(mach_task_self(), port, &type);
        mach_port_deallocate(mach_task_self(), port);
        if (kr) return failure_domain(@"mach_port_type failed", kr, @"Mach");
        return @{ @"ok": @YES, @"name": name, @"port_type": @(type), @"note": @"lookup proves registration; service-specific RPCs still need testing" };
    }
    if ([op isEqual:@"framework"]) {
        NSString *path = r[@"path"], *symbol = r[@"symbol"];
        if (![path isKindOfClass:[NSString class]] || !([path hasPrefix:@"/System/Library/"] || [path hasPrefix:@"/usr/lib/"])) return failure(@"system library path required", EINVAL);
        void *lib = dlopen(path.fileSystemRepresentation, RTLD_LAZY | RTLD_LOCAL);
        if (!lib) return failure([NSString stringWithUTF8String:dlerror() ?: "dlopen failed"], ENOENT);
        BOOL found = !symbol || dlsym(lib, symbol.UTF8String) != NULL;
        dlclose(lib);
        return found ? @{ @"ok": @YES, @"loaded": path, @"symbol_found": @(symbol != nil) } : failure(@"dlsym: symbol unavailable", ENOENT);
    }
    if ([op isEqual:@"apps"]) {
        dlopen("/System/Library/Frameworks/MobileCoreServices.framework/MobileCoreServices", RTLD_LAZY);
        Class cls = NSClassFromString(@"LSApplicationWorkspace");
        SEL workspace = NSSelectorFromString(@"defaultWorkspace"), all = NSSelectorFromString(@"allApplications");
        id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
        if (!cls || ![cls respondsToSelector:workspace]) return failure(@"LSApplicationWorkspace unavailable", ENOTSUP);
        id ws = send(cls, workspace);
        if (![ws respondsToSelector:all]) return failure(@"application enumeration unavailable", ENOTSUP);
        NSMutableArray *apps = [NSMutableArray array];
        for (id app in send(ws, all)) {
            SEL bundle = NSSelectorFromString(@"bundleIdentifier"), name = NSSelectorFromString(@"localizedName");
            if ([app respondsToSelector:bundle]) [apps addObject:@{ @"bundle": send(app, bundle) ?: @"", @"name": [app respondsToSelector:name] ? send(app, name) ?: @"" : @"" }];
        }
        return @{ @"ok": @YES, @"applications": apps };
    }
    if ([op isEqual:@"clipboard"]) {
        dlopen("/System/Library/Frameworks/UIKit.framework/UIKit", RTLD_LAZY);
        Class cls = NSClassFromString(@"UIPasteboard");
        id (*send)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
        SEL general = NSSelectorFromString(@"generalPasteboard");
        if (!cls || ![cls respondsToSelector:general]) return failure(@"UIPasteboard unavailable", ENOTSUP);
        id board = send(cls, general);
        if (!board) return failure(@"pasteboard access refused", EPERM);
        if (r[@"text"]) {
            if (![r[@"text"] isKindOfClass:[NSString class]]) return failure(@"clipboard text must be a string", EINVAL);
            ((void (*)(id, SEL, id))objc_msgSend)(board, NSSelectorFromString(@"setString:"), r[@"text"]);
        }
        id value = send(board, NSSelectorFromString(@"string"));
        if (r[@"text"] && ![value isEqual:r[@"text"]]) return failure(@"pasteboard write/readback mismatch or privacy refusal", EACCES);
        return @{ @"ok": @YES, @"text": value ?: [NSNull null], @"access_verified": @(value != nil),
                  @"note": @"an empty value does not distinguish empty content from iOS privacy restrictions" };
    }
    if ([op isEqual:@"signal"]) {
        int pid = [r[@"pid"] intValue], sig = [r[@"signal"] intValue];
        if (pid <= 1 || (sig != SIGTERM && sig != SIGKILL && sig != SIGSTOP && sig != SIGCONT)) return failure(@"invalid pid or signal", EINVAL);
        return kill(pid, sig) ? failure(@"kill failed", errno) : @{ @"ok": @YES, @"signal_sent": @(sig), @"pid": @(pid) };
    }
    if ([op isEqual:@"ls"] || [op isEqual:@"stat"]) {
        NSString *path = r[@"path"];
        if (![path isKindOfClass:[NSString class]] || ![path hasPrefix:@"/"]) return failure(@"absolute path required", EINVAL);
        NSError *error = nil;
        if ([op isEqual:@"stat"]) {
            NSDictionary *attrs = [fm attributesOfItemAtPath:path error:&error];
            if (!attrs) return failure_domain(error.description, (int)error.code, error.domain);
            return @{ @"ok": @YES, @"attributes": [attrs description], @"size": attrs[NSFileSize] ?: @0,
                      @"type": attrs[NSFileType] ?: @"unknown", @"mode": attrs[NSFilePosixPermissions] ?: @0 };
        }
        NSArray *names = [fm contentsOfDirectoryAtPath:path error:&error];
        if (!names) return failure_domain(error.description, (int)error.code, error.domain);
        return @{ @"ok": @YES, @"path": path, @"entries": [names sortedArrayUsingSelector:@selector(compare:)] };
    }
    if ([op isEqual:@"read"]) {
        NSString *path = r[@"path"];
        long long offset = [r[@"offset"] longLongValue]; NSUInteger length = [r[@"length"] unsignedIntegerValue];
        if (![path isKindOfClass:[NSString class]] || ![path hasPrefix:@"/"] || offset < 0 || length > 24000)
            return failure(@"invalid read path, offset, or length", EINVAL);
        int fd = open(path.fileSystemRepresentation, O_RDONLY | O_NONBLOCK | O_NOFOLLOW);
        if (fd < 0) return failure(@"open failed", errno);
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return failure(@"only regular files can be read", EINVAL); }
        NSMutableData *data = [NSMutableData dataWithLength:length];
        ssize_t n = pread(fd, data.mutableBytes, length, offset); int saved = errno;
        struct stat after;
        if (fstat(fd, &after) || after.st_size != st.st_size || after.st_mtimespec.tv_sec != st.st_mtimespec.tv_sec ||
            after.st_mtimespec.tv_nsec != st.st_mtimespec.tv_nsec) { close(fd); return failure(@"file changed during read", ESTALE); }
        close(fd);
        if (n < 0) return failure(@"pread failed", saved);
        data.length = n;
        return @{ @"ok": @YES, @"offset": @(offset), @"bytes": @(n), @"size": @(st.st_size), @"inode": @(st.st_ino), @"mtime_ns": @((uint64_t)st.st_mtimespec.tv_sec * 1000000000ULL + st.st_mtimespec.tv_nsec), @"base64": [data base64EncodedStringWithOptions:0] };
    }
    if ([op isEqual:@"write"]) {
        NSString *name = r[@"name"];
        if (![name isKindOfClass:[NSString class]] || !name.length || ![name.lastPathComponent isEqual:name] || [name isEqual:@"."] || [name isEqual:@".."])
            return failure(@"upload name must be a single filename", EINVAL);
        NSString *dir = @"/private/var/root/iphont";
        NSError *error = nil;
        if (![fm createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:@{NSFilePosixPermissions:@0700} error:&error]) return failure_domain(error.description, (int)error.code, error.domain);
        NSData *data = [[NSData alloc] initWithBase64EncodedString:r[@"base64"] options:0];
        long long offset = [r[@"offset"] longLongValue];
        if (!data || data.length > 2048 || offset < 0) return failure(@"invalid upload data", EINVAL);
        int fd = open([[dir stringByAppendingPathComponent:name] fileSystemRepresentation], O_CREAT | O_WRONLY | O_NOFOLLOW | (offset == 0 ? O_EXCL : 0), 0600);
        if (fd < 0) return failure(@"open upload failed (new names only)", errno);
        struct stat st;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size != offset || st.st_uid != 0 || st.st_nlink != 1) { close(fd); return failure(@"upload offset or ownership mismatch", EINVAL); }
        ssize_t n = pwrite(fd, data.bytes, data.length, offset); int saved = errno; close(fd);
        if (n < 0 || (NSUInteger)n != data.length) return failure(@"short upload write", n < 0 ? saved : EIO);
        return @{ @"ok": @YES, @"bytes": @(n), @"path": [dir stringByAppendingPathComponent:name] };
    }
    if ([op isEqual:@"launch"]) {
        void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", RTLD_LAZY);
        int (*launch)(CFStringRef, BOOL) = sbs ? dlsym(sbs, "SBSLaunchApplicationWithIdentifier") : NULL;
        if (!launch) return failure(@"SBSLaunchApplicationWithIdentifier unavailable", ENOTSUP);
        NSString *bundle = r[@"bundle"];
        if (![bundle isKindOfClass:[NSString class]] || !bundle.length) return failure(@"bundle identifier required", EINVAL);
        int error = launch((__bridge CFStringRef)bundle, NO);
        return error ? failure_domain(@"SpringBoard refused application launch", error, @"SpringBoardServices") : @{ @"ok": @YES, @"launch_requested": bundle };
    }
    if ([op isEqual:@"memory"]) {
        int pid = [r[@"pid"] intValue]; mach_vm_address_t address = strtoull([r[@"address"] UTF8String], NULL, 0);
        unsigned size = [r[@"length"] unsignedIntValue];
        if (pid <= 1 || !size || size > 4096) return failure(@"memory reads require pid > 1 and length 1..4096", EINVAL);
        mach_port_t task = MACH_PORT_NULL;
        kern_return_t error = task_for_pid(mach_task_self(), pid, &task);
        if (error) return failure_domain(@"task_for_pid refused", error, @"Mach");
        char buf[4096]; vm_size_t got = 0;
        error = vm_read_overwrite(task, (vm_address_t)address, size, (vm_address_t)buf, &got);
        mach_port_deallocate(mach_task_self(), task);
        if (error) return failure_domain(@"vm_read_overwrite failed", error, @"Mach");
        NSData *data = [NSData dataWithBytes:buf length:got];
        return @{ @"ok": @YES, @"bytes": @(got), @"base64": [data base64EncodedStringWithOptions:0] };
    }
    if ([op isEqual:@"key"]) {
        unsigned usage = [r[@"usage"] unsignedIntValue];
        if (usage < 4 || usage > 231) return failure(@"invalid HID keyboard usage", EINVAL);
        void *lib = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY);
        CFTypeRef (*clientCreate)(CFAllocatorRef) = lib ? dlsym(lib, "IOHIDEventSystemClientCreate") : NULL;
        CFTypeRef (*keyCreate)(CFAllocatorRef, uint64_t, uint32_t, uint32_t, Boolean, uint32_t) = lib ? dlsym(lib, "IOHIDEventCreateKeyboardEvent") : NULL;
        void (*dispatch)(CFTypeRef, CFTypeRef) = lib ? dlsym(lib, "IOHIDEventSystemClientDispatchEvent") : NULL;
        if (!clientCreate || !keyCreate || !dispatch) return failure(@"IOHID keyboard dispatch unavailable", ENOTSUP);
        CFTypeRef client = clientCreate(NULL), event = keyCreate(NULL, mach_absolute_time(), 7, usage, [r[@"down"] boolValue], 0);
        if (!client || !event) { if (client) CFRelease(client); if (event) CFRelease(event); return failure(@"IOHID allocation failed", ENOMEM); }
        dispatch(client, event); CFRelease(event); CFRelease(client);
        return @{ @"ok": @YES, @"dispatched": @YES, @"delivery_verified": @NO, @"note": @"dispatch API provides no delivery acknowledgement" };
    }
    return failure(@"unsupported userspace operation", ENOTSUP);
}
int main(void)
{
    @autoreleasepool {
        int fd = open("/dev/iphont", O_RDWR | O_CLOEXEC);
        if (fd < 0) { fprintf(stderr, "iphontd: /dev/iphont: %s\n", strerror(errno)); return 1; }
        for (;;) { @autoreleasepool {
            unsigned char request[4096]; ssize_t n = read(fd, request, sizeof(request));
            if (n < 0 && errno == EINTR) continue;
            if (n < 4) { fprintf(stderr, "iphontd: bridge read failed: %s\n", strerror(errno)); break; }
            uint32_t sequence; memcpy(&sequence, request, 4);
            NSData *data = [NSData dataWithBytes:request + 4 length:n - 4];
            NSDictionary *result;
            @try {
                id input = [NSJSONSerialization JSONObjectWithData:data options:0 error:NULL];
                result = [input isKindOfClass:[NSDictionary class]] && valid_request(input, 0) ? handle(input) : failure(@"invalid JSON, embedded NUL, or excessive nesting", EINVAL);
            } @catch (NSException *exception) { result = failure(exception.reason, EINVAL); }
            NSData *body = [NSJSONSerialization dataWithJSONObject:result options:0 error:NULL];
            if (!body || body.length > 65536) body = [NSJSONSerialization dataWithJSONObject:failure(@"response exceeds 64 KiB; narrow the query", EOVERFLOW) options:0 error:NULL];
            struct reply header = {1, sequence, 0, (uint32_t)body.length, 0};
            NSMutableData *packet = [NSMutableData dataWithBytes:&header length:sizeof(header)]; [packet appendData:body];
            if (write(fd, packet.bytes, packet.length) != (ssize_t)packet.length) { fprintf(stderr, "iphontd: bridge write failed: %s\n", strerror(errno)); break; }
        } }
        close(fd);
    }
    return 1;
}
