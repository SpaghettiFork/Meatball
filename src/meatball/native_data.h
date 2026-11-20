/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef MEATBALL_NATIVE_DATA_H
#define MEATBALL_NATIVE_DATA_H

#include "native_surface.h"

struct MbData;
typedef struct MbDataSource MbDataSource;

typedef enum
{
    MB_SOURCE_TARGET,
    MB_SOURCE_ACTION,
    MB_SOURCE_DROP,
    MB_SOURCE_FINISHED,
    MB_SOURCE_CANCELLED,
    MB_SOURCE_DESTROYED,
} MbSourceEvent;

typedef struct
{
    void (*receive) (void *data, const char *mime, int fd);
    /* Defer source destruction until notification returns. DESTROYED may
     * release the provider state. */
    void (*notify) (void *data, MbSourceEvent event, const char *mime,
                    uint32_t action);
} MbDataSourceOps;

typedef enum
{
    MB_SELECTION_CLIPBOARD,
    MB_SELECTION_PRIMARY,
    MB_SELECTION_COUNT,
} MbSelectionKind;

typedef struct
{
    void (*selection) (void *data, MbSelectionKind kind, uint64_t seat,
                       MbDataSource *source);
    void (*removed) (void *data, uint64_t seat);
    void (*destroyed) (void *data, MbDataSource *source);
} MbDataBridge;

extern MbDataSource *mb_data_source_create (const char *const *mimes,
                                            size_t count, uint32_t actions,
                                            const MbDataSourceOps *ops,
                                            void *data);
extern void mb_data_source_destroy (MbDataSource *source);
/* Takes ownership of fd, including when the MIME type is unavailable. */
extern Bool mb_data_source_send (MbDataSource *source, const char *mime,
                                 int fd);
extern const char *const *mb_data_source_mimes (MbDataSource *source,
                                                size_t *count);
extern uint32_t mb_data_source_actions (MbDataSource *source);
extern void mb_data_import_selection (MbSelectionKind kind,
                                      MbDataSource *source);
extern void mb_data_set_bridge (const MbDataBridge *bridge, void *data);
typedef struct
{
    MbSurface *surface;
    ScreenPtr screen;
    Window native;
    wl_fixed_t x, y;
    int32_t root_x, root_y;
    uint32_t time;
} MbDragPosition;

extern Bool mb_data_drag_import (uint64_t seat, MbDataSource *source);
extern void mb_data_drag_source_actions (uint64_t seat, uint32_t actions);
extern void mb_data_drag_native_action (uint64_t seat, uint32_t action);
extern void mb_data_drag_native_finish (uint64_t seat, Bool success);
extern void mb_data_drag_motion (uint64_t seat,
                                 const MbDragPosition *position);
extern void mb_data_drag_drop (uint64_t seat);
extern void mb_data_drag_cancel (uint64_t seat);
extern Bool mb_data_icon_changed (MbSurface *surface);
extern void mb_data_surface_destroyed (MbSurface *surface);

extern Bool mb_data_initialize (void);
extern void mb_data_shutdown (void);
extern void mb_data_keyboard_focus (uint64_t seat, MbClient *client);
extern void mb_data_seat_removed (uint64_t seat);

#endif
