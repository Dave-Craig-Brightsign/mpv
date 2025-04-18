/*
 * This file is part of mpv video player.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>

#include "common/msg.h"
#include "input/input.h"
#include "input/keycodes.h"
#include "options/m_config.h"
#include "osdep/io.h"
#include "osdep/timer.h"
#include "present_sync.h"
#include "wayland_common.h"
#include "win_state.h"

// Generated from wayland-protocols
#include "generated/wayland/idle-inhibit-unstable-v1.h"
#include "generated/wayland/linux-dmabuf-unstable-v1.h"
#include "generated/wayland/presentation-time.h"
#include "generated/wayland/xdg-decoration-unstable-v1.h"
#include "generated/wayland/xdg-shell.h"
#include "generated/wayland/viewporter.h"

#if WAYLAND_VERSION_MAJOR > 1 || WAYLAND_VERSION_MINOR >= 20
#define HAVE_WAYLAND_1_20
#endif

#ifndef CLOCK_MONOTONIC_RAW
#define CLOCK_MONOTONIC_RAW 4
#endif


static const struct mp_keymap keymap[] = {
    /* Special keys */
    {XKB_KEY_Pause,     MP_KEY_PAUSE}, {XKB_KEY_Escape, MP_KEY_ESC},
    {XKB_KEY_BackSpace, MP_KEY_BS},    {XKB_KEY_Tab,    MP_KEY_TAB},
    {XKB_KEY_Return,    MP_KEY_ENTER}, {XKB_KEY_Menu,   MP_KEY_MENU},
    {XKB_KEY_Print,     MP_KEY_PRINT},

    /* Cursor keys */
    {XKB_KEY_Left, MP_KEY_LEFT}, {XKB_KEY_Right, MP_KEY_RIGHT},
    {XKB_KEY_Up,   MP_KEY_UP},   {XKB_KEY_Down,  MP_KEY_DOWN},

    /* Navigation keys */
    {XKB_KEY_Insert,  MP_KEY_INSERT},  {XKB_KEY_Delete,    MP_KEY_DELETE},
    {XKB_KEY_Home,    MP_KEY_HOME},    {XKB_KEY_End,       MP_KEY_END},
    {XKB_KEY_Page_Up, MP_KEY_PAGE_UP}, {XKB_KEY_Page_Down, MP_KEY_PAGE_DOWN},

    /* F-keys */
    {XKB_KEY_F1,  MP_KEY_F + 1},  {XKB_KEY_F2,  MP_KEY_F + 2},
    {XKB_KEY_F3,  MP_KEY_F + 3},  {XKB_KEY_F4,  MP_KEY_F + 4},
    {XKB_KEY_F5,  MP_KEY_F + 5},  {XKB_KEY_F6,  MP_KEY_F + 6},
    {XKB_KEY_F7,  MP_KEY_F + 7},  {XKB_KEY_F8,  MP_KEY_F + 8},
    {XKB_KEY_F9,  MP_KEY_F + 9},  {XKB_KEY_F10, MP_KEY_F +10},
    {XKB_KEY_F11, MP_KEY_F +11},  {XKB_KEY_F12, MP_KEY_F +12},
    {XKB_KEY_F13, MP_KEY_F +13},  {XKB_KEY_F14, MP_KEY_F +14},
    {XKB_KEY_F15, MP_KEY_F +15},  {XKB_KEY_F16, MP_KEY_F +16},
    {XKB_KEY_F17, MP_KEY_F +17},  {XKB_KEY_F18, MP_KEY_F +18},
    {XKB_KEY_F19, MP_KEY_F +19},  {XKB_KEY_F20, MP_KEY_F +20},
    {XKB_KEY_F21, MP_KEY_F +21},  {XKB_KEY_F22, MP_KEY_F +22},
    {XKB_KEY_F23, MP_KEY_F +23},  {XKB_KEY_F24, MP_KEY_F +24},

    /* Numpad independent of numlock */
    {XKB_KEY_KP_Subtract, '-'}, {XKB_KEY_KP_Add,    '+'},
    {XKB_KEY_KP_Multiply, '*'}, {XKB_KEY_KP_Divide, '/'},
    {XKB_KEY_KP_Enter, MP_KEY_KPENTER},

    /* Numpad with numlock */
    {XKB_KEY_KP_0, MP_KEY_KP0}, {XKB_KEY_KP_1, MP_KEY_KP1},
    {XKB_KEY_KP_2, MP_KEY_KP2}, {XKB_KEY_KP_3, MP_KEY_KP3},
    {XKB_KEY_KP_4, MP_KEY_KP4}, {XKB_KEY_KP_5, MP_KEY_KP5},
    {XKB_KEY_KP_6, MP_KEY_KP6}, {XKB_KEY_KP_7, MP_KEY_KP7},
    {XKB_KEY_KP_8, MP_KEY_KP8}, {XKB_KEY_KP_9, MP_KEY_KP9},
    {XKB_KEY_KP_Decimal, MP_KEY_KPDEC}, {XKB_KEY_KP_Separator, MP_KEY_KPDEC},

    /* Numpad without numlock */
    {XKB_KEY_KP_Insert, MP_KEY_KPINS}, {XKB_KEY_KP_End,       MP_KEY_KP1},
    {XKB_KEY_KP_Down,   MP_KEY_KP2},   {XKB_KEY_KP_Page_Down, MP_KEY_KP3},
    {XKB_KEY_KP_Left,   MP_KEY_KP4},   {XKB_KEY_KP_Begin,     MP_KEY_KP5},
    {XKB_KEY_KP_Right,  MP_KEY_KP6},   {XKB_KEY_KP_Home,      MP_KEY_KP7},
    {XKB_KEY_KP_Up,     MP_KEY_KP8},   {XKB_KEY_KP_Page_Up,   MP_KEY_KP9},
    {XKB_KEY_KP_Delete, MP_KEY_KPDEL},

    /* Multimedia keys */
    {XKB_KEY_XF86MenuKB, MP_KEY_MENU},
    {XKB_KEY_XF86AudioPlay, MP_KEY_PLAY}, {XKB_KEY_XF86AudioPause, MP_KEY_PAUSE},
    {XKB_KEY_XF86AudioStop, MP_KEY_STOP},
    {XKB_KEY_XF86AudioPrev, MP_KEY_PREV}, {XKB_KEY_XF86AudioNext, MP_KEY_NEXT},
    {XKB_KEY_XF86AudioRewind, MP_KEY_REWIND},
    {XKB_KEY_XF86AudioForward, MP_KEY_FORWARD},
    {XKB_KEY_XF86AudioMute, MP_KEY_MUTE},
    {XKB_KEY_XF86AudioLowerVolume, MP_KEY_VOLUME_DOWN},
    {XKB_KEY_XF86AudioRaiseVolume, MP_KEY_VOLUME_UP},
    {XKB_KEY_XF86HomePage, MP_KEY_HOMEPAGE}, {XKB_KEY_XF86WWW, MP_KEY_WWW},
    {XKB_KEY_XF86Mail, MP_KEY_MAIL}, {XKB_KEY_XF86Favorites, MP_KEY_FAVORITES},
    {XKB_KEY_XF86Search, MP_KEY_SEARCH}, {XKB_KEY_XF86Sleep, MP_KEY_SLEEP},

    {0, 0}
};

#define OPT_BASE_STRUCT struct wayland_opts
const struct m_sub_options wayland_conf = {
    .opts = (const struct m_option[]) {
        {"wayland-configure-bounds", OPT_FLAG(configure_bounds)},
        {"wayland-disable-vsync", OPT_FLAG(disable_vsync)},
        {"wayland-edge-pixels-pointer", OPT_INT(edge_pixels_pointer),
            M_RANGE(0, INT_MAX)},
        {"wayland-edge-pixels-touch", OPT_INT(edge_pixels_touch),
            M_RANGE(0, INT_MAX)},
        {0},
    },
    .size = sizeof(struct wayland_opts),
    .defaults = &(struct wayland_opts) {
        .configure_bounds = true,
        .disable_vsync = false,
        .edge_pixels_pointer = 10,
        .edge_pixels_touch = 32,
    },
};

struct vo_wayland_output {
    struct vo_wayland_state *wl;
    struct wl_output *output;
    struct mp_rect geometry;
    bool has_surface;
    uint32_t id;
    uint32_t flags;
    int phys_width;
    int phys_height;
    int scale;
    double refresh_rate;
    char *make;
    char *model;
    char *name;
    struct wl_list link;
};

static int check_for_resize(struct vo_wayland_state *wl, wl_fixed_t x_w, wl_fixed_t y_w,
                            int edge_pixels, enum xdg_toplevel_resize_edge *edge);
static int get_mods(struct vo_wayland_state *wl);
static int lookupkey(int key);
static int set_cursor_visibility(struct vo_wayland_state *wl, bool on);
static int spawn_cursor(struct vo_wayland_state *wl);

static void greatest_common_divisor(struct vo_wayland_state *wl, int a, int b);
static void remove_output(struct vo_wayland_output *out);
static void request_decoration_mode(struct vo_wayland_state *wl, uint32_t mode);
static void set_geometry(struct vo_wayland_state *wl);
static void set_surface_scaling(struct vo_wayland_state *wl);
static void window_move(struct vo_wayland_state *wl, uint32_t serial);

/* Wayland listener boilerplate */
static void pointer_handle_enter(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface,
                                 wl_fixed_t sx, wl_fixed_t sy)
{
    struct vo_wayland_state *wl = data;

    wl->pointer    = pointer;
    wl->pointer_id = serial;

    set_cursor_visibility(wl, wl->cursor_visible);
    mp_input_put_key(wl->vo->input_ctx, MP_KEY_MOUSE_ENTER);
}

static void pointer_handle_leave(void *data, struct wl_pointer *pointer,
                                 uint32_t serial, struct wl_surface *surface)
{
    struct vo_wayland_state *wl = data;
    mp_input_put_key(wl->vo->input_ctx, MP_KEY_MOUSE_LEAVE);
}

static void pointer_handle_motion(void *data, struct wl_pointer *pointer,
                                  uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
    struct vo_wayland_state *wl = data;

    wl->mouse_x = wl_fixed_to_int(sx) * wl->scaling;
    wl->mouse_y = wl_fixed_to_int(sy) * wl->scaling;
    wl->mouse_unscaled_x = sx;
    wl->mouse_unscaled_y = sy;

    if (!wl->toplevel_configured)
        mp_input_set_mouse_pos(wl->vo->input_ctx, wl->mouse_x, wl->mouse_y);
    wl->toplevel_configured = false;
}

static void pointer_handle_button(void *data, struct wl_pointer *wl_pointer,
                                  uint32_t serial, uint32_t time, uint32_t button,
                                  uint32_t state)
{
    struct vo_wayland_state *wl = data;
    int mpmod = 0;

    state = state == WL_POINTER_BUTTON_STATE_PRESSED ? MP_KEY_STATE_DOWN
                                                     : MP_KEY_STATE_UP;

    if (button >= BTN_MOUSE && button < BTN_JOYSTICK) {
        switch (button) {
        case BTN_LEFT:
            button = MP_MBTN_LEFT;
            break;
        case BTN_MIDDLE:
            button = MP_MBTN_MID;
            break;
        case BTN_RIGHT:
            button = MP_MBTN_RIGHT;
            break;
        case BTN_SIDE:
            button = MP_MBTN_BACK;
            break;
        case BTN_EXTRA:
            button = MP_MBTN_FORWARD;
            break;
        default:
            button += MP_MBTN9 - BTN_FORWARD;
            break;
        }
    } else {
        button = 0;
    }

    if (wl->keyboard)
        mpmod = get_mods(wl);

    if (button)
        mp_input_put_key(wl->vo->input_ctx, button | state | mpmod);

    if (!mp_input_test_dragging(wl->vo->input_ctx, wl->mouse_x, wl->mouse_y) &&
        (!wl->vo_opts->fullscreen) && (!wl->vo_opts->window_maximized) &&
        (button == MP_MBTN_LEFT) && (state == MP_KEY_STATE_DOWN)) {
        uint32_t edges;
        // Implement an edge resize zone if there are no decorations
        if (!wl->xdg_toplevel_decoration &&
            check_for_resize(wl, wl->mouse_unscaled_x, wl->mouse_unscaled_y,
                             wl->opts->edge_pixels_pointer, &edges))
            xdg_toplevel_resize(wl->xdg_toplevel, wl->seat, serial, edges);
        else
            window_move(wl, serial);
        // Explictly send an UP event after the client finishes a move/resize
        mp_input_put_key(wl->vo->input_ctx, button | MP_KEY_STATE_UP);
    }
}

static void pointer_handle_axis(void *data, struct wl_pointer *wl_pointer,
                                uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct vo_wayland_state *wl = data;

    int mpmod = get_mods(wl);
    double val = wl_fixed_to_double(value) < 0 ? -1 : 1;
    switch (axis) {
    case WL_POINTER_AXIS_VERTICAL_SCROLL:
        if (value > 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_DOWN | mpmod, +val);
        if (value < 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_UP | mpmod, -val);
        break;
    case WL_POINTER_AXIS_HORIZONTAL_SCROLL:
        if (value > 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_RIGHT | mpmod, +val);
        if (value < 0)
            mp_input_put_wheel(wl->vo->input_ctx, MP_WHEEL_LEFT | mpmod, -val);
        break;
    }
}

static const struct wl_pointer_listener pointer_listener = {
    pointer_handle_enter,
    pointer_handle_leave,
    pointer_handle_motion,
    pointer_handle_button,
    pointer_handle_axis,
};

static void touch_handle_down(void *data, struct wl_touch *wl_touch,
                              uint32_t serial, uint32_t time, struct wl_surface *surface,
                              int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct vo_wayland_state *wl = data;

    wl->mouse_x = wl_fixed_to_int(x_w) * wl->scaling;
    wl->mouse_y = wl_fixed_to_int(y_w) * wl->scaling;

    mp_input_set_mouse_pos(wl->vo->input_ctx, wl->mouse_x, wl->mouse_y);
    mp_input_put_key(wl->vo->input_ctx, MP_MBTN_LEFT | MP_KEY_STATE_DOWN);

    enum xdg_toplevel_resize_edge edge;
    if (check_for_resize(wl, x_w, y_w, wl->opts->edge_pixels_touch, &edge)) {
        xdg_toplevel_resize(wl->xdg_toplevel, wl->seat, serial, edge);
    } else {
        xdg_toplevel_move(wl->xdg_toplevel, wl->seat, serial);
    }
}

static void touch_handle_up(void *data, struct wl_touch *wl_touch,
                            uint32_t serial, uint32_t time, int32_t id)
{
    struct vo_wayland_state *wl = data;
    mp_input_put_key(wl->vo->input_ctx, MP_MBTN_LEFT | MP_KEY_STATE_UP);
}

static void touch_handle_motion(void *data, struct wl_touch *wl_touch,
                                uint32_t time, int32_t id, wl_fixed_t x_w, wl_fixed_t y_w)
{
    struct vo_wayland_state *wl = data;

    wl->mouse_x = wl_fixed_to_int(x_w) * wl->scaling;
    wl->mouse_y = wl_fixed_to_int(y_w) * wl->scaling;

    mp_input_set_mouse_pos(wl->vo->input_ctx, wl->mouse_x, wl->mouse_y);
}

static void touch_handle_frame(void *data, struct wl_touch *wl_touch)
{
}

static void touch_handle_cancel(void *data, struct wl_touch *wl_touch)
{
}

static const struct wl_touch_listener touch_listener = {
    touch_handle_down,
    touch_handle_up,
    touch_handle_motion,
    touch_handle_frame,
    touch_handle_cancel,
};

static void keyboard_handle_keymap(void *data, struct wl_keyboard *wl_keyboard,
                                   uint32_t format, int32_t fd, uint32_t size)
{
    struct vo_wayland_state *wl = data;
    char *map_str;

    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
        close(fd);
        return;
    }

    map_str = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map_str == MAP_FAILED) {
        close(fd);
        return;
    }

    wl->xkb_keymap = xkb_keymap_new_from_buffer(wl->xkb_context, map_str,
                                                strnlen(map_str, size),
                                                XKB_KEYMAP_FORMAT_TEXT_V1, 0);

    munmap(map_str, size);
    close(fd);

    if (!wl->xkb_keymap) {
        MP_ERR(wl, "failed to compile keymap\n");
        return;
    }

    wl->xkb_state = xkb_state_new(wl->xkb_keymap);
    if (!wl->xkb_state) {
        MP_ERR(wl, "failed to create XKB state\n");
        xkb_keymap_unref(wl->xkb_keymap);
        wl->xkb_keymap = NULL;
        return;
    }
}

static void keyboard_handle_enter(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *surface,
                                  struct wl_array *keys)
{
    struct vo_wayland_state *wl = data;
    wl->has_keyboard_input = true;
}

static void keyboard_handle_leave(void *data, struct wl_keyboard *wl_keyboard,
                                  uint32_t serial, struct wl_surface *surface)
{
    struct vo_wayland_state *wl = data;
    wl->has_keyboard_input = false;
}

static void keyboard_handle_key(void *data, struct wl_keyboard *wl_keyboard,
                                uint32_t serial, uint32_t time, uint32_t key,
                                uint32_t state)
{
    struct vo_wayland_state *wl = data;

    wl->keyboard_code = key + 8;
    xkb_keysym_t sym = xkb_state_key_get_one_sym(wl->xkb_state, wl->keyboard_code);

    state = state == WL_KEYBOARD_KEY_STATE_PRESSED ? MP_KEY_STATE_DOWN
                                                   : MP_KEY_STATE_UP;
    int mpmod = get_mods(wl);
    int mpkey = lookupkey(sym);
    if (mpkey) {
        mp_input_put_key(wl->vo->input_ctx, mpkey | state | mpmod);
    } else {
        char s[128];
        if (xkb_keysym_to_utf8(sym, s, sizeof(s)) > 0)
            mp_input_put_key_utf8(wl->vo->input_ctx, state | mpmod, bstr0(s));
    }
}

static void keyboard_handle_modifiers(void *data, struct wl_keyboard *wl_keyboard,
                                      uint32_t serial, uint32_t mods_depressed,
                                      uint32_t mods_latched, uint32_t mods_locked,
                                      uint32_t group)
{
    struct vo_wayland_state *wl = data;

    if (wl->xkb_state) {
        xkb_state_update_mask(wl->xkb_state, mods_depressed, mods_latched,
                              mods_locked, 0, 0, group);
    }
}

static void keyboard_handle_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
                                        int32_t rate, int32_t delay)
{
    struct vo_wayland_state *wl = data;
    if (wl->vo_opts->native_keyrepeat)
        mp_input_set_repeat_info(wl->vo->input_ctx, rate, delay);
}

static const struct wl_keyboard_listener keyboard_listener = {
    keyboard_handle_keymap,
    keyboard_handle_enter,
    keyboard_handle_leave,
    keyboard_handle_key,
    keyboard_handle_modifiers,
    keyboard_handle_repeat_info,
};

static void seat_handle_caps(void *data, struct wl_seat *seat,
                             enum wl_seat_capability caps)
{
    struct vo_wayland_state *wl = data;

    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !wl->pointer) {
        wl->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(wl->pointer, &pointer_listener, wl);
    } else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && wl->pointer) {
        wl_pointer_destroy(wl->pointer);
        wl->pointer = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !wl->keyboard) {
        wl->keyboard = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(wl->keyboard, &keyboard_listener, wl);
    } else if (!(caps & WL_SEAT_CAPABILITY_KEYBOARD) && wl->keyboard) {
        wl_keyboard_destroy(wl->keyboard);
        wl->keyboard = NULL;
    }

    if ((caps & WL_SEAT_CAPABILITY_TOUCH) && !wl->touch) {
        wl->touch = wl_seat_get_touch(seat);
        wl_touch_set_user_data(wl->touch, wl);
        wl_touch_add_listener(wl->touch, &touch_listener, wl);
    } else if (!(caps & WL_SEAT_CAPABILITY_TOUCH) && wl->touch) {
        wl_touch_destroy(wl->touch);
        wl->touch = NULL;
    }
}

static const struct wl_seat_listener seat_listener = {
    seat_handle_caps,
};

static void data_offer_handle_offer(void *data, struct wl_data_offer *offer,
                                    const char *mime_type)
{
    struct vo_wayland_state *wl = data;
    int score = mp_event_get_mime_type_score(wl->vo->input_ctx, mime_type);
    if (score > wl->dnd_mime_score) {
        wl->dnd_mime_score = score;
        talloc_free(wl->dnd_mime_type);
        wl->dnd_mime_type = talloc_strdup(wl, mime_type);
        MP_VERBOSE(wl, "Given DND offer with mime type %s\n", wl->dnd_mime_type);
    }
}

static void data_offer_source_actions(void *data, struct wl_data_offer *offer, uint32_t source_actions)
{
}

static void data_offer_action(void *data, struct wl_data_offer *wl_data_offer, uint32_t dnd_action)
{
    struct vo_wayland_state *wl = data;
    wl->dnd_action = dnd_action & WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY ?
                     DND_REPLACE : DND_APPEND;
    MP_VERBOSE(wl, "DND action is %s\n",
               wl->dnd_action == DND_REPLACE ? "DND_REPLACE" : "DND_APPEND");
}

static const struct wl_data_offer_listener data_offer_listener = {
    data_offer_handle_offer,
    data_offer_source_actions,
    data_offer_action,
};

static void data_device_handle_data_offer(void *data, struct wl_data_device *wl_ddev,
                                          struct wl_data_offer *id)
{
    struct vo_wayland_state *wl = data;
    if (wl->dnd_offer)
        wl_data_offer_destroy(wl->dnd_offer);

    wl->dnd_offer = id;
    wl_data_offer_add_listener(id, &data_offer_listener, wl);
}

static void data_device_handle_enter(void *data, struct wl_data_device *wl_ddev,
                                     uint32_t serial, struct wl_surface *surface,
                                     wl_fixed_t x, wl_fixed_t y,
                                     struct wl_data_offer *id)
{
    struct vo_wayland_state *wl = data;
    if (wl->dnd_offer != id) {
        MP_FATAL(wl, "DND offer ID mismatch!\n");
        return;
    }

    wl_data_offer_set_actions(id, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY |
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_MOVE,
                                  WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

    wl_data_offer_accept(id, serial, wl->dnd_mime_type);

    MP_VERBOSE(wl, "Accepting DND offer with mime type %s\n", wl->dnd_mime_type);
}

static void data_device_handle_leave(void *data, struct wl_data_device *wl_ddev)
{
    struct vo_wayland_state *wl = data;

    if (wl->dnd_offer) {
        if (wl->dnd_fd != -1)
            return;
        wl_data_offer_destroy(wl->dnd_offer);
        wl->dnd_offer = NULL;
    }

    MP_VERBOSE(wl, "Releasing DND offer with mime type %s\n", wl->dnd_mime_type);

    talloc_free(wl->dnd_mime_type);
    wl->dnd_mime_type = NULL;
    wl->dnd_mime_score = 0;
}

static void data_device_handle_motion(void *data, struct wl_data_device *wl_ddev,
                                      uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
    struct vo_wayland_state *wl = data;

    wl_data_offer_accept(wl->dnd_offer, time, wl->dnd_mime_type);
}

static void data_device_handle_drop(void *data, struct wl_data_device *wl_ddev)
{
    struct vo_wayland_state *wl = data;

    int pipefd[2];

    if (pipe2(pipefd, O_CLOEXEC) == -1) {
        MP_ERR(wl, "Failed to create dnd pipe!\n");
        return;
    }

    MP_VERBOSE(wl, "Receiving DND offer with mime %s\n", wl->dnd_mime_type);

    wl_data_offer_receive(wl->dnd_offer, wl->dnd_mime_type, pipefd[1]);
    close(pipefd[1]);

    wl->dnd_fd = pipefd[0];
}

static void data_device_handle_selection(void *data, struct wl_data_device *wl_ddev,
                                         struct wl_data_offer *id)
{
    struct vo_wayland_state *wl = data;

    if (wl->dnd_offer) {
        wl_data_offer_destroy(wl->dnd_offer);
        wl->dnd_offer = NULL;
        MP_VERBOSE(wl, "Received a new DND offer. Releasing the previous offer.\n");
    }

}

static const struct wl_data_device_listener data_device_listener = {
    data_device_handle_data_offer,
    data_device_handle_enter,
    data_device_handle_leave,
    data_device_handle_motion,
    data_device_handle_drop,
    data_device_handle_selection,
};

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t phys_width,
                                   int32_t phys_height, int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform)
{
    struct vo_wayland_output *output = data;
    output->make = talloc_strdup(output->wl, make);
    output->model = talloc_strdup(output->wl, model);
    output->geometry.x0 = x;
    output->geometry.y0 = y;
    output->phys_width = phys_width;
    output->phys_height = phys_height;
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width,
                               int32_t height, int32_t refresh)
{
    struct vo_wayland_output *output = data;

    /* Only save current mode */
    if (!(flags & WL_OUTPUT_MODE_CURRENT))
        return;

    output->geometry.x1 = width;
    output->geometry.y1 = height;
    output->flags = flags;
    output->refresh_rate = (double)refresh * 0.001;
}

static void output_handle_done(void *data, struct wl_output *wl_output)
{
    struct vo_wayland_output *o = data;
    struct vo_wayland_state *wl = o->wl;

    o->geometry.x1 += o->geometry.x0;
    o->geometry.y1 += o->geometry.y0;

    MP_VERBOSE(o->wl, "Registered output %s %s (0x%x):\n"
               "\tx: %dpx, y: %dpx\n"
               "\tw: %dpx (%dmm), h: %dpx (%dmm)\n"
               "\tscale: %d\n"
               "\tHz: %f\n", o->make, o->model, o->id, o->geometry.x0,
               o->geometry.y0, mp_rect_w(o->geometry), o->phys_width,
               mp_rect_h(o->geometry), o->phys_height, o->scale, o->refresh_rate);

    /* If we satisfy this conditional, something about the current
     * output must have changed (resolution, scale, etc). All window
     * geometry and scaling should be recalculated. */
    if (wl->current_output && wl->current_output->output == wl_output) {
        set_surface_scaling(wl);
        spawn_cursor(wl);
        set_geometry(wl);
        wl->pending_vo_events |= VO_EVENT_DPI;
        wl->pending_vo_events |= VO_EVENT_RESIZE;
    }

    wl->pending_vo_events |= VO_EVENT_WIN_STATE;
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t factor)
{
    struct vo_wayland_output *output = data;
    if (!factor) {
        MP_ERR(output->wl, "Invalid output scale given by the compositor!\n");
        return;
    }
    output->scale = factor;
}

#ifdef HAVE_WAYLAND_1_20
static void output_handle_name(void *data, struct wl_output *wl_output,
                               const char *name)
{
    struct vo_wayland_output *output = data;
    output->name = talloc_strdup(output->wl, name);
}

static void output_handle_description(void *data, struct wl_output *wl_output,
                                      const char *description)
{
}
#endif

static const struct wl_output_listener output_listener = {
    output_handle_geometry,
    output_handle_mode,
    output_handle_done,
    output_handle_scale,
#ifdef HAVE_WAYLAND_1_20
    output_handle_name,
    output_handle_description,
#endif
};

static void surface_handle_enter(void *data, struct wl_surface *wl_surface,
                                 struct wl_output *output)
{
    struct vo_wayland_state *wl = data;
    if (!wl->current_output)
        return;

    struct mp_rect old_output_geometry = wl->current_output->geometry;
    struct mp_rect old_geometry = wl->geometry;
    wl->current_output = NULL;

    struct vo_wayland_output *o;
    wl_list_for_each(o, &wl->output_list, link) {
        if (o->output == output) {
            wl->current_output = o;
            break;
        }
    }

    wl->current_output->has_surface = true;
    bool force_resize = false;

    if (wl->scaling != wl->current_output->scale) {
        set_surface_scaling(wl);
        spawn_cursor(wl);
        force_resize = true;
        wl->pending_vo_events |= VO_EVENT_DPI;
    }

    if (!mp_rect_equals(&old_output_geometry, &wl->current_output->geometry)) {
        set_geometry(wl);
        force_resize = true;
    }

    if (!mp_rect_equals(&old_geometry, &wl->geometry) || force_resize)
        wl->pending_vo_events |= VO_EVENT_RESIZE;

    MP_VERBOSE(wl, "Surface entered output %s %s (0x%x), scale = %i\n", o->make,
               o->model, o->id, wl->scaling);

    wl->pending_vo_events |= VO_EVENT_WIN_STATE;
}

static void surface_handle_leave(void *data, struct wl_surface *wl_surface,
                                 struct wl_output *output)
{
    struct vo_wayland_state *wl = data;

    struct vo_wayland_output *o;
    wl_list_for_each(o, &wl->output_list, link) {
        if (o->output == output) {
            o->has_surface = false;
            wl->pending_vo_events |= VO_EVENT_WIN_STATE;
            return;
        }
    }
}

static const struct wl_surface_listener surface_listener = {
    surface_handle_enter,
    surface_handle_leave,
};

static void xdg_wm_base_ping(void *data, struct xdg_wm_base *wm_base, uint32_t serial)
{
    xdg_wm_base_pong(wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    xdg_wm_base_ping,
};

static void handle_surface_config(void *data, struct xdg_surface *surface,
                                  uint32_t serial)
{
    xdg_surface_ack_configure(surface, serial);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    handle_surface_config,
};

static void handle_toplevel_config(void *data, struct xdg_toplevel *toplevel,
                                   int32_t width, int32_t height, struct wl_array *states)
{
    struct vo_wayland_state *wl = data;
    struct mp_vo_opts *vo_opts = wl->vo_opts;
    struct mp_rect old_geometry = wl->geometry;

    int old_toplevel_width = wl->toplevel_width;
    int old_toplevel_height = wl->toplevel_height;
    wl->toplevel_width = width;
    wl->toplevel_height = height;

    /* Don't do anything here if we haven't finished setting geometry. */
    if (mp_rect_w(wl->geometry) == 0 || mp_rect_h(wl->geometry) == 0)
        return;

    bool is_maximized = false;
    bool is_fullscreen = false;
    bool is_activated = false;
    enum xdg_toplevel_state *state;
    wl_array_for_each(state, states) {
        switch (*state) {
        case XDG_TOPLEVEL_STATE_FULLSCREEN:
            is_fullscreen = true;
            break;
        case XDG_TOPLEVEL_STATE_RESIZING:
            break;
        case XDG_TOPLEVEL_STATE_ACTIVATED:
            is_activated = true;
            /*
             * If we get an ACTIVATED state, we know it cannot be
             * minimized, but it may not have been minimized
             * previously, so we can't detect the exact state.
             */
            vo_opts->window_minimized = false;
            m_config_cache_write_opt(wl->vo_opts_cache,
                                     &vo_opts->window_minimized);
            break;
        case XDG_TOPLEVEL_STATE_TILED_TOP:
        case XDG_TOPLEVEL_STATE_TILED_LEFT:
        case XDG_TOPLEVEL_STATE_TILED_RIGHT:
        case XDG_TOPLEVEL_STATE_TILED_BOTTOM:
        case XDG_TOPLEVEL_STATE_MAXIMIZED:
            is_maximized = true;
            break;
        }
    }

    if (vo_opts->fullscreen != is_fullscreen) {
        wl->state_change = true;
        vo_opts->fullscreen = is_fullscreen;
        m_config_cache_write_opt(wl->vo_opts_cache, &vo_opts->fullscreen);
    }

    if (vo_opts->window_maximized != is_maximized) {
        wl->state_change = true;
        vo_opts->window_maximized = is_maximized;
        m_config_cache_write_opt(wl->vo_opts_cache, &vo_opts->window_maximized);
    }

    if (wl->requested_decoration)
        request_decoration_mode(wl, wl->requested_decoration);

    if (wl->activated != is_activated) {
        wl->activated = is_activated;
        if ((!wl->focused && wl->activated && wl->has_keyboard_input) ||
            (wl->focused && !wl->activated))
        {
            wl->focused = !wl->focused;
            wl->pending_vo_events |= VO_EVENT_FOCUS;
        }
        /* Just force a redraw to be on the safe side. */
        if (wl->activated) {
            wl->hidden = false;
            wl->pending_vo_events |= VO_EVENT_EXPOSE;
        }
    }

    if (wl->state_change) {
        if (!is_fullscreen && !is_maximized) {
            wl->geometry = wl->window_size;
            wl->state_change = false;
            goto resize;
        }
    }

    /* Reuse old size if either of these are 0. */
    if (width == 0 || height == 0) {
        if (!is_fullscreen && !is_maximized) {
            wl->geometry = wl->window_size;
        }
        goto resize;
    }

    if (old_toplevel_width == wl->toplevel_width &&
        old_toplevel_height == wl->toplevel_height)
        return;

    if (!is_fullscreen && !is_maximized) {
        if (vo_opts->keepaspect) {
            double scale_factor = (double)width / wl->reduced_width;
            width = ceil(wl->reduced_width * scale_factor);
            if (vo_opts->keepaspect_window)
                height = ceil(wl->reduced_height * scale_factor);
        }
        wl->window_size.x0 = 0;
        wl->window_size.y0 = 0;
        wl->window_size.x1 = width;
        wl->window_size.y1 = height;
    }
    wl->geometry.x0 = 0;
    wl->geometry.y0 = 0;
    wl->geometry.x1 = width;
    wl->geometry.y1 = height;

    if (mp_rect_equals(&old_geometry, &wl->geometry))
        return;

resize:
    MP_VERBOSE(wl, "Resizing due to xdg from %ix%i to %ix%i\n",
               mp_rect_w(old_geometry)*wl->scaling, mp_rect_h(old_geometry)*wl->scaling,
               mp_rect_w(wl->geometry)*wl->scaling, mp_rect_h(wl->geometry)*wl->scaling);

    wl->pending_vo_events |= VO_EVENT_RESIZE;
    wl->toplevel_configured = true;
}

static void handle_toplevel_close(void *data, struct xdg_toplevel *xdg_toplevel)
{
    struct vo_wayland_state *wl = data;
    mp_input_put_key(wl->vo->input_ctx, MP_KEY_CLOSE_WIN);
}

#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
static void handle_configure_bounds(void *data, struct xdg_toplevel *xdg_toplevel,
                                    int32_t width, int32_t height)
{
    struct vo_wayland_state *wl = data;
    wl->bounded_width = width;
    wl->bounded_height = height;
}
#endif

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    handle_toplevel_config,
    handle_toplevel_close,
#ifdef XDG_TOPLEVEL_CONFIGURE_BOUNDS_SINCE_VERSION
    handle_configure_bounds,
#endif
};

static const char *zxdg_decoration_mode_to_str(const uint32_t mode)
{
    switch (mode) {
    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE:
        return "server-side";
    case ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE:
        return "client-side";
    default:
        return "<unknown>";
    }
}

static void configure_decorations(void *data,
                                  struct zxdg_toplevel_decoration_v1 *xdg_toplevel_decoration,
                                  uint32_t mode)
{
    struct vo_wayland_state *wl = data;
    struct mp_vo_opts *opts = wl->vo_opts;

    if (wl->requested_decoration && mode != wl->requested_decoration) {
        MP_DBG(wl,
               "Requested %s decorations but compositor responded with %s. "
               "It is likely that compositor wants us to stay in a given mode.\n",
               zxdg_decoration_mode_to_str(wl->requested_decoration),
               zxdg_decoration_mode_to_str(mode));
    }

    wl->requested_decoration = 0;

    if (mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE) {
        MP_VERBOSE(wl, "Enabling server decorations\n");
    } else {
        MP_VERBOSE(wl, "Disabling server decorations\n");
    }
    opts->border = mode == ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
    m_config_cache_write_opt(wl->vo_opts_cache, &opts->border);
}

static const struct zxdg_toplevel_decoration_v1_listener decoration_listener = {
    configure_decorations,
};

static void pres_set_clockid(void *data, struct wp_presentation *pres,
                             uint32_t clockid)
{
    struct vo_wayland_state *wl = data;

    if (clockid == CLOCK_MONOTONIC || clockid == CLOCK_MONOTONIC_RAW)
        wl->use_present = true;
}

static const struct wp_presentation_listener pres_listener = {
    pres_set_clockid,
};

static void feedback_sync_output(void *data, struct wp_presentation_feedback *fback,
                               struct wl_output *output)
{
}

static void feedback_presented(void *data, struct wp_presentation_feedback *fback,
                              uint32_t tv_sec_hi, uint32_t tv_sec_lo,
                              uint32_t tv_nsec, uint32_t refresh_nsec,
                              uint32_t seq_hi, uint32_t seq_lo,
                              uint32_t flags)
{
    struct vo_wayland_state *wl = data;

    if (fback)
        wp_presentation_feedback_destroy(fback);

    if (!wl->use_present)
        return;

    wl->refresh_interval = (int64_t)refresh_nsec / 1000;

    // Very similar to oml_sync_control, in this case we assume that every
    // time the compositor receives feedback, a buffer swap has been already
    // been performed.
    //
    // Notes:
    //  - tv_sec_lo + tv_sec_hi is the equivalent of oml's ust
    //  - seq_lo + seq_hi is the equivalent of oml's msc
    //  - these values are updated everytime the compositor receives feedback.

    int64_t sec = (uint64_t) tv_sec_lo + ((uint64_t) tv_sec_hi << 32);
    int64_t ust = sec * 1000000LL + (uint64_t) tv_nsec / 1000;
    int64_t msc = (uint64_t) seq_lo + ((uint64_t) seq_hi << 32);
    present_update_sync_values(wl->present, ust, msc);
}

static void feedback_discarded(void *data, struct wp_presentation_feedback *fback)
{
    if (fback)
        wp_presentation_feedback_destroy(fback);
}

static const struct wp_presentation_feedback_listener feedback_listener = {
    feedback_sync_output,
    feedback_presented,
    feedback_discarded,
};

static const struct wl_callback_listener frame_listener;

static void frame_callback(void *data, struct wl_callback *callback, uint32_t time)
{
    struct vo_wayland_state *wl = data;

    if (callback)
        wl_callback_destroy(callback);

    wl->frame_callback = wl_surface_frame(wl->surface);
    wl_callback_add_listener(wl->frame_callback, &frame_listener, wl);

    if (wl->presentation) {
        wl->feedback = wp_presentation_feedback(wl->presentation, wl->surface);
        wp_presentation_feedback_add_listener(wl->feedback, &feedback_listener, wl);
    }

    wl->frame_wait = false;
    wl->hidden = false;
}

static const struct wl_callback_listener frame_listener = {
    frame_callback,
};

static void dmabuf_format(void *data, struct zwp_linux_dmabuf_v1 *zwp_linux_dmabuf,
                          uint32_t format)
{
    struct vo_wayland_state *wl = data;

    if (wl->drm_format_ct == wl->drm_format_ct_max) {
        wl->drm_format_ct_max *= 2;
        wl->drm_formats = talloc_realloc(wl, wl->drm_formats, int, wl->drm_format_ct_max);
    }

    wl->drm_formats[wl->drm_format_ct++] = format;
    MP_VERBOSE(wl, "%s is supported by the compositor.\n", mp_tag_str(format));
}

static const struct zwp_linux_dmabuf_v1_listener dmabuf_listener = {
    dmabuf_format
};

static void done(void *data,
                 struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1)
{
}

static void format_table(void *data,
                         struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                         int32_t fd,
                         uint32_t size)
{
    struct vo_wayland_state *wl = data;

    void *map = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (map != MAP_FAILED) {
        wl->format_map = map;
        wl->format_size = size;
    }
}

static void main_device(void *data,
                        struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                        struct wl_array *device)
{
}

static void tranche_done(void *data,
                         struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1)
{
}

static void tranche_target_device(void *data,
                                  struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                                  struct wl_array *device)
{
}

static void tranche_formats(void *data,
                            struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                            struct wl_array *indices)
{
}

static void tranche_flags(void *data,
                          struct zwp_linux_dmabuf_feedback_v1 *zwp_linux_dmabuf_feedback_v1,
                          uint32_t flags)
{
}

static const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    done,
    format_table,
    main_device,
    tranche_done,
    tranche_target_device,
    tranche_formats,
    tranche_flags,
};

static void registry_handle_add(void *data, struct wl_registry *reg, uint32_t id,
                                const char *interface, uint32_t ver)
{
    int found = 1;
    struct vo_wayland_state *wl = data;

    if (!strcmp(interface, wl_compositor_interface.name) && (ver >= 4) && found++) {
        wl->compositor = wl_registry_bind(reg, id, &wl_compositor_interface, 4);
        wl->surface = wl_compositor_create_surface(wl->compositor);
        wl->video_surface = wl_compositor_create_surface(wl->compositor);
        /* never accept input events on the video surface */
        struct wl_region *region = wl_compositor_create_region(wl->compositor);
        wl_surface_set_input_region(wl->video_surface, region);
        wl_region_destroy(region);
        wl->cursor_surface = wl_compositor_create_surface(wl->compositor);
        wl_surface_add_listener(wl->surface, &surface_listener, wl);
    }

    if (!strcmp(interface, wl_subcompositor_interface.name) && (ver >= 1) && found++) {
        wl->subcompositor = wl_registry_bind(reg, id, &wl_subcompositor_interface, 1);
    }

    if (!strcmp (interface, zwp_linux_dmabuf_v1_interface.name) && (ver >= 4) && found++) {
        wl->dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, 4);
        wl->dmabuf_feedback = zwp_linux_dmabuf_v1_get_default_feedback(wl->dmabuf);
        zwp_linux_dmabuf_feedback_v1_add_listener(wl->dmabuf_feedback, &dmabuf_feedback_listener, wl);
    }
    else if (!strcmp (interface, zwp_linux_dmabuf_v1_interface.name) && (ver >= 2) && found++) {
        wl->dmabuf = wl_registry_bind(reg, id, &zwp_linux_dmabuf_v1_interface, 2);
        zwp_linux_dmabuf_v1_add_listener(wl->dmabuf, &dmabuf_listener, wl);
        wl->drm_format_ct_max = 64;
        wl->drm_formats = talloc_array(wl, int, wl->drm_format_ct_max);
    }

    if (!strcmp (interface, wp_viewporter_interface.name) && (ver >= 1) && found++) {
       wl->viewporter = wl_registry_bind (reg, id, &wp_viewporter_interface, 1);
    }

    if (!strcmp(interface, wl_data_device_manager_interface.name) && (ver >= 3) && found++) {
        wl->dnd_devman = wl_registry_bind(reg, id, &wl_data_device_manager_interface, 3);
    }

    if (!strcmp(interface, wl_output_interface.name) && (ver >= 2) && found++) {
        struct vo_wayland_output *output = talloc_zero(wl, struct vo_wayland_output);

        output->wl     = wl;
        output->id     = id;
        output->scale  = 1;
        output->name   = "";

        ver = MPMIN(ver, 4); /* Cap at 4 in case new events are added later. */
        output->output = wl_registry_bind(reg, id, &wl_output_interface, ver);
        wl_output_add_listener(output->output, &output_listener, output);
        wl_list_insert(&wl->output_list, &output->link);
    }

    if (!strcmp(interface, wl_seat_interface.name) && found++) {
        wl->seat = wl_registry_bind(reg, id, &wl_seat_interface, 1);
        wl_seat_add_listener(wl->seat, &seat_listener, wl);
    }

    if (!strcmp(interface, wl_shm_interface.name) && found++) {
        wl->shm = wl_registry_bind(reg, id, &wl_shm_interface, 1);
    }

    if (!strcmp(interface, wp_presentation_interface.name) && found++) {
        wl->presentation = wl_registry_bind(reg, id, &wp_presentation_interface, 1);
        wp_presentation_add_listener(wl->presentation, &pres_listener, wl);
    }

    if (!strcmp(interface, xdg_wm_base_interface.name) && found++) {
        ver = MPMIN(ver, 4); /* Cap at 4 in case new events are added later. */
        wl->wm_base = wl_registry_bind(reg, id, &xdg_wm_base_interface, ver);
        xdg_wm_base_add_listener(wl->wm_base, &xdg_wm_base_listener, wl);
    }

    if (!strcmp(interface, zxdg_decoration_manager_v1_interface.name) && found++) {
        wl->xdg_decoration_manager = wl_registry_bind(reg, id, &zxdg_decoration_manager_v1_interface, 1);
    }

    if (!strcmp(interface, zwp_idle_inhibit_manager_v1_interface.name) && found++) {
        wl->idle_inhibit_manager = wl_registry_bind(reg, id, &zwp_idle_inhibit_manager_v1_interface, 1);
    }

    if (found > 1)
        MP_VERBOSE(wl, "Registered for protocol %s\n", interface);
}

static void registry_handle_remove(void *data, struct wl_registry *reg, uint32_t id)
{
    struct vo_wayland_state *wl = data;
    struct vo_wayland_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &wl->output_list, link) {
        if (output->id == id) {
            remove_output(output);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_handle_add,
    registry_handle_remove,
};

/* Static functions */
static void check_dnd_fd(struct vo_wayland_state *wl)
{
    if (wl->dnd_fd == -1)
        return;

    struct pollfd fdp = { wl->dnd_fd, POLLIN | POLLERR | POLLHUP, 0 };
    if (poll(&fdp, 1, 0) <= 0)
        return;

    if (fdp.revents & POLLIN) {
        ptrdiff_t offset = 0;
        size_t data_read = 0;
        const size_t chunk_size = 1;
        uint8_t *buffer = ta_zalloc_size(wl, chunk_size);
        if (!buffer)
            goto end;

        while ((data_read = read(wl->dnd_fd, buffer + offset, chunk_size)) > 0) {
            offset += data_read;
            buffer = ta_realloc_size(wl, buffer, offset + chunk_size);
            memset(buffer + offset, 0, chunk_size);
            if (!buffer)
                goto end;
        }

        MP_VERBOSE(wl, "Read %td bytes from the DND fd\n", offset);

        struct bstr file_list = bstr0(buffer);
        mp_event_drop_mime_data(wl->vo->input_ctx, wl->dnd_mime_type,
                                file_list, wl->dnd_action);
        talloc_free(buffer);
end:
        if (wl->dnd_mime_type)
            talloc_free(wl->dnd_mime_type);

        if (wl->dnd_action >= 0 && wl->dnd_offer)
            wl_data_offer_finish(wl->dnd_offer);

        wl->dnd_action = -1;
        wl->dnd_mime_type = NULL;
        wl->dnd_mime_score = 0;
    }

    if (fdp.revents & (POLLIN | POLLERR | POLLHUP)) {
        close(wl->dnd_fd);
        wl->dnd_fd = -1;
    }
}

static int check_for_resize(struct vo_wayland_state *wl, wl_fixed_t x_w, wl_fixed_t y_w,
                            int edge_pixels, enum xdg_toplevel_resize_edge *edge)
{
    if (wl->vo_opts->fullscreen || wl->vo_opts->window_maximized)
        return 0;

    int pos[2] = { wl_fixed_to_double(x_w), wl_fixed_to_double(y_w) };
    int left_edge   = pos[0] < edge_pixels;
    int top_edge    = pos[1] < edge_pixels;
    int right_edge  = pos[0] > (mp_rect_w(wl->geometry) - edge_pixels);
    int bottom_edge = pos[1] > (mp_rect_h(wl->geometry) - edge_pixels);

    if (left_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_LEFT;
        if (top_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT;
        else if (bottom_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT;
    } else if (right_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT;
        if (top_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT;
        else if (bottom_edge)
            *edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT;
    } else if (top_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_TOP;
    } else if (bottom_edge) {
        *edge = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM;
    } else {
        *edge = 0;
        return 0;
    }

    return 1;
}

static bool create_input(struct vo_wayland_state *wl)
{
    wl->xkb_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);

    if (!wl->xkb_context) {
        MP_ERR(wl, "failed to initialize input: check xkbcommon\n");
        return 1;
    }

    return 0;
}

static int create_xdg_surface(struct vo_wayland_state *wl)
{
    wl->xdg_surface = xdg_wm_base_get_xdg_surface(wl->wm_base, wl->surface);
    xdg_surface_add_listener(wl->xdg_surface, &xdg_surface_listener, wl);

    wl->xdg_toplevel = xdg_surface_get_toplevel(wl->xdg_surface);
    xdg_toplevel_add_listener(wl->xdg_toplevel, &xdg_toplevel_listener, wl);

    if (!wl->xdg_surface || !wl->xdg_toplevel)
        return 1;
    return 0;
}

static void do_minimize(struct vo_wayland_state *wl)
{
    if (!wl->xdg_toplevel)
        return;
    if (wl->vo_opts->window_minimized)
        xdg_toplevel_set_minimized(wl->xdg_toplevel);
}

static char **get_displays_spanned(struct vo_wayland_state *wl)
{
    char **names = NULL;
    int displays_spanned = 0;
    struct vo_wayland_output *output;
    wl_list_for_each(output, &wl->output_list, link) {
        if (output->has_surface) {
            char *name = output->name ? output->name : output->model;
            MP_TARRAY_APPEND(NULL, names, displays_spanned,
                             talloc_strdup(NULL, name));
        }
    }
    MP_TARRAY_APPEND(NULL, names, displays_spanned, NULL);
    return names;
}

static int get_mods(struct vo_wayland_state *wl)
{
    static char* const mod_names[] = {
        XKB_MOD_NAME_SHIFT,
        XKB_MOD_NAME_CTRL,
        XKB_MOD_NAME_ALT,
        XKB_MOD_NAME_LOGO,
    };

    static const int mods[] = {
        MP_KEY_MODIFIER_SHIFT,
        MP_KEY_MODIFIER_CTRL,
        MP_KEY_MODIFIER_ALT,
        MP_KEY_MODIFIER_META,
    };

    int modifiers = 0;

    for (int n = 0; n < MP_ARRAY_SIZE(mods); n++) {
        xkb_mod_index_t index = xkb_keymap_mod_get_index(wl->xkb_keymap, mod_names[n]);
        if (!xkb_state_mod_index_is_consumed(wl->xkb_state, wl->keyboard_code, index)
            && xkb_state_mod_index_is_active(wl->xkb_state, index,
                                             XKB_STATE_MODS_DEPRESSED))
            modifiers |= mods[n];
    }
    return modifiers;
}

static void greatest_common_divisor(struct vo_wayland_state *wl, int a, int b) {
    // euclidean algorithm
    int larger;
    int smaller;
    if (a > b) {
        larger = a;
        smaller = b;
    } else {
        larger = b;
        smaller = a;
    }
    int remainder = larger - smaller * floor(larger/smaller);
    if (remainder == 0) {
        wl->gcd = smaller;
    } else {
        greatest_common_divisor(wl, smaller, remainder);
    }
}

static struct vo_wayland_output *find_output(struct vo_wayland_state *wl)
{
    int index = 0;
    int screen_id = wl->vo_opts->fsscreen_id;
    char *screen_name = wl->vo_opts->fsscreen_name;
    struct vo_wayland_output *output = NULL;
    struct vo_wayland_output *fallback_output = NULL;
    wl_list_for_each(output, &wl->output_list, link) {
        if (index == 0)
            fallback_output = output;
        if (screen_id == -1 && !screen_name)
            return output;
        if (screen_id == -1 && screen_name && !strcmp(screen_name, output->name))
            return output;
        if (screen_id == -1 && screen_name && !strcmp(screen_name, output->model))
            return output;
        if (screen_id == index++)
            return output;
    }
    if (!fallback_output) {
        MP_ERR(wl, "No screens could be found!\n");
        return NULL;
    } else if (wl->vo_opts->fsscreen_id >= 0) {
        MP_WARN(wl, "Screen index %i not found/unavailable! Falling back to screen 0!\n", screen_id);
    } else if (wl->vo_opts->fsscreen_name) {
        MP_WARN(wl, "Screen name %s not found/unavailable! Falling back to screen 0!\n", screen_name);
    }
    return fallback_output;
}

static int lookupkey(int key)
{
    const char *passthrough_keys = " -+*/<>`~!@#$%^&()_{}:;\"\',.?\\|=[]";

    int mpkey = 0;
    if ((key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') ||
        (key >= '0' && key <= '9') ||
        (key >  0   && key <  256 && strchr(passthrough_keys, key)))
        mpkey = key;

    if (!mpkey)
        mpkey = lookup_keymap_table(keymap, key);

    return mpkey;
}

static void request_decoration_mode(struct vo_wayland_state *wl, uint32_t mode)
{
    wl->requested_decoration = mode;
    zxdg_toplevel_decoration_v1_set_mode(wl->xdg_toplevel_decoration, mode);
}

static void remove_output(struct vo_wayland_output *out)
{
    if (!out)
        return;

    MP_VERBOSE(out->wl, "Deregistering output %s %s (0x%x)\n", out->make,
               out->model, out->id);
    wl_list_remove(&out->link);
    talloc_free(out->make);
    talloc_free(out->model);
    talloc_free(out);
    return;
}

static int set_cursor_visibility(struct vo_wayland_state *wl, bool on)
{
    wl->cursor_visible = on;
    if (on) {
        if (spawn_cursor(wl))
            return VO_FALSE;
        struct wl_cursor_image *img = wl->default_cursor->images[0];
        struct wl_buffer *buffer = wl_cursor_image_get_buffer(img);
        if (!buffer)
            return VO_FALSE;
        wl_pointer_set_cursor(wl->pointer, wl->pointer_id, wl->cursor_surface,
                              img->hotspot_x/wl->scaling, img->hotspot_y/wl->scaling);
        wl_surface_set_buffer_scale(wl->cursor_surface, wl->scaling);
        wl_surface_attach(wl->cursor_surface, buffer, 0, 0);
        wl_surface_damage_buffer(wl->cursor_surface, 0, 0, img->width, img->height);
        wl_surface_commit(wl->cursor_surface);
    } else {
        wl_pointer_set_cursor(wl->pointer, wl->pointer_id, NULL, 0, 0);
    }
    return VO_TRUE;
}

static void set_geometry(struct vo_wayland_state *wl)
{
    struct vo *vo = wl->vo;
    assert(wl->current_output);

    struct vo_win_geometry geo;
    struct mp_rect screenrc = wl->current_output->geometry;
    vo_calc_window_geometry(vo, &screenrc, &geo);
    vo_apply_window_geometry(vo, &geo);

    greatest_common_divisor(wl, vo->dwidth, vo->dheight);
    wl->reduced_width = vo->dwidth / wl->gcd;
    wl->reduced_height = vo->dheight / wl->gcd;

    wl->vdparams.x0 = 0;
    wl->vdparams.y0 = 0;
    wl->vdparams.x1 = vo->dwidth / wl->scaling;
    wl->vdparams.y1 = vo->dheight / wl->scaling;
}

static int set_screensaver_inhibitor(struct vo_wayland_state *wl, int state)
{
    if (!wl->idle_inhibit_manager)
        return VO_NOTIMPL;
    if (state == (!!wl->idle_inhibitor))
        return VO_TRUE;
    if (state) {
        MP_VERBOSE(wl, "Enabling idle inhibitor\n");
        struct zwp_idle_inhibit_manager_v1 *mgr = wl->idle_inhibit_manager;
        wl->idle_inhibitor = zwp_idle_inhibit_manager_v1_create_inhibitor(mgr, wl->surface);
    } else {
        MP_VERBOSE(wl, "Disabling the idle inhibitor\n");
        zwp_idle_inhibitor_v1_destroy(wl->idle_inhibitor);
        wl->idle_inhibitor = NULL;
    }
    return VO_TRUE;
}

static void set_surface_scaling(struct vo_wayland_state *wl)
{
    bool dmabuf_wayland = !strcmp(wl->vo->driver->name, "dmabuf-wayland");
    int old_scale = wl->scaling;
    if (wl->vo_opts->hidpi_window_scale && !dmabuf_wayland) {
        wl->scaling = wl->current_output->scale;
    } else {
        wl->scaling = 1;
    }

    double factor = (double)old_scale / wl->scaling;
    wl->vdparams.x1 *= factor;
    wl->vdparams.y1 *= factor;
    wl->window_size.x1 *= factor;
    wl->window_size.y1 *= factor;
    wl_surface_set_buffer_scale(wl->surface, wl->scaling);
}

static void set_window_bounds(struct vo_wayland_state *wl)
{
    if (wl->bounded_width && wl->bounded_width < wl->window_size.x1)
        wl->window_size.x1 = wl->bounded_width;
    if (wl->bounded_height && wl->bounded_height < wl->window_size.y1)
        wl->window_size.y1 = wl->bounded_height;
}

static int spawn_cursor(struct vo_wayland_state *wl)
{
    /* Reuse if size is identical */
    if (!wl->pointer || wl->allocated_cursor_scale == wl->scaling)
        return 0;
    else if (wl->cursor_theme)
        wl_cursor_theme_destroy(wl->cursor_theme);

    const char *xcursor_theme = getenv("XCURSOR_THEME");
    const char *size_str = getenv("XCURSOR_SIZE");
    int size = 24;
    if (size_str != NULL) {
        errno = 0;
        char *end;
        long size_long = strtol(size_str, &end, 10);
        if (!*end && !errno && size_long > 0 && size_long <= INT_MAX)
            size = (int)size_long;
    }

    wl->cursor_theme = wl_cursor_theme_load(xcursor_theme, size*wl->scaling, wl->shm);
    if (!wl->cursor_theme) {
        MP_ERR(wl, "Unable to load cursor theme!\n");
        return 1;
    }

    wl->default_cursor = wl_cursor_theme_get_cursor(wl->cursor_theme, "left_ptr");
    if (!wl->default_cursor) {
        MP_ERR(wl, "Unable to load cursor theme!\n");
        return 1;
    }

    wl->allocated_cursor_scale = wl->scaling;

    return 0;
}

static void toggle_fullscreen(struct vo_wayland_state *wl)
{
    if (!wl->xdg_toplevel)
        return;
    wl->state_change = true;
    bool specific_screen = wl->vo_opts->fsscreen_id >= 0 || wl->vo_opts->fsscreen_name;
    if (wl->vo_opts->fullscreen && !specific_screen) {
        xdg_toplevel_set_fullscreen(wl->xdg_toplevel, NULL);
    } else if (wl->vo_opts->fullscreen && specific_screen) {
        struct vo_wayland_output *output = find_output(wl);
        xdg_toplevel_set_fullscreen(wl->xdg_toplevel, output->output);
    } else {
        xdg_toplevel_unset_fullscreen(wl->xdg_toplevel);
    }
}

static void toggle_maximized(struct vo_wayland_state *wl)
{
    if (!wl->xdg_toplevel)
        return;
    wl->state_change = true;
    if (wl->vo_opts->window_maximized) {
        xdg_toplevel_set_maximized(wl->xdg_toplevel);
    } else {
        xdg_toplevel_unset_maximized(wl->xdg_toplevel);
    }
}

static void update_app_id(struct vo_wayland_state *wl)
{
    if (!wl->xdg_toplevel)
        return;
    xdg_toplevel_set_app_id(wl->xdg_toplevel, wl->vo_opts->appid);
}

static int update_window_title(struct vo_wayland_state *wl, const char *title)
{
    if (!wl->xdg_toplevel)
        return VO_NOTAVAIL;
    /* The xdg-shell protocol requires that the title is UTF-8. */
    void *tmp = talloc_new(NULL);
    struct bstr b_title = bstr_sanitize_utf8_latin1(tmp, bstr0(title));
    xdg_toplevel_set_title(wl->xdg_toplevel, b_title.start);
    talloc_free(tmp);
    return VO_TRUE;
}

static void window_move(struct vo_wayland_state *wl, uint32_t serial)
{
    if (wl->xdg_toplevel)
        xdg_toplevel_move(wl->xdg_toplevel, wl->seat, serial);
}

static void vo_wayland_dispatch_events(struct vo_wayland_state *wl, int nfds, int timeout)
{
    struct pollfd fds[2] = {
        {.fd = wl->display_fd,     .events = POLLIN },
        {.fd = wl->wakeup_pipe[0], .events = POLLIN },
    };

    while (wl_display_prepare_read(wl->display) != 0)
        wl_display_dispatch_pending(wl->display);
    wl_display_flush(wl->display);

    poll(fds, nfds, timeout);

    if (fds[0].revents & POLLIN) {
        wl_display_read_events(wl->display);
    } else {
        wl_display_cancel_read(wl->display);
    }

    if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
        MP_FATAL(wl, "Error occurred on the display fd, closing\n");
        close(wl->display_fd);
        wl->display_fd = -1;
        mp_input_put_key(wl->vo->input_ctx, MP_KEY_CLOSE_WIN);
    }

    if (fds[1].revents & POLLIN)
        mp_flush_wakeup_pipe(wl->wakeup_pipe[0]);

    wl_display_dispatch_pending(wl->display);
}

/* Non-static */
int vo_wayland_allocate_memfd(struct vo *vo, size_t size)
{
#if !HAVE_MEMFD_CREATE
    return VO_ERROR;
#else
    int fd = memfd_create("mpv", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) {
        MP_ERR(vo, "Failed to allocate memfd: %s\n", mp_strerror(errno));
        return VO_ERROR;
    }

    fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_SEAL);

    if (posix_fallocate(fd, 0, size) == 0)
        return fd;

    close(fd);
    MP_ERR(vo, "Failed to allocate memfd: %s\n", mp_strerror(errno));

    return VO_ERROR;
#endif
}

bool vo_wayland_check_visible(struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wl;
    bool render = !wl->hidden || wl->opts->disable_vsync;
    wl->frame_wait = true;
    return render;
}

int vo_wayland_control(struct vo *vo, int *events, int request, void *arg)
{
    struct vo_wayland_state *wl = vo->wl;
    struct mp_vo_opts *opts = wl->vo_opts;
    wl_display_dispatch_pending(wl->display);

    switch (request) {
    case VOCTRL_CHECK_EVENTS: {
        check_dnd_fd(wl);
        *events |= wl->pending_vo_events;
        if (*events & VO_EVENT_RESIZE) {
            *events |= VO_EVENT_EXPOSE;
            wl->frame_wait = false;
            wl->timeout_count = 0;
            wl->hidden = false;
        }
        wl->pending_vo_events = 0;
        return VO_TRUE;
    }
    case VOCTRL_VO_OPTS_CHANGED: {
        void *opt;
        while (m_config_cache_get_next_changed(wl->vo_opts_cache, &opt)) {
            if (opt == &opts->appid)
                update_app_id(wl);
            if (opt == &opts->border)
            {
                // This is stupid but the value of border shouldn't be written
                // unless we get a configure event. Change it back to its old
                // value and let configure_decorations handle it after the request.
                if (wl->xdg_toplevel_decoration) {
                    int requested_border_mode = opts->border;
                    opts->border = !opts->border;
                    m_config_cache_write_opt(wl->vo_opts_cache,
                                             &opts->border);
                    request_decoration_mode(
                        wl, requested_border_mode ?
                            ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE :
                            ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
                } else {
                    opts->border = false;
                    m_config_cache_write_opt(wl->vo_opts_cache,
                                             &wl->vo_opts->border);
                }
            }
            if (opt == &opts->fullscreen)
                toggle_fullscreen(wl);
            if (opt == &opts->hidpi_window_scale)
                set_surface_scaling(wl);
            if (opt == &opts->window_maximized)
                toggle_maximized(wl);
            if (opt == &opts->window_minimized)
                do_minimize(wl);
            if (opt == &opts->geometry || opt == &opts->autofit ||
                opt == &opts->autofit_smaller || opt == &opts->autofit_larger)
            {
                if (wl->current_output) {
                    set_geometry(wl);
                    wl->window_size = wl->vdparams;
                    if (!wl->vo_opts->fullscreen && !wl->vo_opts->window_maximized)
                        wl->geometry = wl->window_size;
                    wl->pending_vo_events |= VO_EVENT_RESIZE;
                }
            }
        }
        return VO_TRUE;
    }
    case VOCTRL_GET_FOCUSED: {
        *(bool *)arg = wl->focused;
        return VO_TRUE;
    }
    case VOCTRL_GET_DISPLAY_NAMES: {
        *(char ***)arg = get_displays_spanned(wl);
        return VO_TRUE;
    }
    case VOCTRL_GET_UNFS_WINDOW_SIZE: {
        int *s = arg;
        if (wl->vo_opts->window_maximized) {
            s[0] = mp_rect_w(wl->geometry) * wl->scaling;
            s[1] = mp_rect_h(wl->geometry) * wl->scaling;
        } else {
            s[0] = mp_rect_w(wl->window_size) * wl->scaling;
            s[1] = mp_rect_h(wl->window_size) * wl->scaling;
        }
        return VO_TRUE;
    }
    case VOCTRL_SET_UNFS_WINDOW_SIZE: {
        int *s = arg;
        wl->window_size.x0 = 0;
        wl->window_size.y0 = 0;
        wl->window_size.x1 = s[0] / wl->scaling;
        wl->window_size.y1 = s[1] / wl->scaling;
        if (!wl->vo_opts->fullscreen) {
            if (wl->vo_opts->window_maximized) {
                xdg_toplevel_unset_maximized(wl->xdg_toplevel);
                wl_display_dispatch_pending(wl->display);
                /* Make sure the compositor let us unmaximize */
                if (wl->vo_opts->window_maximized)
                    return VO_TRUE;
            }
            wl->geometry = wl->window_size;
            wl->pending_vo_events |= VO_EVENT_RESIZE;
        }
        return VO_TRUE;
    }
    case VOCTRL_GET_DISPLAY_FPS: {
        if (!wl->current_output)
            return VO_NOTAVAIL;
        *(double *)arg = wl->current_output->refresh_rate;
        return VO_TRUE;
    }
    case VOCTRL_GET_DISPLAY_RES: {
        if (!wl->current_output)
            return VO_NOTAVAIL;
        ((int *)arg)[0] = wl->current_output->geometry.x1;
        ((int *)arg)[1] = wl->current_output->geometry.y1;
        return VO_TRUE;
    }
    case VOCTRL_GET_HIDPI_SCALE: {
        if (!wl->scaling)
            return VO_NOTAVAIL;
        *(double *)arg = wl->scaling;
        return VO_TRUE;
    }
    case VOCTRL_UPDATE_WINDOW_TITLE:
        return update_window_title(wl, (const char *)arg);
    case VOCTRL_SET_CURSOR_VISIBILITY:
        if (!wl->pointer)
            return VO_NOTAVAIL;
        return set_cursor_visibility(wl, *(bool *)arg);
    case VOCTRL_KILL_SCREENSAVER:
        return set_screensaver_inhibitor(wl, true);
    case VOCTRL_RESTORE_SCREENSAVER:
        return set_screensaver_inhibitor(wl, false);
    }

    return VO_NOTIMPL;
}

int vo_wayland_init(struct vo *vo)
{
    vo->wl = talloc_zero(NULL, struct vo_wayland_state);
    struct vo_wayland_state *wl = vo->wl;

    *wl = (struct vo_wayland_state) {
        .display = wl_display_connect(NULL),
        .vo = vo,
        .log = mp_log_new(wl, vo->log, "wayland"),
        .bounded_width = 0,
        .bounded_height = 0,
        .refresh_interval = 0,
        .scaling = 1,
        .wakeup_pipe = {-1, -1},
        .dnd_fd = -1,
        .cursor_visible = true,
        .vo_opts_cache = m_config_cache_alloc(wl, vo->global, &vo_sub_opts),
    };
    wl->vo_opts = wl->vo_opts_cache->opts;

    wl_list_init(&wl->output_list);

    if (!wl->display)
        return false;

    if (create_input(wl))
        return false;

    wl->registry = wl_display_get_registry(wl->display);
    wl_registry_add_listener(wl->registry, &registry_listener, wl);

    /* Do a roundtrip to run the registry */
    wl_display_roundtrip(wl->display);

    if (!wl->wm_base) {
        MP_FATAL(wl, "Compositor doesn't support the required %s protocol!\n",
                 xdg_wm_base_interface.name);
        return false;
    }

    if (!wl_list_length(&wl->output_list)) {
        MP_FATAL(wl, "No outputs found or compositor doesn't support %s (ver. 2)\n",
                 wl_output_interface.name);
        return false;
    }

    /* Can't be initialized during registry due to multi-protocol dependence */
    if (create_xdg_surface(wl))
        return false;

    if (wl->subcompositor) {
        wl->video_subsurface = wl_subcompositor_get_subsurface(wl->subcompositor, wl->video_surface, wl->surface);
        wl_subsurface_set_desync(wl->video_subsurface);
    }

    if (wl->viewporter) {
        wl->viewport = wp_viewporter_get_viewport(wl->viewporter, wl->surface);
        wl->video_viewport = wp_viewporter_get_viewport(wl->viewporter, wl->video_surface);
    }

    const char *xdg_current_desktop = getenv("XDG_CURRENT_DESKTOP");
    if (xdg_current_desktop != NULL && strstr(xdg_current_desktop, "GNOME"))
        MP_WARN(wl, "GNOME's wayland compositor lacks support for the idle inhibit protocol. This means the screen can blank during playback.\n");

    if (wl->dnd_devman && wl->seat) {
        wl->dnd_ddev = wl_data_device_manager_get_data_device(wl->dnd_devman, wl->seat);
        wl_data_device_add_listener(wl->dnd_ddev, &data_device_listener, wl);
    } else if (!wl->dnd_devman) {
        MP_VERBOSE(wl, "Compositor doesn't support the %s (ver. 3) protocol!\n",
                   wl_data_device_manager_interface.name);
    }

    if (wl->presentation) {
        wl->present = talloc_zero(wl, struct mp_present);
    } else {
        MP_VERBOSE(wl, "Compositor doesn't support the %s protocol!\n",
                   wp_presentation_interface.name);
    }

    if (wl->xdg_decoration_manager) {
        wl->xdg_toplevel_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(wl->xdg_decoration_manager, wl->xdg_toplevel);
        zxdg_toplevel_decoration_v1_add_listener(wl->xdg_toplevel_decoration, &decoration_listener, wl);
        request_decoration_mode(
            wl, wl->vo_opts->border ?
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE :
                ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
    } else {
        wl->vo_opts->border = false;
        m_config_cache_write_opt(wl->vo_opts_cache,
                                 &wl->vo_opts->border);
        MP_VERBOSE(wl, "Compositor doesn't support the %s protocol!\n",
                   zxdg_decoration_manager_v1_interface.name);
    }

    if (!wl->idle_inhibit_manager)
        MP_VERBOSE(wl, "Compositor doesn't support the %s protocol!\n",
                   zwp_idle_inhibit_manager_v1_interface.name);

    wl->opts = mp_get_config_group(wl, wl->vo->global, &wayland_conf);
    wl->display_fd = wl_display_get_fd(wl->display);

    update_app_id(wl);
    mp_make_wakeup_pipe(wl->wakeup_pipe);

    wl->frame_callback = wl_surface_frame(wl->surface);
    wl_callback_add_listener(wl->frame_callback, &frame_listener, wl);
    wl_surface_commit(wl->surface);

    /* Do another roundtrip to ensure all of the above is initialized
     * before mpv does anything else. */
    wl_display_roundtrip(wl->display);

    return true;
}

int vo_wayland_reconfig(struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wl;

    MP_VERBOSE(wl, "Reconfiguring!\n");

    if (!wl->current_output) {
        wl->current_output = find_output(wl);
        if (!wl->current_output)
            return false;
        set_surface_scaling(wl);
        wl->pending_vo_events |= VO_EVENT_DPI;
    }

    set_geometry(wl);
    wl->window_size = wl->vdparams;

    if (wl->opts->configure_bounds)
        set_window_bounds(wl);

    if ((!wl->vo_opts->fullscreen && !wl->vo_opts->window_maximized) ||
        mp_rect_w(wl->geometry) == 0 || mp_rect_h(wl->geometry) == 0)
    {
        wl->geometry = wl->window_size;
    }

    if (wl->vo_opts->fullscreen)
        toggle_fullscreen(wl);

    if (wl->vo_opts->window_maximized)
        toggle_maximized(wl);

    if (wl->vo_opts->window_minimized)
        do_minimize(wl);

    wl->pending_vo_events |= VO_EVENT_RESIZE;

    return true;
}

void vo_wayland_set_opaque_region(struct vo_wayland_state *wl, int alpha)
{
    const int32_t width = wl->scaling * mp_rect_w(wl->geometry);
    const int32_t height = wl->scaling * mp_rect_h(wl->geometry);
    if (!alpha) {
        struct wl_region *region = wl_compositor_create_region(wl->compositor);
        wl_region_add(region, 0, 0, width, height);
        wl_surface_set_opaque_region(wl->surface, region);
        wl_region_destroy(region);
    } else {
        wl_surface_set_opaque_region(wl->surface, NULL);
    }
}

bool vo_wayland_supported_format(struct vo *vo, uint32_t drm_format, uint64_t modifier)
{
    struct vo_wayland_state *wl = vo->wl;

    const struct {
        uint32_t format;
        uint32_t padding;
        uint64_t modifier;
    } *formats = wl->format_map;

    for (int i = 0; i < wl->format_size / 16; ++i) {
        if (drm_format == formats[i].format && modifier == formats[i].modifier)
            return true;
    }

    /* TODO: remove these once zwp_linux_dmabuf_v1 version 2 support is removed. */
    for (int i = 0; i < wl->drm_format_ct; ++i) {
        if (drm_format == wl->drm_formats[i])
            return true;
    }

    return false;
}

void vo_wayland_uninit(struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wl;
    if (!wl)
        return;

    mp_input_put_key(wl->vo->input_ctx, MP_INPUT_RELEASE_ALL);

    if (wl->compositor)
        wl_compositor_destroy(wl->compositor);

    if (wl->subcompositor)
        wl_subcompositor_destroy(wl->subcompositor);

    if (wl->current_output && wl->current_output->output)
        wl_output_destroy(wl->current_output->output);

    if (wl->cursor_surface)
        wl_surface_destroy(wl->cursor_surface);

    if (wl->cursor_theme)
        wl_cursor_theme_destroy(wl->cursor_theme);

    if (wl->dnd_ddev)
        wl_data_device_destroy(wl->dnd_ddev);

    if (wl->dnd_devman)
        wl_data_device_manager_destroy(wl->dnd_devman);

    if (wl->dnd_offer)
        wl_data_offer_destroy(wl->dnd_offer);

    if (wl->feedback)
        wp_presentation_feedback_destroy(wl->feedback);

    if (wl->frame_callback)
        wl_callback_destroy(wl->frame_callback);

    if (wl->idle_inhibitor)
        zwp_idle_inhibitor_v1_destroy(wl->idle_inhibitor);

    if (wl->idle_inhibit_manager)
        zwp_idle_inhibit_manager_v1_destroy(wl->idle_inhibit_manager);

    if (wl->keyboard)
        wl_keyboard_destroy(wl->keyboard);

    if (wl->pointer)
        wl_pointer_destroy(wl->pointer);

    if (wl->presentation)
        wp_presentation_destroy(wl->presentation);

    if (wl->registry)
        wl_registry_destroy(wl->registry);

    if (wl->viewporter)
        wp_viewporter_destroy (wl->viewporter);

    if (wl->viewport)
        wp_viewport_destroy (wl->viewport);

    if (wl->video_viewport)
        wp_viewport_destroy (wl->video_viewport);

    if (wl->dmabuf)
        zwp_linux_dmabuf_v1_destroy (wl->dmabuf);

    if (wl->seat)
        wl_seat_destroy(wl->seat);

    if (wl->shm)
        wl_shm_destroy(wl->shm);

    if (wl->surface)
        wl_surface_destroy(wl->surface);

    if (wl->video_surface)
        wl_surface_destroy(wl->video_surface);

    if (wl->video_subsurface)
        wl_subsurface_destroy(wl->video_subsurface);

    if (wl->wm_base)
        xdg_wm_base_destroy(wl->wm_base);

    if (wl->xdg_decoration_manager)
        zxdg_decoration_manager_v1_destroy(wl->xdg_decoration_manager);

    if (wl->xdg_toplevel)
        xdg_toplevel_destroy(wl->xdg_toplevel);

    if (wl->xdg_toplevel_decoration)
        zxdg_toplevel_decoration_v1_destroy(wl->xdg_toplevel_decoration);

    if (wl->xdg_surface)
        xdg_surface_destroy(wl->xdg_surface);

    if (wl->xkb_context)
        xkb_context_unref(wl->xkb_context);

    if (wl->xkb_keymap)
        xkb_keymap_unref(wl->xkb_keymap);

    if (wl->xkb_state)
        xkb_state_unref(wl->xkb_state);

    if (wl->display) {
        close(wl_display_get_fd(wl->display));
        wl_display_disconnect(wl->display);
    }

    munmap(wl->format_map, wl->format_size);

    struct vo_wayland_output *output, *tmp;
    wl_list_for_each_safe(output, tmp, &wl->output_list, link)
        remove_output(output);

    for (int n = 0; n < 2; n++)
        close(wl->wakeup_pipe[n]);
    talloc_free(wl);
    vo->wl = NULL;
}

void vo_wayland_wait_frame(struct vo_wayland_state *wl)
{
    int64_t vblank_time = 0;
    /* We need some vblank interval to use for the timeout in
     * this function. The order of preference of values to use is:
     * 1. vsync duration from presentation time
     * 2. refresh inteval reported by presentation time
     * 3. refresh rate of the output reported by the compositor
     * 4. make up crap if vblank_time is still <= 0 (better than nothing) */

    if (wl->use_present)
        vblank_time = wl->present->vsync_duration;

    if (vblank_time <= 0 && wl->refresh_interval > 0)
        vblank_time = wl->refresh_interval;

    if (vblank_time <= 0 && wl->current_output->refresh_rate > 0)
        vblank_time = 1e6 / wl->current_output->refresh_rate;

    // Ideally you should never reach this point.
    if (vblank_time <= 0)
        vblank_time = 1e6 / 60;

    // Completely arbitrary amount of additional time to wait.
    vblank_time += 0.05 * vblank_time;
    int64_t finish_time = mp_time_us() + vblank_time;

    while (wl->frame_wait && finish_time > mp_time_us()) {
        int poll_time = ceil((double)(finish_time - mp_time_us()) / 1000);
        if (poll_time < 0) {
            poll_time = 0;
        }
        vo_wayland_dispatch_events(wl, 1, poll_time);
    }

    /* If the compositor does not have presentation time, we cannot be sure
     * that this wait is accurate. Do a hacky block with wl_display_roundtrip. */
    if (!wl->use_present && !wl_display_get_error(wl->display))
        wl_display_roundtrip(wl->display);

    if (wl->frame_wait) {
        // Only consider consecutive missed callbacks.
        if (wl->timeout_count > 1) {
            wl->hidden = true;
            return;
        } else {
            wl->timeout_count += 1;
            return;
        }
    }

    wl->timeout_count = 0;
}

void vo_wayland_wait_events(struct vo *vo, int64_t until_time_us)
{
    struct vo_wayland_state *wl = vo->wl;

    if (wl->display_fd == -1)
        return;

    int64_t wait_us = until_time_us - mp_time_us();
    int timeout_ms = MPCLAMP((wait_us + 999) / 1000, 0, 10000);

    vo_wayland_dispatch_events(wl, 2, timeout_ms);
}

void vo_wayland_wakeup(struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wl;
    (void)write(wl->wakeup_pipe[1], &(char){0}, 1);
}
