// Reads what macOS already knows about the external display: its model name, its manufacturer
// and product numbers, when it was built, its current resolution, and the firmware version of
// its Thunderbolt controller. It also reads a few facts that help when a monitor does not answer:
// the Mac's model, the macOS version, how many external displays there are, how many of them
// macOS offers a DDC/CI channel for, and which Thunderbolt devices are plugged in.
// Nothing here talks to the monitor. The resolution comes from the public CoreGraphics interface,
// and the rest comes from the IORegistry, the same place System Information reads it, and from
// sysctl. Serial numbers, the computer's name and the user's name are never read.
#include "info_mac.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>
#include <sys/sysctl.h>

static CFDictionaryRef dictionary_in(CFDictionaryRef parent, CFStringRef key) {
	CFTypeRef value = CFDictionaryGetValue(parent, key);
	return value && CFGetTypeID(value) == CFDictionaryGetTypeID() ? value : NULL;
}

static int number_is(CFDictionaryRef dictionary, CFStringRef key, long long expected) {
	CFTypeRef value = CFDictionaryGetValue(dictionary, key);
	long long number;
	if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return 0;
	return CFNumberGetValue(value, kCFNumberLongLongType, &number) && number == expected;
}

// A whole number from a dictionary, or 0 when it is missing or negative.
static unsigned number_in(CFDictionaryRef dictionary, CFStringRef key) {
	CFTypeRef value = CFDictionaryGetValue(dictionary, key);
	long long number;
	if (!value || CFGetTypeID(value) != CFNumberGetTypeID()) return 0;
	return CFNumberGetValue(value, kCFNumberLongLongType, &number) && number > 0 ? (unsigned)number : 0;
}

static int number_property(io_service_t service, CFStringRef key, long long *number) {
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	int ok = value && CFGetTypeID(value) == CFNumberGetTypeID() && CFNumberGetValue(value, kCFNumberLongLongType, number);
	if (value) CFRelease(value);
	return ok;
}

// Each display's registry entry carries the vendor and product numbers from its EDID next to the
// product name and the build date. CoreGraphics reports the same two numbers, so they pick out
// the right entry. The entry also holds serial numbers, which are never read.
static void read_product(struct display_info *info) {
	char *model = info->model;
	size_t size = sizeof info->model;
	uint32_t vendor = info->vendor, product = info->product;
	io_iterator_t iterator;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOMobileFramebufferShim"), &iterator) != KERN_SUCCESS) return;
	io_service_t service;
	while ((service = IOIteratorNext(iterator))) {
		CFTypeRef attributes = IORegistryEntryCreateCFProperty(service, CFSTR("DisplayAttributes"), kCFAllocatorDefault, 0);
		if (!model[0] && attributes && CFGetTypeID(attributes) == CFDictionaryGetTypeID()) {
			CFDictionaryRef details = dictionary_in(attributes, CFSTR("ProductAttributes"));
			if (details && number_is(details, CFSTR("LegacyManufacturerID"), vendor) && number_is(details, CFSTR("ProductID"), product)) {
				CFTypeRef name = CFDictionaryGetValue(details, CFSTR("ProductName"));
				if (name && CFGetTypeID(name) == CFStringGetTypeID() && !CFStringGetCString(name, model, (CFIndex)size, kCFStringEncodingUTF8)) model[0] = '\0';
				info->year = number_in(details, CFSTR("YearOfManufacture"));
				info->week = number_in(details, CFSTR("WeekOfManufacture"));
			}
		}
		if (attributes) CFRelease(attributes);
		IOObjectRelease(service);
	}
	IOObjectRelease(iterator);
}

// Copies a text property into `text`, or leaves "" when it is missing or too long.
static void string_property(io_service_t service, CFStringRef key, char *text, size_t size) {
	text[0] = '\0';
	CFTypeRef value = IORegistryEntryCreateCFProperty(service, key, kCFAllocatorDefault, 0);
	if (value && CFGetTypeID(value) == CFStringGetTypeID() && !CFStringGetCString(value, text, (CFIndex)size, kCFStringEncodingUTF8)) text[0] = '\0';
	if (value) CFRelease(value);
}

// Lists the Thunderbolt devices plugged into the Mac. The Mac's own ports are in the same list
// at depth 0, and are skipped.
// System Information shows a device's firmware as the ROM version in hexadecimal, a dot, and the
// EEPROM revision: a ROM version of 97 and a revision of 2 read "61.2".
static void read_devices(struct display_info *info) {
	io_iterator_t iterator;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOThunderboltSwitch"), &iterator) != KERN_SUCCESS) return;
	int limit = (int)(sizeof info->devices / sizeof info->devices[0]);
	io_service_t service;
	while ((service = IOIteratorNext(iterator))) {
		long long depth = 0, rom, revision;
		if (info->device_count < limit && number_property(service, CFSTR("Depth"), &depth) && depth > 0) {
			struct attached_device *device = &info->devices[info->device_count];
			string_property(service, CFSTR("Device Vendor Name"), device->vendor, sizeof device->vendor);
			string_property(service, CFSTR("Device Model Name"), device->model, sizeof device->model);
			if (number_property(service, CFSTR("ROM Version"), &rom) && number_property(service, CFSTR("EEPROM Revision"), &revision)) {
				snprintf(device->firmware, sizeof device->firmware, "%llx.%llx", rom, revision);
			}
			if (device->vendor[0] || device->model[0]) info->device_count++;
			else memset(device, 0, sizeof *device);
		}
		IOObjectRelease(service);
	}
	IOObjectRelease(iterator);
}

// A monitor on a Thunderbolt cable appears as a Thunderbolt device with the same model name.
static void read_thunderbolt(struct display_info *info) {
	if (!info->model[0]) return;
	for (int i = 0; i < info->device_count; i++) {
		if (strcmp(info->devices[i].model, info->model) == 0 && info->devices[i].firmware[0]) {
			snprintf(info->thunderbolt, sizeof info->thunderbolt, "%s", info->devices[i].firmware);
			return;
		}
	}
}

// Counts the external displays that macOS offers a DDC/CI channel for.
static unsigned count_channels(void) {
	io_iterator_t iterator;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("DCPAVServiceProxy"), &iterator) != KERN_SUCCESS) return 0;
	unsigned count = 0;
	io_service_t service;
	while ((service = IOIteratorNext(iterator))) {
		char location[32];
		string_property(service, CFSTR("Location"), location, sizeof location);
		if (strcmp(location, "External") == 0) count++;
		IOObjectRelease(service);
	}
	IOObjectRelease(iterator);
	return count;
}

static void read_sysctl(const char *name, char *text, size_t size) {
	size_t length = size;
	if (sysctlbyname(name, text, &length, NULL, 0) != 0 || length == 0 || length > size) text[0] = '\0';
	else text[size - 1] = '\0';
}

int info_mac_read(void *ctx, struct display_info *info) {
	(void)ctx;
	memset(info, 0, sizeof *info);

	read_sysctl("hw.model", info->computer, sizeof info->computer);
	read_sysctl("kern.osproductversion", info->system, sizeof info->system);
	info->channels = count_channels();
	read_devices(info);

	CGDirectDisplayID displays[16];
	uint32_t count = 0;
	if (CGGetOnlineDisplayList(16, displays, &count) != kCGErrorSuccess) return -1;
	for (uint32_t j = 0; j < count; j++)
		if (!CGDisplayIsBuiltin(displays[j])) info->displays++;
	uint32_t i = 0;
	while (i < count && CGDisplayIsBuiltin(displays[i])) i++;
	if (i == count) return -1;
	CGDirectDisplayID display = displays[i];

	CGDisplayModeRef mode = CGDisplayCopyDisplayMode(display);
	if (mode) {
		info->width = (unsigned)CGDisplayModeGetPixelWidth(mode);
		info->height = (unsigned)CGDisplayModeGetPixelHeight(mode);
		info->refresh = CGDisplayModeGetRefreshRate(mode);
		CGDisplayModeRelease(mode);
	}

	info->vendor = CGDisplayVendorNumber(display);
	info->product = CGDisplayModelNumber(display);
	read_product(info);
	read_thunderbolt(info);
	return 0;
}
