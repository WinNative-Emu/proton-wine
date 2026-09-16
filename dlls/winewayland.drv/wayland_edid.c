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

#if 0
#pragma makedep unix
#endif

#include "config.h"

#include <math.h>
#include <string.h>

#include "wayland_edid.h"

/* An EDID 1.4 base block with one CTA-861 extension block. Two readers matter: win32u
 * (sysparams.c get_monitor_info_from_edid: vendor, product, name, preferred mode) and, through the
 * monitor's "EDID" registry value, DXVK's DXGI (libdisplay-info: the base block's chromaticity and
 * the CTA Colorimetry and HDR Static Metadata Data Blocks -> DXGI_OUTPUT_DESC1). */

/* "WAY", a PnP vendor id nobody has been assigned. */
#define EDID_VENDOR ((('W' - '@') << 10) | (('A' - '@') << 5) | ('Y' - '@'))
#define EDID_PRODUCT 0x0001
#define EDID_NAME "Wayland"

int wayland_edid_parse_nits(const char *str, double *nits)
{
    double value = 0, scale = 1;
    int digits = 0;

    if (!str) return 0;
    while (*str == ' ' || *str == '\t') str++;
    for (; *str >= '0' && *str <= '9'; str++, digits++) value = value * 10 + (*str - '0');
    if (*str == '.')
        for (str++; *str >= '0' && *str <= '9'; str++, digits++) value += (*str - '0') * (scale /= 10);
    while (*str == ' ' || *str == '\t') str++;
    if (*str || !digits || !(value <= 1e6)) return 0;
    *nits = value;
    return 1;
}

/* CTA-861.3: max = 50 * 2^(code / 32), min = max * (code / 255)^2 / 100, code 0 = not given. */
double wayland_edid_max_luminance_value(unsigned char code)
{
    return code ? 50.0 * pow(2.0, code / 32.0) : 0.0;
}

double wayland_edid_min_luminance_value(unsigned char code, unsigned char max_code)
{
    if (!code || !max_code) return 0.0;
    return wayland_edid_max_luminance_value(max_code) * (code / 255.0) * (code / 255.0) / 100.0;
}

unsigned char wayland_edid_max_luminance_code(double nits)
{
    double code;

    if (!(nits > 0)) return 0;
    code = floor(32.0 * log2(nits / 50.0) + 0.5);
    if (code < 1) return 1; /* 51 nits, the least the coding can say */
    if (code > 255) return 255;
    return code;
}

unsigned char wayland_edid_min_luminance_code(double nits, unsigned char max_code)
{
    double code;

    if (!max_code || !(nits > 0)) return 0;
    code = floor(255.0 * sqrt(nits * 100.0 / wayland_edid_max_luminance_value(max_code)) + 0.5);
    if (code < 1) return 1; /* a real, tiny black level rather than "not given" */
    if (code > 255) return 255;
    return code;
}

static unsigned int chromaticity(double value)
{
    return floor(value * 1024 + 0.5);
}

/* Display P3 primaries and D65 white: the panel DXVK itself assumes for HDR when it finds no EDID. */
static void put_chromaticity(unsigned char *base)
{
    const unsigned int rx = chromaticity(0.680), ry = chromaticity(0.320);
    const unsigned int gx = chromaticity(0.265), gy = chromaticity(0.690);
    const unsigned int bx = chromaticity(0.150), by = chromaticity(0.060);
    const unsigned int wx = chromaticity(0.3127), wy = chromaticity(0.3290);

    base[25] = ((rx & 3) << 6) | ((ry & 3) << 4) | ((gx & 3) << 2) | (gy & 3);
    base[26] = ((bx & 3) << 6) | ((by & 3) << 4) | ((wx & 3) << 2) | (wy & 3);
    base[27] = rx >> 2;
    base[28] = ry >> 2;
    base[29] = gx >> 2;
    base[30] = gy >> 2;
    base[31] = bx >> 2;
    base[32] = by >> 2;
    base[33] = wx >> 2;
    base[34] = wy >> 2;
}

/* The preferred mode, with CVT reduced blanking proportions (160 pixels of horizontal blanking,
 * at least 460 us of vertical blanking). Only the active size matters to Windows. */
static void put_detailed_timing(unsigned char *desc, int width, int height, int refresh_mhz)
{
    const unsigned int h_blank = 160, h_front = 48, h_sync = 32, v_front = 3, v_sync = 5, v_back = 6;
    unsigned int h_active = width < 1 ? 1 : width > 4095 ? 4095 : width;
    unsigned int v_active = height < 1 ? 1 : height > 4095 ? 4095 : height;
    unsigned int v_blank = v_front + v_sync + v_back, clock_code;
    double refresh = (refresh_mhz > 0 ? refresh_mhz : 60000) / 1000.0, frame = 1.0 / refresh, clock;

    if (frame > 460e-6)
    {
        double lines = ceil(460e-6 * v_active / (frame - 460e-6));
        if (lines > v_blank) v_blank = lines > 4095 ? 4095 : lines;
    }
    clock = (double)(h_active + h_blank) * (v_active + v_blank) * refresh / 10000.0; /* 10 kHz units */
    clock_code = clock < 1 ? 1 : clock > 65535 ? 65535 : floor(clock + 0.5);

    desc[0] = clock_code & 0xff;
    desc[1] = clock_code >> 8;
    desc[2] = h_active & 0xff;
    desc[3] = h_blank & 0xff;
    desc[4] = ((h_active >> 8) << 4) | (h_blank >> 8);
    desc[5] = v_active & 0xff;
    desc[6] = v_blank & 0xff;
    desc[7] = ((v_active >> 8) << 4) | (v_blank >> 8);
    desc[8] = h_front & 0xff;
    desc[9] = h_sync & 0xff;
    desc[10] = ((v_front & 0xf) << 4) | (v_sync & 0xf);
    desc[11] = ((h_front >> 8) << 6) | ((h_sync >> 8) << 4) | ((v_front >> 4) << 2) | (v_sync >> 4);
    /* 12-16: image size unknown, no borders */
    desc[17] = 0x1e; /* digital separate sync, positive polarities */
}

static void put_name(unsigned char *desc, const char *name)
{
    size_t i, len = strlen(name);

    desc[3] = 0xfc;
    for (i = 0; i < 13; i++) desc[5 + i] = i < len ? name[i] : i == len ? 0x0a : 0x20;
}

static void put_checksum(unsigned char *block)
{
    unsigned char sum = 0;
    unsigned int i;

    for (i = 0; i < 127; i++) sum += block[i];
    block[127] = -sum;
}

size_t wayland_edid_build(unsigned char edid[WAYLAND_EDID_SIZE], const struct wayland_edid_hdr *hdr,
                          int width, int height, int refresh_mhz)
{
    static const unsigned char header[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
    unsigned char max_code = wayland_edid_max_luminance_code(hdr->max_nits);
    unsigned char avg_code = wayland_edid_max_luminance_code(hdr->max_avg_nits);
    unsigned char min_code = wayland_edid_min_luminance_code(hdr->min_nits, max_code);
    unsigned char *base = edid, *cta = edid + 128, *block;
    unsigned int i, count;

    memset(edid, 0, WAYLAND_EDID_SIZE);

    memcpy(base, header, sizeof(header));
    base[8] = EDID_VENDOR >> 8;
    base[9] = EDID_VENDOR & 0xff;
    base[10] = EDID_PRODUCT & 0xff;
    base[11] = EDID_PRODUCT >> 8;
    /* 12-16: no serial number, week of manufacture not given */
    base[17] = 2026 - 1990;
    base[18] = 1; /* EDID 1.4 */
    base[19] = 4;
    base[20] = 0x80 | (3 << 4) | 0x05; /* digital, 10 bits per primary, DisplayPort */
    /* 21-22: screen size unknown */
    base[23] = 220 - 100; /* gamma 2.2 */
    base[24] = 0x02; /* RGB 4:4:4; the preferred timing is the native mode */
    put_chromaticity(base);
    /* 35-37: no established timings */
    for (i = 38; i < 54; i++) base[i] = 0x01; /* no standard timings */
    put_detailed_timing(base + 54, width, height, refresh_mhz);
    put_name(base + 72, EDID_NAME);
    base[90 + 3] = 0x10; /* dummy descriptors */
    base[108 + 3] = 0x10;
    base[126] = 1; /* one extension block */
    put_checksum(base);

    cta[0] = 0x02; /* CTA-861, revision 3 */
    cta[1] = 0x03;
    /* cta[3]: RGB only, no audio, no native detailed timings */
    block = cta + 4;
    /* Colorimetry Data Block: BT.2020 RGB. */
    *block++ = (7 << 5) | 3;
    *block++ = 0x05;
    *block++ = 0x80;
    *block++ = 0x00;
    /* HDR Static Metadata Data Block: traditional SDR gamma and SMPTE ST 2084, Static Metadata
     * Type 1, then the desired content max luminance, max frame-average luminance and min
     * luminance; trailing values that were not given are left out, and the min only goes with a max. */
    count = min_code && max_code ? 3 : avg_code ? 2 : max_code ? 1 : 0;
    *block++ = (7 << 5) | (3 + count);
    *block++ = 0x06;
    *block++ = 0x05;
    *block++ = 0x01;
    if (count >= 1) *block++ = max_code;
    if (count >= 2) *block++ = avg_code;
    if (count >= 3) *block++ = min_code;
    cta[2] = block - cta; /* no detailed timings after the data blocks */
    put_checksum(cta);

    return WAYLAND_EDID_SIZE;
}
