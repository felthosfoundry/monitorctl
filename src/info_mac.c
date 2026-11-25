// Reads what macOS already knows about the external display: its model name, its manufacturer
// and product numbers, when it was built, its current resolution, and the firmware version of
// its Thunderbolt controller.
// Nothing here talks to the monitor. The resolution comes from the public CoreGraphics interface,
// and the other two come from the IORegistry, the same place System Information reads them.
#include "info_mac.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <IOKit/IOKitLib.h>
#include <stdio.h>
#include <string.h>

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

// A monitor on a Thunderbolt cable appears as a Thunderbolt device with the same model name.
// System Information shows its firmware as the ROM version in hexadecimal, a dot, and the EEPROM
// revision: a ROM version of 97 and a revision of 2 read "61.2".
static void read_thunderbolt(const char *model, char *version, size_t size) {
	if (!model[0]) return;
	io_iterator_t iterator;
	if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOThunderboltSwitch"), &iterator) != KERN_SUCCESS) return;
	io_service_t service;
	while ((service = IOIteratorNext(iterator))) {
		CFTypeRef name = IORegistryEntryCreateCFProperty(service, CFSTR("Device Model Name"), kCFAllocatorDefault, 0);
		char text[64] = "";
		long long rom, revision;
		if (!version[0] && name && CFGetTypeID(name) == CFStringGetTypeID() && CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8) &&
			strcmp(text, model) == 0 && number_property(service, CFSTR("ROM Version"), &rom) && number_property(service, CFSTR("EEPROM Revision"), &revision)) {
			snprintf(version, size, "%llx.%llx", rom, revision);
		}
		if (name) CFRelease(name);
		IOObjectRelease(service);
	}
	IOObjectRelease(iterator);
}

int info_mac_read(void *ctx, struct display_info *info) {
	(void)ctx;
	memset(info, 0, sizeof *info);

	CGDirectDisplayID displays[16];
	uint32_t count = 0;
	if (CGGetOnlineDisplayList(16, displays, &count) != kCGErrorSuccess) return -1;
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
	read_thunderbolt(info->model, info->thunderbolt, sizeof info->thunderbolt);
	return 0;
}
