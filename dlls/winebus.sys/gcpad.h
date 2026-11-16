/*
 * Shared state between winemac.drv (GameController.framework reader) and
 * winebus.sys (HID gamepad device).
 *
 * On macOS 15+ wired Xbox controllers are owned by Apple's GameController
 * driver: their HID reports are vendor defined and are not delivered to plain
 * IOHID clients, and GameController only delivers input to the frontmost
 * application. winebus runs in a windowless process, so the process that owns
 * the game window reads the controller and publishes its state here.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 */

#ifndef __WINE_GCPAD_H
#define __WINE_GCPAD_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>

#define GCPAD_MAGIC    0x44504347 /* 'GCPD' */
#define GCPAD_VERSION  1
#define GCPAD_MAX_PADS 4

enum gcpad_button
{
    GCPAD_A, GCPAD_B, GCPAD_X, GCPAD_Y, GCPAD_LB, GCPAD_RB, GCPAD_BACK, GCPAD_START,
    GCPAD_LS, GCPAD_RS, GCPAD_UP, GCPAD_DOWN, GCPAD_LEFT, GCPAD_RIGHT, GCPAD_GUIDE,
};

struct gcpad_state
{
    volatile uint32_t seq;        /* odd while the input fields are being written */
    volatile uint32_t connected;
    uint32_t buttons;             /* 1 << enum gcpad_button */
    int16_t  lx, ly, rx, ry;      /* -32768..32767, y positive = up */
    uint16_t lt, rt;              /* 0..32767 */
    volatile uint32_t rumble_seq; /* written by winebus */
    uint16_t rumble_low, rumble_high; /* 0..65535 */
    uint32_t rumble_ms;
    char     name[64];
};

struct gcpad_shm
{
    uint32_t magic;
    uint32_t version;
    struct gcpad_state pads[GCPAD_MAX_PADS];
};

/* names are per prefix, so a game in one prefix does not drive another one */
static inline uint64_t gcpad_prefix_hash(void)
{
    const char *prefix = getenv( "WINEPREFIX" ), *home = getenv( "HOME" );
    char buffer[1024], resolved[1024];
    uint64_t hash = 0xcbf29ce484222325ull;
    const char *p;

    if (prefix) snprintf( buffer, sizeof(buffer), "%s", prefix );
    else snprintf( buffer, sizeof(buffer), "%s/.wine", home ? home : "" );
    p = realpath( buffer, resolved ) ? resolved : buffer;
    for (; *p; p++) hash = (hash ^ (unsigned char)*p) * 0x100000001b3ull;
    return hash;
}

static inline void gcpad_shm_name( char *buffer, size_t size )
{
    snprintf( buffer, size, "/winegc-%016llx", (unsigned long long)gcpad_prefix_hash() );
}

static inline void gcpad_notify_name( char *buffer, size_t size, const char *what )
{
    snprintf( buffer, size, "org.winehq.gcpad.%s.%016llx", what, (unsigned long long)gcpad_prefix_hash() );
}

static inline struct gcpad_shm *gcpad_shm_map(void)
{
    struct gcpad_shm *shm;
    char name[64];
    int fd;

    gcpad_shm_name( name, sizeof(name) );
    if ((fd = shm_open( name, O_RDWR | O_CREAT, 0600 )) < 0) return NULL;
    ftruncate( fd, sizeof(*shm) ); /* fails if it already has a size, which is fine */
    shm = mmap( NULL, sizeof(*shm), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0 );
    close( fd );
    if (shm == MAP_FAILED) return NULL;
    if (shm->magic != GCPAD_MAGIC || shm->version != GCPAD_VERSION)
    {
        memset( shm, 0, sizeof(*shm) );
        shm->version = GCPAD_VERSION;
        shm->magic = GCPAD_MAGIC;
    }
    return shm;
}

static inline int gcpad_enabled(void)
{
    const char *env = getenv( "WINE_GCPAD" );
    return !env || atoi( env );
}

#endif /* __WINE_GCPAD_H */
