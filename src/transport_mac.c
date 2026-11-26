// Sends DDC/CI packets to an external display on an Apple Silicon Mac.
// macOS has no public interface for this. IOAVService is a private IOKit interface that the
// well-known tools m1ddc, MonitorControl and BetterDisplay use for the same purpose.
#include "transport_mac.h"

#include "info_mac.h"
#include "ask_tty.h"
#include "database_curl.h"
#include "profile.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <unistd.h>

typedef CFTypeRef IOAVServiceRef;
extern IOAVServiceRef IOAVServiceCreateWithService(CFAllocatorRef allocator, io_service_t service);
extern IOReturn IOAVServiceReadI2C(IOAVServiceRef service, uint32_t chipAddress, uint32_t offset, void *outputBuffer, uint32_t outputBufferSize);
extern IOReturn IOAVServiceWriteI2C(IOAVServiceRef service, uint32_t chipAddress, uint32_t dataAddress, void *inputBuffer, uint32_t inputBufferSize);

#define DDC_CHIP 0x37 // the I2C address every DDC/CI monitor listens on
#define DDC_HOST 0x51 // the address a computer signs its requests with

static int mac_write(void *ctx, const uint8_t *packet, size_t n) {
	uint8_t copy[64];
	if (n > sizeof copy) return -1;
	memcpy(copy, packet, n);
	return IOAVServiceWriteI2C((IOAVServiceRef)ctx, DDC_CHIP, DDC_HOST, copy, (uint32_t)n) == kIOReturnSuccess ? 0 : -1;
}

static int mac_read(void *ctx, uint8_t *buffer, size_t n) {
	memset(buffer, 0, n);
	return IOAVServiceReadI2C((IOAVServiceRef)ctx, DDC_CHIP, DDC_HOST, buffer, (uint32_t)n) == kIOReturnSuccess ? 0 : -1;
}

static void mac_pause(void *ctx, unsigned milliseconds) {
	(void)ctx;
	usleep(milliseconds * 1000);
}

int transport_mac_open(struct transport *t) {
	io_iterator_t iterator;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("DCPAVServiceProxy"), &iterator) != KERN_SUCCESS) return -1;

	// The Mac's built-in screen also has a service of this kind. Only an external one speaks DDC/CI.
	IOAVServiceRef found = NULL;
	io_service_t service;
	while ((service = IOIteratorNext(iterator))) {
		CFStringRef location = IORegistryEntryCreateCFProperty(service, CFSTR("Location"), kCFAllocatorDefault, 0);
		if (!found && location && CFStringCompare(location, CFSTR("External"), 0) == kCFCompareEqualTo) {
			found = IOAVServiceCreateWithService(kCFAllocatorDefault, service);
		}
		if (location) CFRelease(location);
		IOObjectRelease(service);
	}
	IOObjectRelease(iterator);
	if (!found) return -1;

	t->ctx = (void *)found;
	t->write = mac_write;
	t->read = mac_read;
	t->pause = mac_pause;
	t->info = info_mac_read;
	t->save = profile_save;
	t->lookup = database_lookup;
	t->ask = ask_tty;
	t->upload = database_upload;
	return 0;
}
