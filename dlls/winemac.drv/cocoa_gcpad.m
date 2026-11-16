/*
 * MACDRV GameController.framework gamepad reader
 *
 * GameController only delivers input to the frontmost application, and since
 * macOS 15 it is the only way to read wired Xbox controllers. winebus.sys runs
 * in a windowless process, so the controllers are read here, in the process
 * that owns the windows, and published through shared memory (see
 * dlls/winebus.sys/gcpad.h and bus_gcpad.c).
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

#import <AppKit/AppKit.h>
#import <GameController/GameController.h>
#import <CoreHaptics/CoreHaptics.h>
#include <notify.h>

#include "../winebus.sys/gcpad.h"

void macdrv_gcpad_init(void);

#define RUMBLE_MAX_SECONDS 30.0

static struct gcpad_shm *shm;
static char input_notify[96];
static GCController *pads[GCPAD_MAX_PADS];
static CHHapticEngine *engines[GCPAD_MAX_PADS][2];
static id<CHHapticPatternPlayer> players[GCPAD_MAX_PADS][2];
static uint32_t rumble_seq[GCPAD_MAX_PADS];
static BOOL debug;

#define GCPAD_LOG(...) do { if (debug) NSLog(@"gcpad: " __VA_ARGS__); } while (0)

static int16_t axis_value(float value)
{
    float scaled = value * 32767.0f;
    if (scaled > 32767.0f) return 32767;
    if (scaled < -32768.0f) return -32768;
    return (int16_t)scaled;
}

static void write_state(int slot, GCExtendedGamepad *gamepad)
{
    struct gcpad_state *pad = &shm->pads[slot];
    uint32_t buttons = 0;

    if (gamepad)
    {
        if (gamepad.buttonA.pressed) buttons |= 1u << GCPAD_A;
        if (gamepad.buttonB.pressed) buttons |= 1u << GCPAD_B;
        if (gamepad.buttonX.pressed) buttons |= 1u << GCPAD_X;
        if (gamepad.buttonY.pressed) buttons |= 1u << GCPAD_Y;
        if (gamepad.leftShoulder.pressed) buttons |= 1u << GCPAD_LB;
        if (gamepad.rightShoulder.pressed) buttons |= 1u << GCPAD_RB;
        if (gamepad.buttonOptions.pressed) buttons |= 1u << GCPAD_BACK;
        if (gamepad.buttonMenu.pressed) buttons |= 1u << GCPAD_START;
        if (gamepad.leftThumbstickButton.pressed) buttons |= 1u << GCPAD_LS;
        if (gamepad.rightThumbstickButton.pressed) buttons |= 1u << GCPAD_RS;
        if (gamepad.dpad.up.pressed) buttons |= 1u << GCPAD_UP;
        if (gamepad.dpad.down.pressed) buttons |= 1u << GCPAD_DOWN;
        if (gamepad.dpad.left.pressed) buttons |= 1u << GCPAD_LEFT;
        if (gamepad.dpad.right.pressed) buttons |= 1u << GCPAD_RIGHT;
        if (gamepad.buttonHome.pressed) buttons |= 1u << GCPAD_GUIDE;
    }

    __sync_fetch_and_add(&pad->seq, 1);
    pad->buttons = buttons;
    pad->lx = gamepad ? axis_value(gamepad.leftThumbstick.xAxis.value) : 0;
    pad->ly = gamepad ? axis_value(gamepad.leftThumbstick.yAxis.value) : 0;
    pad->rx = gamepad ? axis_value(gamepad.rightThumbstick.xAxis.value) : 0;
    pad->ry = gamepad ? axis_value(gamepad.rightThumbstick.yAxis.value) : 0;
    pad->lt = gamepad ? axis_value(gamepad.leftTrigger.value) : 0;
    pad->rt = gamepad ? axis_value(gamepad.rightTrigger.value) : 0;
    __sync_fetch_and_add(&pad->seq, 1);

    notify_post(input_notify);
}

static void stop_motor(int slot, int motor)
{
    if (players[slot][motor])
    {
        [players[slot][motor] stopAtTime:CHHapticTimeImmediate error:NULL];
        [players[slot][motor] release];
        players[slot][motor] = nil;
    }
}

static void release_engines(int slot)
{
    int motor;

    for (motor = 0; motor < 2; motor++)
    {
        stop_motor(slot, motor);
        if (engines[slot][motor])
        {
            [engines[slot][motor] stopWithCompletionHandler:nil];
            [engines[slot][motor] release];
            engines[slot][motor] = nil;
        }
    }
}

static void set_motor(int slot, int motor, float intensity, uint32_t duration_ms)
{
    GCController *controller = pads[slot];
    CHHapticEventParameter *param;
    CHHapticPattern *pattern;
    CHHapticEvent *event;
    NSTimeInterval duration;
    NSError *error = nil;

    stop_motor(slot, motor);
    if (intensity <= 0 || !controller || !controller.haptics) return;

    if (!engines[slot][motor])
    {
        GCHapticsLocality locality = motor ? GCHapticsLocalityRightHandle : GCHapticsLocalityLeftHandle;
        CHHapticEngine *engine;

        if (![controller.haptics.supportedLocalities containsObject:locality]) locality = GCHapticsLocalityDefault;
        if (!(engine = [controller.haptics createEngineWithLocality:locality])) return;
        engine.stoppedHandler = ^(CHHapticEngineStoppedReason reason) {
            dispatch_async(dispatch_get_main_queue(), ^{
                if (engines[slot][motor] == engine) release_engines(slot);
            });
        };
        if (![engine startAndReturnError:&error])
        {
            GCPAD_LOG(@"failed to start haptic engine: %@", error);
            return;
        }
        engines[slot][motor] = [engine retain];
    }

    /* XInput vibration lasts until it is changed */
    duration = RUMBLE_MAX_SECONDS;
    (void)duration_ms;

    param = [[[CHHapticEventParameter alloc] initWithParameterID:CHHapticEventParameterIDHapticIntensity
                                                           value:intensity] autorelease];
    event = [[[CHHapticEvent alloc] initWithEventType:CHHapticEventTypeHapticContinuous
                                           parameters:@[param]
                                         relativeTime:0
                                             duration:duration] autorelease];
    pattern = [[[CHHapticPattern alloc] initWithEvents:@[event] parameters:@[] error:&error] autorelease];
    if (!pattern || !(players[slot][motor] = [[engines[slot][motor] createPlayerWithPattern:pattern error:&error] retain]) ||
        ![players[slot][motor] startAtTime:CHHapticTimeImmediate error:&error])
        GCPAD_LOG(@"failed to play haptic pattern: %@", error);
}

static void apply_rumble(BOOL force_stop)
{
    int slot;

    for (slot = 0; slot < GCPAD_MAX_PADS; slot++)
    {
        struct gcpad_state *pad = &shm->pads[slot];
        uint32_t seq = pad->rumble_seq;

        if (force_stop)
        {
            stop_motor(slot, 0);
            stop_motor(slot, 1);
            continue;
        }
        if (!pads[slot] || seq == rumble_seq[slot]) continue;
        rumble_seq[slot] = seq;
        GCPAD_LOG(@"rumble slot %d low %u high %u", slot, pad->rumble_low, pad->rumble_high);
        set_motor(slot, 0, pad->rumble_low / 65535.0f, pad->rumble_ms);
        set_motor(slot, 1, pad->rumble_high / 65535.0f, pad->rumble_ms);
    }
}

static int find_slot(GCController *controller)
{
    int slot;
    for (slot = 0; slot < GCPAD_MAX_PADS; slot++) if (pads[slot] == controller) return slot;
    return -1;
}

static void controller_connected(GCController *controller)
{
    GCExtendedGamepad *gamepad = controller.extendedGamepad;
    struct gcpad_state *pad;
    const char *name;
    int slot;

    /* other controllers have standard HID reports and are handled by winebus.sys itself */
    if (!gamepad || ![controller.productCategory hasPrefix:@"Xbox"]) return;
    if (find_slot(controller) >= 0) return;
    if ((slot = find_slot(nil)) < 0) return;

    pads[slot] = [controller retain];
    pad = &shm->pads[slot];
    name = [controller.vendorName ?: controller.productCategory UTF8String];
    if (!pad->connected)
    {
        memset(pad->name, 0, sizeof(pad->name));
        if (name) strncpy(pad->name, name, sizeof(pad->name) - 1);
    }
    rumble_seq[slot] = pad->rumble_seq;
    GCPAD_LOG(@"controller %@ (%@) connected in slot %d", controller.vendorName, controller.productCategory, slot);

    gamepad.valueChangedHandler = ^(GCExtendedGamepad *gamepad, GCControllerElement *element) {
        int slot = find_slot(gamepad.controller);
        if (slot >= 0) write_state(slot, gamepad);
    };

    if (!pad->connected || [NSApp isActive]) write_state(slot, gamepad);
    pad->connected = 1;
    notify_post(input_notify);
}

static void controller_disconnected(GCController *controller)
{
    int slot = find_slot(controller);

    if (slot < 0) return;
    GCPAD_LOG(@"controller in slot %d disconnected", slot);
    controller.extendedGamepad.valueChangedHandler = nil;
    release_engines(slot);
    [pads[slot] release];
    pads[slot] = nil;
    shm->pads[slot].connected = 0;
    notify_post(input_notify);
}

void macdrv_gcpad_init(void)
{
    static BOOL initialized;
    NSNotificationCenter *nc = [NSNotificationCenter defaultCenter];
    char rumble_notify[96];
    int token;

    if (initialized) return;
    initialized = YES;

    if (!gcpad_enabled()) return;
    if (@available(macOS 15.0, *)) {} else return;
    if (!(shm = gcpad_shm_map())) return;

    debug = getenv("WINE_GCPAD_DEBUG") != NULL;
    gcpad_notify_name(input_notify, sizeof(input_notify), "input");
    gcpad_notify_name(rumble_notify, sizeof(rumble_notify), "rumble");

    /* harmless when it is not honored; the foreground process is what matters */
    GCController.shouldMonitorBackgroundEvents = YES;

    [nc addObserverForName:GCControllerDidConnectNotification object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) { controller_connected(note.object); }];
    [nc addObserverForName:GCControllerDidDisconnectNotification object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) { controller_disconnected(note.object); }];
    [nc addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                    int slot;
                    for (slot = 0; slot < GCPAD_MAX_PADS; slot++)
                        if (pads[slot]) write_state(slot, pads[slot].extendedGamepad);
                }];
    [nc addObserverForName:NSApplicationDidResignActiveNotification object:nil queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification *note) {
                    int slot;
                    /* no more input is delivered from now on, don't leave buttons stuck or motors running */
                    for (slot = 0; slot < GCPAD_MAX_PADS; slot++)
                        if (pads[slot]) write_state(slot, nil);
                    apply_rumble(YES);
                }];

    notify_register_dispatch(rumble_notify, &token, dispatch_get_main_queue(), ^(int t) {
        if ([NSApp isActive]) apply_rumble(NO);
    });

    for (GCController *controller in [GCController controllers]) controller_connected(controller);

    /* a controller unplugged while no windowed process was running leaves a stale entry */
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 5 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
        int slot;
        for (slot = 0; slot < GCPAD_MAX_PADS; slot++)
        {
            if (pads[slot] || !shm->pads[slot].connected) continue;
            if ([[GCController controllers] count]) continue; /* slot order may differ between processes */
            shm->pads[slot].connected = 0;
            notify_post(input_notify);
        }
    });

    GCPAD_LOG(@"initialized, %lu controllers", (unsigned long)[[GCController controllers] count]);
}
