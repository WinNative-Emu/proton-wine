/*
 * WAYLAND display device functions
 *
 * Copyright 2020 Alexandros Frantzis for Collabora Ltd
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

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "waylanddrv.h"
#include "wayland_edid.h"

#include "wine/debug.h"

#include "ntuser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

/* The screen's luminance, from the Bannerlator app (per session). With none of the three
 * variables set the monitor gets no EDID, as before. */
static struct wayland_edid_hdr edid_hdr = {-1, -1, -1};
static BOOL edid_hdr_given;
/* DXVK_HDR=1: the app switched HDR output on for this session, read exactly as Proton's X11
 * driver reads it, so both drivers gate Windows advanced colour on the same thing. */
static BOOL hdr_output_enabled;
/* The EDID names a real peak luminance, so it describes an HDR10 screen. An SDR screen gets no
 * EDID from us at all, and must never be able to claim advanced colour. */
static BOOL edid_hdr_has_peak;

static void edid_hdr_init(void)
{
    static const char *names[] =
    {
        "BANNER_WAYLAND_HDR_MAX_NITS", "BANNER_WAYLAND_HDR_MAX_AVG_NITS", "BANNER_WAYLAND_HDR_MIN_NITS"
    };
    double *values[] = {&edid_hdr.max_nits, &edid_hdr.max_avg_nits, &edid_hdr.min_nits};
    unsigned char max_code, avg_code, min_code;
    char said[3][48];
    const char *env;
    int i;

    hdr_output_enabled = (env = getenv("DXVK_HDR")) && *env == '1';

    for (i = 0; i < 3; i++)
    {
        if (!(env = getenv(names[i]))) continue;
        if (wayland_edid_parse_nits(env, values[i])) edid_hdr_given = TRUE;
        else MESSAGE("winewayland: %s=%s is not a number of nits, ignoring it\n", names[i], env);
    }
    if (!edid_hdr_given) return;

    max_code = wayland_edid_max_luminance_code(edid_hdr.max_nits);
    edid_hdr_has_peak = max_code != 0;
    avg_code = wayland_edid_max_luminance_code(edid_hdr.max_avg_nits);
    min_code = wayland_edid_min_luminance_code(edid_hdr.min_nits, max_code);
    if (max_code) snprintf(said[0], sizeof(said[0]), "%.4g (EDID %.1f)", edid_hdr.max_nits,
                           wayland_edid_max_luminance_value(max_code));
    else strcpy(said[0], "not given");
    if (avg_code) snprintf(said[1], sizeof(said[1]), "%.4g (EDID %.1f)", edid_hdr.max_avg_nits,
                           wayland_edid_max_luminance_value(avg_code));
    else strcpy(said[1], "not given");
    if (min_code) snprintf(said[2], sizeof(said[2]), "%.4g (EDID %.4f)", edid_hdr.min_nits,
                           wayland_edid_min_luminance_value(min_code, max_code));
    else if (edid_hdr.min_nits >= 0) snprintf(said[2], sizeof(said[2]), "%.4g (EDID: not given%s)",
                                              edid_hdr.min_nits, max_code ? "" : ", needs a max");
    else strcpy(said[2], "not given");
    MESSAGE("winewayland: HDR10 monitor description (EDID) for Windows: max %s, max frame-average %s, "
            "min %s nits\n", said[0], said[1], said[2]);
}

static const struct wayland_edid_hdr *get_edid_hdr(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    pthread_once(&once, edid_hdr_init);
    return edid_hdr_given ? &edid_hdr : NULL;
}

/* Which process described the screen, and what win32u was handed: once per process, and again
 * if it changes. win32u serializes display updates, so no lock is needed here. */
static void report_edid_handoff(const char *output_name, const struct wayland_output_mode *mode,
                                UINT edid_len, BOOL hdr_enabled)
{
    static UINT last_len = ~0u;
    static int last_width, last_height, last_hdr = -1;

    if (edid_len == last_len && mode->width == last_width && mode->height == last_height &&
        last_hdr == (int)hdr_enabled) return;
    last_len = edid_len;
    last_width = mode->width;
    last_height = mode->height;
    last_hdr = hdr_enabled;

    MESSAGE("winewayland: %s (pid %04x) built the screen's EDID for output %s (%dx%d) and hands "
            "win32u %u bytes, advanced colour %s\n", process_name ? process_name : "?",
            (UINT)GetCurrentProcessId(), output_name ? output_name : "?", mode->width, mode->height,
            edid_len, hdr_enabled ? "on (DXVK_HDR=1, HDR10 EDID)" :
            hdr_output_enabled ? "off (the EDID names no peak luminance)" : "off (DXVK_HDR is not 1)");
}

struct output_info
{
    int x, y;
    struct wayland_output_state *output;
};

static int output_info_cmp_primary_x_y(const void *va, const void *vb)
{
    const struct output_info *a = va;
    const struct output_info *b = vb;
    BOOL a_is_primary = a->x == 0 && a->y == 0;
    BOOL b_is_primary = b->x == 0 && b->y == 0;

    if (a_is_primary && !b_is_primary) return -1;
    if (!a_is_primary && b_is_primary) return 1;
    if (a->x < b->x) return -1;
    if (a->x > b->x) return 1;
    if (a->y < b->y) return -1;
    if (a->y > b->y) return 1;
    return strcmp(a->output->name, b->output->name);
}

static inline BOOL output_info_overlap(struct output_info *a, struct output_info *b)
{
    return b->x < a->x + a->output->current_mode->width &&
           b->x + b->output->current_mode->width > a->x &&
           b->y < a->y + a->output->current_mode->height &&
           b->y + b->output->current_mode->height > a->y;
}

/* Map a point to one of the four quadrants of our 2d coordinate space:
 * 0: bottom right (x >= 0, y >= 0)
 * 1: top right (x >= 0, y < 0)
 * 2: bottom left (x < 0, y >= 0)
 * 3: top left (x < 0, y < 0) */
static inline int point_to_quadrant(int x, int y)
{
    return (x < 0) * 2 + (y < 0);
}

/* Decide which of two outputs to keep stationary in order
 * to resolve an overlap. */
static struct output_info *output_info_get_overlap_anchor(struct output_info *a,
                                                          struct output_info *b)
{
    /* Preferences for the direction of growth in each quadrant, with a
     * lower value signifying a higher preference. */
    static const int quadrant_prefs[4][4] =
    {
        {0, 1, 2, 3}, /* quadrant 0 */
        {3, 0, 2, 1}, /* quadrant 1 */
        {2, 3, 0, 1}, /* quadrant 2 */
        {3, 2, 1, 0}, /* quadrant 3 */
    };
    int qa = point_to_quadrant(a->output->logical_x, a->output->logical_y);
    int qb = point_to_quadrant(b->output->logical_x, b->output->logical_y);
    /* Direction of growth if a is the anchor. */
    int qab = point_to_quadrant(b->output->logical_x - a->output->logical_x,
                                b->output->logical_y - a->output->logical_y);
    /* Direction of growth if b is the anchor. */
    int qba = point_to_quadrant(a->output->logical_x - b->output->logical_x,
                                a->output->logical_y - b->output->logical_y);

    /* If the two output origins are in different quadrants, use the output
     * in the lower valued quadrant as the anchor (so effectively outputs
     * grow/move away from quadrant 0). */
    if (qa != qb) return (qa < qb) ? a : b;

    /* If the outputs are in the same quadrant, use the preference for the
     * direction of growth in that quadrant to select the anchor. Again the
     * intended effect is to grow/move outputs away from the origin. */
    return (quadrant_prefs[qa][qab] < quadrant_prefs[qa][qba]) ? a : b;
}

static BOOL output_info_array_resolve_overlaps(struct wl_array *output_info_array)
{
    struct output_info *a, *b;
    BOOL found_overlap = FALSE;

    wl_array_for_each(a, output_info_array)
    {
        wl_array_for_each(b, output_info_array)
        {
            struct output_info *anchor, *move;
            BOOL x_use_end, y_use_end;
            double rel_x, rel_y;

            /* Break if we reach the same output in the inner loop, so that we
             * don't process output pairs twice (since order doesn't matter for
             * our algorithm.) */
            if (a == b) break;

            if (!output_info_overlap(a, b)) continue;
            found_overlap = TRUE;

            /* Decide which output to move to resolve the overlap. */
            anchor = output_info_get_overlap_anchor(a, b);
            move = anchor == a ? b : a;

            /* Move the selected output on the X axis to resolve the overlap,
             * while maintaining the same relative positioning of the outputs as
             * the one they have in logical space. Use either the start or end
             * of the moved output as the point to maintain the relative
             * position of, depending on whether the anchor is before or after
             * the moved output on the axis. */
            x_use_end = move->output->logical_x < anchor->output->logical_x;
            rel_x = (move->output->logical_x - anchor->output->logical_x +
                     (x_use_end ? move->output->logical_w : 0)) /
                    (double)anchor->output->logical_w;
            move->x = anchor->x + anchor->output->current_mode->width * rel_x -
                      (x_use_end ? move->output->current_mode->width : 0);

            /* Similarly for the Y axis. */
            y_use_end = move->output->logical_y < anchor->output->logical_y;
            rel_y = (move->output->logical_y - anchor->output->logical_y +
                     (y_use_end ? move->output->logical_h : 0)) /
                    (double)anchor->output->logical_h;
            move->y = anchor->y + anchor->output->current_mode->height * rel_y -
                      (y_use_end ? move->output->current_mode->height : 0);
        }
    }

    return found_overlap;
}

static void output_info_array_arrange_physical_coords(struct wl_array *output_info_array)
{
    struct output_info *info;
    size_t num_outputs = output_info_array->size / sizeof(struct output_info);
    int steps = 0;

    /* Set the initial physical pixel coordinates. */
    wl_array_for_each(info, output_info_array)
    {
        info->x = info->output->logical_x;
        info->y = info->output->logical_y;
    }

    /* Try to iteratively resolve overlaps, but be defensive and set an upper
     * iteration bound to ensure we avoid infinite loops. */
    while (output_info_array_resolve_overlaps(output_info_array) &&
           ++steps < num_outputs)
        continue;

    /* Now that we have our physical pixel coordinates, sort from physical left
     * to right, but ensure the primary output is first. */
    qsort(output_info_array->data, num_outputs, sizeof(struct output_info),
          output_info_cmp_primary_x_y);
}

static void wayland_add_device_gpu(const struct gdi_device_manager *device_manager,
                                   void *param)
{
    struct pci_id pci_id = {0};

    TRACE("\n");

    device_manager->add_gpu(NULL, &pci_id, NULL, param);
}

static void wayland_add_device_source(const struct gdi_device_manager *device_manager,
                                       void *param, UINT state_flags, struct output_info *output_info)
{
    UINT dpi = NtUserGetSystemDpiForProcess( NULL );
    TRACE("name=%s state_flags=0x%x\n",
          output_info->output->name, state_flags);
    device_manager->add_source(output_info->output->name, state_flags, dpi, param);
}

static void wayland_add_device_monitor(const struct gdi_device_manager *device_manager,
                                       void *param, struct output_info *output_info)
{
    struct wayland_output_mode *mode = output_info->output->current_mode;
    const struct wayland_edid_hdr *hdr = get_edid_hdr();
    struct gdi_monitor monitor = {0};
    unsigned char edid[WAYLAND_EDID_SIZE];

    SetRect(&monitor.rc_monitor, output_info->x, output_info->y,
            output_info->x + output_info->output->current_mode->width,
            output_info->y + output_info->output->current_mode->height);

    /* We don't have a direct way to get the work area in Wayland. */
    monitor.rc_work = monitor.rc_monitor;

    /* An HDR10 screen whose luminance the app handed over: describe it, so DXGI reports its
     * real peak, frame-average and black level instead of DXVK's stand-in values. */
    if (hdr)
    {
        monitor.edid_len = wayland_edid_build(edid, hdr, mode->width, mode->height, mode->refresh);
        monitor.edid = edid;
        /* And tell Windows the screen is in HDR, so a game's own HDR option stops being greyed
         * out: DisplayConfig reports advanced colour for this monitor. Both halves are needed -
         * the session's HDR switch is on, and this monitor really is described as HDR10. */
        monitor.hdr_enabled = hdr_output_enabled && edid_hdr_has_peak;
        report_edid_handoff(output_info->output->name, mode, monitor.edid_len, monitor.hdr_enabled);
    }

    TRACE("name=%s rc_monitor=rc_work=%s edid_len=%u hdr_enabled=%d\n",
          output_info->output->name, wine_dbgstr_rect(&monitor.rc_monitor), monitor.edid_len,
          monitor.hdr_enabled);

    device_manager->add_monitor(&monitor, param);
}

static void populate_devmode(struct wayland_output_mode *output_mode, DEVMODEW *mode)
{
    mode->dmFields = DM_DISPLAYORIENTATION | DM_BITSPERPEL | DM_PELSWIDTH | DM_PELSHEIGHT |
                     DM_DISPLAYFLAGS | DM_DISPLAYFREQUENCY;
    mode->dmDisplayOrientation = DMDO_DEFAULT;
    mode->dmDisplayFlags = 0;
    mode->dmBitsPerPel = 32;
    mode->dmPelsWidth = output_mode->width;
    mode->dmPelsHeight = output_mode->height;
    /* Round the refresh rate to calculate the win32 display frequency. */
    mode->dmDisplayFrequency = (output_mode->refresh + 500) / 1000;
}

static void wayland_add_device_modes(const struct gdi_device_manager *device_manager,
                                     void *param, struct output_info *output_info)
{
    DEVMODEW *modes, current = {.dmSize = sizeof(current)};
    struct wayland_output_mode *output_mode;
    int modes_count = 0;

    if (!(modes = malloc(output_info->output->modes_count * sizeof(*modes))))
        return;

    populate_devmode(output_info->output->current_mode, &current);
    current.dmFields |= DM_POSITION;
    current.dmPosition.x = output_info->x;
    current.dmPosition.y = output_info->y;

    RB_FOR_EACH_ENTRY(output_mode, &output_info->output->modes,
                      struct wayland_output_mode, entry)
    {
        DEVMODEW mode = {.dmSize = sizeof(mode)};
        populate_devmode(output_mode, &mode);
        modes[modes_count++] = mode;
    }

    device_manager->add_modes(&current, modes_count, modes, param);
    free(modes);
}

/***********************************************************************
 *      UpdateDisplayDevices (WAYLAND.@)
 */
UINT WAYLAND_UpdateDisplayDevices(const struct gdi_device_manager *device_manager, void *param)
{
    struct wayland_output *output;
    DWORD state_flags = DISPLAY_DEVICE_ATTACHED_TO_DESKTOP | DISPLAY_DEVICE_PRIMARY_DEVICE;
    struct wl_array output_info_array;
    struct output_info *output_info;

    TRACE("\n");

    wl_array_init(&output_info_array);

    pthread_mutex_lock(&process_wayland.output_mutex);

    wl_list_for_each(output, &process_wayland.output_list, link)
    {
        if (!output->current.current_mode) continue;
        output_info = wl_array_add(&output_info_array, sizeof(*output_info));
        if (output_info) output_info->output = &output->current;
        else ERR("Failed to allocate space for output_info\n");
    }

    output_info_array_arrange_physical_coords(&output_info_array);

    /* Populate GDI devices. */
    wayland_add_device_gpu(device_manager, param);

    wl_array_for_each(output_info, &output_info_array)
    {
        wayland_add_device_source(device_manager, param, state_flags, output_info);
        wayland_add_device_monitor(device_manager, param, output_info);
        wayland_add_device_modes(device_manager, param, output_info);
        state_flags &= ~DISPLAY_DEVICE_PRIMARY_DEVICE;
    }

    wl_array_release(&output_info_array);

    pthread_mutex_unlock(&process_wayland.output_mutex);

    return STATUS_SUCCESS;
}
