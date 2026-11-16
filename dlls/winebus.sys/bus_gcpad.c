/*
 * Gamepads read through GameController.framework by winemac.drv (macOS)
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

#include <stdarg.h>
#include <stdlib.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "winbase.h"
#include "winternl.h"
#include "ddk/wdm.h"
#include "ddk/hidtypes.h"
#include "hidusage.h"
#include "wine/debug.h"
#include "wine/unixlib.h"

#include "unix_private.h"

WINE_DEFAULT_DEBUG_CHANNEL(hid);

#ifdef __APPLE__

#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <notify.h>
#include <sched.h>
#include <sys/utsname.h>
#include "gcpad.h"

struct gcpad_device
{
    struct unix_device unix_device;
    UINT index;
    UINT last_seq;
    BOOL removed;
};

static pthread_mutex_t gcpad_cs = PTHREAD_MUTEX_INITIALIZER;
static struct list event_queue = LIST_INIT(event_queue);
static struct list device_list = LIST_INIT(device_list);
static struct gcpad_device *devices[GCPAD_MAX_PADS];
static struct gcpad_shm *shm;
static int notify_fd = -1, notify_token, stop_pipe[2] = {-1, -1};
static char rumble_notify[96];

static inline struct gcpad_device *impl_from_unix_device(struct unix_device *iface)
{
    return CONTAINING_RECORD(iface, struct gcpad_device, unix_device);
}

static void gcpad_device_destroy(struct unix_device *iface)
{
}

static NTSTATUS gcpad_device_start(struct unix_device *iface)
{
    return STATUS_SUCCESS;
}

static void gcpad_device_stop(struct unix_device *iface)
{
    struct gcpad_device *impl = impl_from_unix_device(iface);

    pthread_mutex_lock(&gcpad_cs);
    if (devices[impl->index] == impl) devices[impl->index] = NULL;
    list_remove(&impl->unix_device.entry);
    pthread_mutex_unlock(&gcpad_cs);
}

extern int gip_rumble_send(unsigned int index, uint16_t low, uint16_t high, uint16_t left_trigger, uint16_t right_trigger);

static void set_rumble(struct gcpad_device *impl, UINT duration_ms, USHORT low, USHORT high, USHORT left, USHORT right)
{
    struct gcpad_state *pad = &shm->pads[impl->index];

    /* wired controllers: CoreHaptics does not reach them, talk to the driver directly */
    if (gip_rumble_send(impl->index, low, high, left, right)) return;

    pad->rumble_low = low;
    pad->rumble_high = high;
    pad->rumble_ms = duration_ms;
    __sync_fetch_and_add(&pad->rumble_seq, 1);
    notify_post(rumble_notify);
}

static NTSTATUS gcpad_device_haptics_start(struct unix_device *iface, UINT duration_ms,
                                           USHORT rumble_intensity, USHORT buzz_intensity,
                                           USHORT left_intensity, USHORT right_intensity)
{
    struct gcpad_device *impl = impl_from_unix_device(iface);

    TRACE("iface %p, duration_ms %u, rumble_intensity %u, buzz_intensity %u, left_intensity %u, right_intensity %u.\n",
          iface, duration_ms, rumble_intensity, buzz_intensity, left_intensity, right_intensity);

    set_rumble(impl, duration_ms, rumble_intensity, buzz_intensity, left_intensity, right_intensity);
    return STATUS_SUCCESS;
}

static NTSTATUS gcpad_device_haptics_stop(struct unix_device *iface)
{
    struct gcpad_device *impl = impl_from_unix_device(iface);

    TRACE("iface %p.\n", iface);

    set_rumble(impl, 0, 0, 0, 0, 0);
    return STATUS_SUCCESS;
}

static NTSTATUS gcpad_device_physical_device_control(struct unix_device *iface, USAGE control)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS gcpad_device_physical_device_set_gain(struct unix_device *iface, BYTE percent)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS gcpad_device_physical_effect_control(struct unix_device *iface, BYTE index,
                                                     USAGE control, BYTE iterations)
{
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS gcpad_device_physical_effect_update(struct unix_device *iface, BYTE index,
                                                    struct effect_params *params)
{
    return STATUS_NOT_SUPPORTED;
}

static const struct hid_device_vtbl gcpad_device_vtbl =
{
    gcpad_device_destroy,
    gcpad_device_start,
    gcpad_device_stop,
    gcpad_device_haptics_start,
    gcpad_device_haptics_stop,
    gcpad_device_physical_device_control,
    gcpad_device_physical_device_set_gain,
    gcpad_device_physical_effect_control,
    gcpad_device_physical_effect_update,
};

static BOOL read_state(UINT index, struct gcpad_state *state)
{
    struct gcpad_state *pad = &shm->pads[index];
    UINT seq, tries = 0;

    do
    {
        if (tries++ > 1000) return FALSE;
        seq = pad->seq;
        if (seq & 1) { sched_yield(); continue; }
        memcpy(state, pad, sizeof(*state));
    } while ((seq & 1) || seq != pad->seq);

    state->seq = seq;
    return TRUE;
}

static void update_report(struct gcpad_device *impl, const struct gcpad_state *state)
{
    static const UINT buttons[] =
    {
        GCPAD_A, GCPAD_B, GCPAD_X, GCPAD_Y, GCPAD_LB, GCPAD_RB, GCPAD_BACK, GCPAD_START, GCPAD_LS, GCPAD_RS,
    };
    struct unix_device *iface = &impl->unix_device;
    UINT i;

    hid_device_set_abs_axis(iface, 0, state->lx);
    /* XUSB / GIP have y positive up, like GameController (the SDL backend has to flip SDL's values) */
    hid_device_set_abs_axis(iface, 1, state->ly);
    hid_device_set_abs_axis(iface, 2, state->rx);
    hid_device_set_abs_axis(iface, 3, state->ry);
    hid_device_set_abs_axis(iface, 4, state->lt);
    hid_device_set_abs_axis(iface, 5, state->rt);

    for (i = 0; i < ARRAY_SIZE(buttons); i++)
        hid_device_set_button(iface, i, !!(state->buttons & (1u << buttons[i])));
    /* same button indices as the SDL backend */
    hid_device_set_button(iface, 10, !!(state->buttons & (1u << GCPAD_UP)));
    hid_device_set_button(iface, 11, !!(state->buttons & (1u << GCPAD_RIGHT)));
    hid_device_set_button(iface, 12, !!(state->buttons & (1u << GCPAD_DOWN)));
    hid_device_set_button(iface, 13, !!(state->buttons & (1u << GCPAD_LEFT)));
    hid_device_set_button(iface, 16, !!(state->buttons & (1u << GCPAD_GUIDE)));

    hid_device_set_hatswitch_x(iface, 0, !!(state->buttons & (1u << GCPAD_RIGHT)) - !!(state->buttons & (1u << GCPAD_LEFT)));
    hid_device_set_hatswitch_y(iface, 0, !!(state->buttons & (1u << GCPAD_DOWN)) - !!(state->buttons & (1u << GCPAD_UP)));
}

static void add_device(UINT index, const struct gcpad_state *state)
{
    static const USAGE_AND_PAGE device_usage = {.UsagePage = HID_USAGE_PAGE_GENERIC, .Usage = HID_USAGE_GENERIC_GAMEPAD};
    struct device_desc desc =
    {
        /* GameController does not expose the real ids; any XInput capable id works for applications */
        .vid = 0x045e, .pid = 0x028e, .version = 0x0114,
        .input = -1, .uid = index,
        .is_gamepad = TRUE,
        .manufacturer = {'G','a','m','e','C','o','n','t','r','o','l','l','e','r',0},
        .serialnumber = {'0','0','0','0',0},
    };
    struct gcpad_device *impl;
    struct unix_device *iface;
    char name[sizeof(state->name) + 1];

    memcpy(name, state->name, sizeof(state->name));
    name[sizeof(state->name)] = 0;
    if (!name[0]) strcpy(name, "Controller");
    ntdll_umbstowcs(name, strlen(name) + 1, desc.product, ARRAY_SIZE(desc.product));
    desc.serialnumber[3] = '1' + index;

    if (!(impl = hid_device_create(&gcpad_device_vtbl, sizeof(*impl)))) return;
    iface = &impl->unix_device;
    impl->index = index;
    impl->last_seq = state->seq;

    if (!hid_device_begin_report_descriptor(iface, &device_usage) ||
        !hid_device_add_gamepad(iface) ||
        !hid_device_add_haptics(iface) ||
        !hid_device_end_report_descriptor(iface))
    {
        iface->vtbl->destroy(iface);
        return;
    }

    update_report(impl, state);

    TRACE("index %u, name %s, desc %s.\n", index, debugstr_a(name), debugstr_device_desc(&desc));

    list_add_tail(&device_list, &iface->entry);
    devices[index] = impl;
    bus_event_queue_device_created(&event_queue, iface, &desc);
}

static void sync_devices(void)
{
    struct gcpad_state state;
    struct gcpad_device *impl;
    UINT i;

    pthread_mutex_lock(&gcpad_cs);
    for (i = 0; i < GCPAD_MAX_PADS; i++)
    {
        if (!read_state(i, &state)) continue;
        impl = devices[i];

        if (!state.connected)
        {
            if (impl && !impl->removed)
            {
                TRACE("index %u disconnected.\n", i);
                impl->removed = TRUE;
                bus_event_queue_device_removed(&event_queue, &impl->unix_device);
            }
            continue;
        }

        if (impl && impl->removed) continue; /* wait for the device to be stopped */
        if (!impl)
        {
            add_device(i, &state);
            continue;
        }

        if (impl->last_seq == state.seq) continue;
        impl->last_seq = state.seq;
        update_report(impl, &state);
        bus_event_queue_input_report(&event_queue, &impl->unix_device, impl->unix_device.hid_device_state.report_buf,
                                     impl->unix_device.hid_device_state.report_len);
    }
    pthread_mutex_unlock(&gcpad_cs);
}

/* GameController.framework owns wired Xbox controllers since macOS 15 */
static BOOL gcpad_supported(void)
{
    static int supported = -1;
    struct utsname name;
    unsigned int major;

    if (supported < 0)
        supported = gcpad_enabled() && !uname(&name) && sscanf(name.release, "%u", &major) == 1 && major >= 24;
    return supported;
}

NTSTATUS gcpad_bus_init(void)
{
    char name[96];

    if (!gcpad_supported())
    {
        TRACE("disabled\n");
        return STATUS_NOT_SUPPORTED;
    }
    if (!(shm = gcpad_shm_map()))
    {
        ERR("failed to map shared memory, errno %d\n", errno);
        return STATUS_UNSUCCESSFUL;
    }
    if (pipe(stop_pipe))
    {
        ERR("failed to create pipe, errno %d\n", errno);
        return STATUS_UNSUCCESSFUL;
    }

    gcpad_notify_name(rumble_notify, sizeof(rumble_notify), "rumble");
    gcpad_notify_name(name, sizeof(name), "input");
    if (notify_register_file_descriptor(name, &notify_fd, 0, &notify_token) != NOTIFY_STATUS_OK)
    {
        WARN("failed to register for %s, falling back to polling\n", name);
        notify_fd = -1;
    }

    TRACE("shm %p, notification %s\n", shm, name);
    return STATUS_SUCCESS;
}

NTSTATUS gcpad_bus_wait(struct bus_event *result)
{
    struct pollfd pfd[2];
    char buffer[256];

    /* cleanup previously returned event */
    bus_event_cleanup(result);

    if (!shm) return STATUS_SUCCESS;

    for (;;)
    {
        if (bus_event_queue_pop(&event_queue, result)) return STATUS_PENDING;

        sync_devices();
        if (bus_event_queue_pop(&event_queue, result)) return STATUS_PENDING;

        pfd[0].fd = stop_pipe[0];
        pfd[0].events = POLLIN;
        pfd[1].fd = notify_fd;
        pfd[1].events = POLLIN;
        /* the timeout only catches a missed notification, or polls when there are none */
        if (poll(pfd, notify_fd >= 0 ? 2 : 1, notify_fd >= 0 ? 500 : 4) < 0 && errno != EINTR) break;
        if (pfd[0].revents) break;
        if (notify_fd >= 0 && pfd[1].revents) read(notify_fd, buffer, sizeof(buffer));
    }

    TRACE("GameController main loop exiting\n");
    bus_event_queue_destroy(&event_queue);
    return STATUS_SUCCESS;
}

NTSTATUS gcpad_bus_stop(void)
{
    if (stop_pipe[1] >= 0) write(stop_pipe[1], "", 1);
    return STATUS_SUCCESS;
}

BOOL gcpad_handles_device(WORD vid, WORD pid)
{
    /* Xbox controllers are published by winemac.drv, don't create them twice */
    return gcpad_supported() && is_xbox_gamepad(vid, pid);
}

#else

NTSTATUS gcpad_bus_init(void) { return STATUS_NOT_SUPPORTED; }
NTSTATUS gcpad_bus_wait(struct bus_event *result) { return STATUS_SUCCESS; }
NTSTATUS gcpad_bus_stop(void) { return STATUS_SUCCESS; }
BOOL gcpad_handles_device(WORD vid, WORD pid) { return FALSE; }

#endif
