/*
 * Wayland driver: the monitor description (EDID) given to Windows
 *
 * Copyright 2026 The412Banner
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

#ifndef __WINE_WAYLAND_EDID_H
#define __WINE_WAYLAND_EDID_H

#include <stddef.h>

/* Base block + one CTA-861 extension block. */
#define WAYLAND_EDID_SIZE 256

/* The screen's luminance as the app measured it, in nits (cd/m2); a negative value means the
 * app gave none. BANNER_WAYLAND_HDR_MAX_NITS / _MAX_AVG_NITS / _MIN_NITS. */
struct wayland_edid_hdr
{
    double max_nits;
    double max_avg_nits;
    double min_nits;
};

/* A plain decimal number of nits ("1351", "0.05", surrounding blanks allowed); 0 if it is not one. */
int wayland_edid_parse_nits(const char *str, double *nits);

/* CTA-861.3 luminance coding, as used in the HDR Static Metadata Data Block. A code of 0 means
 * "not given"; max_code is the desired content max luminance code the minimum is relative to. */
unsigned char wayland_edid_max_luminance_code(double nits);
unsigned char wayland_edid_min_luminance_code(double nits, unsigned char max_code);
double wayland_edid_max_luminance_value(unsigned char code);
double wayland_edid_min_luminance_value(unsigned char code, unsigned char max_code);

/* Fills edid with a WAYLAND_EDID_SIZE byte EDID 1.4 describing an HDR10-capable screen with the
 * given luminances (SMPTE ST 2084 and BT.2020 RGB in a CTA-861 extension block) whose preferred
 * mode is width x height at refresh_mhz. Returns the number of bytes written. */
size_t wayland_edid_build(unsigned char edid[WAYLAND_EDID_SIZE], const struct wayland_edid_hdr *hdr,
                          int width, int height, int refresh_mhz);

#endif /* __WINE_WAYLAND_EDID_H */
