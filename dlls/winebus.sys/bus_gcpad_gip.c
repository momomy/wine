/*
 * Rumble for wired Xbox controllers owned by Apple's XboxGamepad driver (macOS 15+)
 *
 * GameController's CoreHaptics engines start without error for these
 * controllers but never send anything to the driver. The driver however
 * forwards 13 byte output reports (report id 1) unmodified to the USB pipe,
 * so a GIP rumble packet sent with IOHIDDeviceSetReport reaches the motors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#include "config.h"

#ifdef __APPLE__

#include <stdint.h>
#include <stdlib.h>
#include <pthread.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDLib.h>

int gip_rumble_send(unsigned int index, uint16_t low, uint16_t high, uint16_t left_trigger, uint16_t right_trigger);

#define GIP_CMD_RUMBLE      0x09
#define GIP_MOTOR_ALL       0x0f
#define GIP_RUMBLE_SIZE     13
#define GIP_OUTPUT_REPORT   1

static pthread_mutex_t gip_cs = PTHREAD_MUTEX_INITIALIZER;
static IOHIDManagerRef manager;
static uint8_t sequence;

static int get_int_property(IOHIDDeviceRef device, CFStringRef key)
{
    CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    int ret = 0;

    if (value && CFGetTypeID(value) == CFNumberGetTypeID()) CFNumberGetValue(value, kCFNumberIntType, &ret);
    return ret;
}

static int is_gip_device(IOHIDDeviceRef device)
{
    CFTypeRef transport = IOHIDDeviceGetProperty(device, CFSTR(kIOHIDTransportKey));

    if (get_int_property(device, CFSTR(kIOHIDVendorIDKey)) != 0x045e) return 0;
    if (!transport || CFGetTypeID(transport) != CFStringGetTypeID() ||
        CFStringCompare(transport, CFSTR(kIOHIDTransportUSBValue), 0)) return 0;
    /* the driver only accepts 13 byte (rumble) and 5 byte (power) output reports */
    return get_int_property(device, CFSTR(kIOHIDMaxOutputReportSizeKey)) == GIP_RUMBLE_SIZE;
}

/* returns the number of controllers the packet was sent to */
int gip_rumble_send(unsigned int index, uint16_t low, uint16_t high, uint16_t left_trigger, uint16_t right_trigger)
{
    uint8_t packet[GIP_RUMBLE_SIZE] =
    {
        GIP_CMD_RUMBLE, 0x00, 0x00, 0x09, 0x00, GIP_MOTOR_ALL,
        left_trigger / 655, right_trigger / 655, low / 655, high / 655, /* 0..100 */
        0xff, 0x00, 0xeb, /* duration, delay, repeat: lasts until the next packet */
    };
    IOHIDDeviceRef devices[16];
    CFIndex i, count, matched = 0;
    CFSetRef set;
    int sent = 0;

    pthread_mutex_lock(&gip_cs);

    if (!manager)
    {
        int vendor = 0x045e;
        CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vendor);
        CFDictionaryRef matching = CFDictionaryCreate(kCFAllocatorDefault, (const void *[]){CFSTR(kIOHIDVendorIDKey)},
                                                      (const void *[]){number}, 1, &kCFTypeDictionaryKeyCallBacks,
                                                      &kCFTypeDictionaryValueCallBacks);

        /* only match the controllers: opening keyboards or mice asks for input monitoring permission */
        manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
        IOHIDManagerSetDeviceMatching(manager, matching);
        IOHIDManagerOpen(manager, kIOHIDOptionsTypeNone);
        CFRelease(matching);
        CFRelease(number);
    }

    if (manager && (set = IOHIDManagerCopyDevices(manager)))
    {
        count = CFSetGetCount(set);
        if (count > 0)
        {
            const void **values = malloc(count * sizeof(*values));
            CFSetGetValues(set, values);
            for (i = 0; i < count && matched < 16; i++)
                if (is_gip_device((IOHIDDeviceRef)values[i])) devices[matched++] = (IOHIDDeviceRef)values[i];
            free(values);
        }

        /* GameController does not tell which device a controller is, the order is a best guess */
        if (matched)
        {
            IOHIDDeviceRef device = devices[index < matched ? index : 0];

            packet[2] = sequence++;
            if (IOHIDDeviceSetReport(device, kIOHIDReportTypeOutput, GIP_OUTPUT_REPORT, packet, sizeof(packet)) == kIOReturnSuccess)
                sent = 1;
        }
        CFRelease(set);
    }

    pthread_mutex_unlock(&gip_cs);
    return sent;
}

#endif /* __APPLE__ */
