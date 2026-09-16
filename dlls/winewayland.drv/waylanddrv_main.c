/*
 * WAYLANDDRV initialization code
 *
 * Copyright 2020 Alexandre Frantzis for Collabora Ltd
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

#include <dlfcn.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "ntstatus.h"
#define WIN32_NO_STATUS

#include "waylanddrv.h"

#include "wine/debug.h"

WINE_DEFAULT_DEBUG_CHANNEL(waylanddrv);

char *process_name = NULL;

static const struct user_driver_funcs waylanddrv_funcs =
{
    .pClipboardWindowProc = WAYLAND_ClipboardWindowProc,
    .pClipCursor = WAYLAND_ClipCursor,
    .pDesktopWindowProc = WAYLAND_DesktopWindowProc,
    .pDestroyWindow = WAYLAND_DestroyWindow,
    .pSetIMECompositionRect = WAYLAND_SetIMECompositionRect,
    .pKbdLayerDescriptor = WAYLAND_KbdLayerDescriptor,
    .pReleaseKbdTables = WAYLAND_ReleaseKbdTables,
    .pSetCursor = WAYLAND_SetCursor,
    .pSetCursorPos = WAYLAND_SetCursorPos,
    .pSetDesktopWindow = WAYLAND_SetDesktopWindow,
    .pSetLayeredWindowAttributes = WAYLAND_SetLayeredWindowAttributes,
    .pSetWindowIcons = WAYLAND_SetWindowIcons,
    .pSetWindowStyle = WAYLAND_SetWindowStyle,
    .pSetWindowText = WAYLAND_SetWindowText,
    .pSysCommand = WAYLAND_SysCommand,
    .pUpdateDisplayDevices = WAYLAND_UpdateDisplayDevices,
    .pWindowMessage = WAYLAND_WindowMessage,
    .pWindowPosChanged = WAYLAND_WindowPosChanged,
    .pClipClientSurfaces = WAYLAND_ClipClientSurfaces,
    .pWindowPosChanging = WAYLAND_WindowPosChanging,
    .pCreateWindowSurface = WAYLAND_CreateWindowSurface,
    .pVulkanInit = WAYLAND_VulkanInit,
    .pOpenGLInit = WAYLAND_OpenGLInit,
};

static void wayland_init_process_name(void)
{
    WCHAR *p, *appname;
    WCHAR appname_lower[MAX_PATH];
    DWORD appname_len;
    DWORD appnamez_size;
    DWORD utf8_size;
    int i;

    appname = NtCurrentTeb()->Peb->ProcessParameters->ImagePathName.Buffer;
    if ((p = wcsrchr(appname, '/'))) appname = p + 1;
    if ((p = wcsrchr(appname, '\\'))) appname = p + 1;
    appname_len = lstrlenW(appname);

    if (appname_len == 0 || appname_len >= MAX_PATH) return;

    for (i = 0; appname[i]; i++) appname_lower[i] = RtlDowncaseUnicodeChar(appname[i]);
    appname_lower[i] = 0;

    appnamez_size = (appname_len + 1) * sizeof(WCHAR);

    if (!RtlUnicodeToUTF8N(NULL, 0, &utf8_size, appname_lower, appnamez_size) &&
        (process_name = malloc(utf8_size)))
    {
        RtlUnicodeToUTF8N(process_name, utf8_size, &utf8_size, appname_lower, appnamez_size);
    }
}

/* Keep a Vulkan driver resident for the life of the process. The Vulkan loader unloads the
 * driver when a program destroys its last instance, and a later call still reaching into it
 * then jumps into unmapped code (DiRT Rally 2.0 probes a device, releases it, and spins on
 * that fault). The library is the manifest's library_path, relative to the manifest's dir.
 * Keeping the driver resident leaves its globals alive across a winevulkan unload, so
 * winevulkan pins itself too on a Wayland desktop and the two stay in step
 * (see dlls/winevulkan/loader.c). */
static void pin_icd_library(const char *json)
{
    char lib[PATH_MAX], buf[4096], *p, *q;
    const char *slash;
    size_t n, dir;
    FILE *f;

    if (!(f = fopen(json, "r"))) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!(p = strstr(buf, "\"library_path\"")) || !(p = strchr(p + 14, ':')) ||
        !(p = strchr(p, '"')) || !(q = strchr(++p, '"')))
    {
        MESSAGE("winewayland: no library_path in %s, driver not pinned\n", json);
        return;
    }
    *q = 0;
    slash = p[0] == '/' ? NULL : strrchr(json, '/');
    dir = slash ? slash - json + 1 : 0;
    if (dir + strlen(p) >= sizeof(lib)) return;
    memcpy(lib, json, dir);
    strcpy(lib + dir, p);
    if (!dlopen(lib, RTLD_NOW | RTLD_NODELETE))
        MESSAGE("winewayland: could not pin %s: %s\n", lib, dlerror());
}

/* <wine>/lib/wine/aarch64-unix/winewayland.so -> <wine>, the installed Proton tree that this
 * build's bundled drivers and data live under. */
static BOOL bundled_root(char *wine, size_t size)
{
    Dl_info info;
    char *p;
    int i;

    if (!dladdr((void *)bundled_root, &info) || !info.dli_fname) return FALSE;
    if (strlen(info.dli_fname) >= size - 96) return FALSE;
    strcpy(wine, info.dli_fname);
    for (i = 0; i < 4; i++)
    {
        if (!(p = strrchr(wine, '/'))) return FALSE;
        *p = 0;
    }
    return TRUE;
}

/* The bundled libxkbregistry / libxkbcommon are Termux builds whose compiled-in xkeyboard-config
 * root is Termux's private directory, unreadable from the app, so parsing the default ruleset
 * failed in every container and layouts could only be named "us" (wayland_keyboard.c). This
 * build ships xkeyboard-config's data under share/X11/xkb; point the libraries at it unless the
 * environment already chose a root. Must run before the compositor hands us a wl_keyboard. */
static void use_bundled_xkb(void)
{
    static const char rules[] = "/rules/evdev.xml";
    char wine[PATH_MAX], path[PATH_MAX];

    if (getenv("XKB_CONFIG_ROOT")) return;
    if (!bundled_root(wine, sizeof(wine))) return;
    snprintf(path, sizeof(path), "%s/share/X11/xkb%s", wine, rules);
    if (access(path, R_OK)) return;
    path[strlen(path) - (sizeof(rules) - 1)] = 0;
    setenv("XKB_CONFIG_ROOT", path, 1);
    MESSAGE("winewayland: Xkb config root %s\n", path);
}

/* Containers on the Bannerlator compositor point VK_ICD_FILENAMES at a wrapper driver that
 * can only present to X11, and their OpenGL is GLX-only. This build ships Wayland-capable
 * Turnips and Mesa's EGL + Zink next to Wine, so use those when we're on that compositor.
 *
 * Which Vulkan driver the game renders on is the app's call, through the environment:
 *   BANNER_WAYLAND_VK_ICD=<absolute path>   the ICD manifest of a driver the app manages (an
 *                                           imported one); taken when it is readable.
 *   BANNER_WAYLAND_VK_VARIANT=a7xx|a8xx|a8xx-perf|a8xx-gen8|a8xx-smxz|a8xx-white|a8xx-upstream
 *                                           one of the bundled Turnip variants
 *                                           (share/vulkan/icd.d/wayland_turnip_<v>.json,
 *                                           a8xx-perf -> _a8xx_perf, a8xx-gen8 -> _a8xx_gen8,
 *                                           a8xx-smxz -> _a8xx_smxz, a8xx-white -> _a8xx_white,
 *                                           a8xx-upstream -> _a8xx_upstream).
 *   neither                                 the plain bundled Turnip (wayland_turnip.json).
 * A value that cannot be honoured is reported (ERR) and falls through to the next line. Every
 * outcome is logged as "winewayland: Vulkan driver <manifest>", the app greps for that. */
static void use_bundled_drivers(void)
{
    static const char icd_base[] = "/share/vulkan/icd.d/wayland_turnip";
    static const char egl[] = "/lib/libEGL.so.1";
    char wine[PATH_MAX], path[PATH_MAX], icd[PATH_MAX];
    const char *env;
    BOOL app_selected = FALSE;

    if (!bundled_root(wine, sizeof(wine))) return;

    icd[0] = 0;
    if ((env = getenv("BANNER_WAYLAND_VK_ICD")) && *env)
    {
        if (env[0] == '/' && strlen(env) < sizeof(icd) && !access(env, R_OK))
        {
            strcpy(icd, env);
            app_selected = TRUE;
        }
        else ERR("winewayland: BANNER_WAYLAND_VK_ICD=%s is not a readable absolute path, ignoring it\n", env);
    }
    if (!icd[0] && (env = getenv("BANNER_WAYLAND_VK_VARIANT")) && *env)
    {
        const char *suffix = !strcmp(env, "a7xx") ? "_a7xx" :
                             !strcmp(env, "a8xx") ? "_a8xx" :
                             !strcmp(env, "a8xx-perf") ? "_a8xx_perf" :
                             !strcmp(env, "a8xx-gen8") ? "_a8xx_gen8" :
                             !strcmp(env, "a8xx-smxz") ? "_a8xx_smxz" :
                             !strcmp(env, "a8xx-white") ? "_a8xx_white" :
                             !strcmp(env, "a8xx-upstream") ? "_a8xx_upstream" : NULL;
        if (suffix)
        {
            snprintf(path, sizeof(path), "%s%s%s.json", wine, icd_base, suffix);
            if (!access(path, R_OK)) strcpy(icd, path);
            else ERR("winewayland: bundled Vulkan driver variant %s is missing (%s), using the plain one\n", env, path);
        }
        else if (strcmp(env, "plain"))
            ERR("winewayland: unknown BANNER_WAYLAND_VK_VARIANT=%s, using the plain driver\n", env);
    }
    if (!icd[0])
    {
        snprintf(path, sizeof(path), "%s%s.json", wine, icd_base);
        if (!access(path, R_OK)) strcpy(icd, path);
    }

    if (icd[0])
    {
        setenv("VK_ICD_FILENAMES", icd, 1);
        if (app_selected) ERR("winewayland: Vulkan driver %s (app-selected)\n", icd);
        else MESSAGE("winewayland: Vulkan driver %s\n", icd);
        pin_icd_library(icd);
    }

    /* OpenGL through EGL on Zink. On by default: Wine's builtin opengl32 and ddraw have nowhere
     * else to go, so native GL and DirectDraw titles are unavailable without it.
     * No fps number belongs here. This comment used to cite "the AIO Graphics Test runs OpenGL at
     * ~230 fps" as proof the path worked; that test presents every one of its backends - its
     * OpenGL one included - through a Vulkan swapchain (its queues are all {mesa vk ...}, never a
     * {mesa egl ...}), so it never touched EGL at all. What this path actually did until the
     * versionCode 7 layer was put every Zink display on Mesa's software Wayland backend, which
     * this gallium build has no rasteriser for (zink only, no LLVM): a native GL window committed
     * never-written buffers and came out solid black, with no GPU frame reaching the compositor.
     * The layer's Mesa now takes EGL's Wayland DRM path, where it reads the compositor's dma-buf
     * feedback for a render node and runs on zink + kopper: GL renders on the Turnip below and
     * presents through its Vulkan WSI, like every other game.
     * BANNER_WAYLAND_GL=0 turns it off. win32u's startup GPU probe stays skipped (below); that probe
     * in the desktop process used to deadlock every other process opening a display DC. */
    strcpy(path, wine);
    strcat(path, egl);
    if (!access(path, R_OK) && (!(env = getenv("BANNER_WAYLAND_GL")) || atoi(env)))
    {
        setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 1);
        /* NOT LIBGL_ALWAYS_SOFTWARE: that makes Zink demand a CPU Vulkan device, of which there is
         * none here - the game dies with "No matching GL pixel format available". The bundled Mesa
         * needs no such push: MESA_LOADER_DRIVER_OVERRIDE alone puts it on zink + kopper through
         * EGL's Wayland DRM path. */
        setenv("WINE_USE_EGL", "1", 1);
        /* win32u's display-cache update would now build a Zink context in every process, and
         * in the desktop owner that stalls everyone else's display DC. Vulkan already reports
         * the GPU, so let OpenGL initialise on first real use instead (win32u/sysparams.c). */
        setenv("WINE_SKIP_OPENGL_GPU_PROBE", "1", 1);
        MESSAGE("winewayland: OpenGL through %s (Zink)\n", path);
    }
}

static NTSTATUS waylanddrv_unix_init(void *arg)
{
    /* Set the user driver functions now so that they are available during
     * our initialization. We clear them on error. */
    __wine_set_user_driver(&waylanddrv_funcs, WINE_GDI_DRIVER_VERSION);

    wayland_init_process_name();
    use_bundled_xkb();

    if (!wayland_process_init()) goto err;

    if (process_wayland.banner_desktop_v1) use_bundled_drivers();

    return 0;

err:
    __wine_set_user_driver(NULL, WINE_GDI_DRIVER_VERSION);
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS waylanddrv_unix_read_events(void *arg)
{
    while (wl_display_dispatch_queue(process_wayland.wl_display,
                                     process_wayland.wl_event_queue) != -1)
        continue;
    /* This function only returns on a fatal error, e.g., if our connection
     * to the Wayland server is lost. */
    return STATUS_UNSUCCESSFUL;
}

static NTSTATUS waylanddrv_unix_init_clipboard(void *arg)
{
    /* If the compositor supports zwlr_data_control_manager_v1, we don't need
     * per-process clipboard window and handling, we can use the default clipboard
     * window from the desktop process. */
    if (process_wayland.zwlr_data_control_manager_v1) return STATUS_UNSUCCESSFUL;
    return STATUS_SUCCESS;
}

const unixlib_entry_t __wine_unix_call_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_funcs) == waylanddrv_unix_func_count);

#ifdef _WIN64

const unixlib_entry_t __wine_unix_call_wow64_funcs[] =
{
    waylanddrv_unix_init,
    waylanddrv_unix_read_events,
    waylanddrv_unix_init_clipboard,
};

C_ASSERT(ARRAYSIZE(__wine_unix_call_wow64_funcs) == waylanddrv_unix_func_count);

#endif /* _WIN64 */
