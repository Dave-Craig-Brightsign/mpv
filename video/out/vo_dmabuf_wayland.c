/*
 * Based on vo_gl.c by Reimar Doeffinger.
 *
 * This file is part of mpv.
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
#include <unistd.h>

#include "config.h"

#if HAVE_VAAPI
#include <va/va_drmcommon.h>
#endif
#if HAVE_DRM
#include <libavutil/hwcontext_drm.h>
#endif

#include "mpv_talloc.h"
#include "common/global.h"
#include "vo.h"
#include "video/mp_image.h"

#include "gpu/hwdec.h"
#include "gpu/video.h"

#if HAVE_VAAPI
#include "video/vaapi.h"
#endif
#include "present_sync.h"
#include "wayland_common.h"
#include "generated/wayland/linux-dmabuf-unstable-v1.h"
#include "generated/wayland/viewporter.h"
#include "wlbuf_pool.h"

struct priv {
    struct mp_log *log;
    struct ra_ctx *ctx;
    struct mpv_global *global;
    struct ra_hwdec_ctx hwdec_ctx;
    int events;

    struct wl_shm_pool *solid_buffer_pool;
    struct wl_buffer *solid_buffer;
    struct wlbuf_pool *wlbuf_pool;
    bool want_reset;
    uint64_t reset_count;

#if HAVE_VAAPI
    VADisplay display;
#endif
};

#if HAVE_VAAPI
static uintptr_t vaapi_key_provider(struct mp_image *src)
{
    return va_surface_id(src);
}

/* va-api dmabuf importer */
static bool vaapi_dmabuf_importer(struct mp_image *src, struct wlbuf_pool_entry* entry,
                                  struct zwp_linux_buffer_params_v1 *params)
{
    struct priv *p = entry->vo->priv;
    VADRMPRIMESurfaceDescriptor desc;
    bool dmabuf_imported = false;
    /* composed has single layer */
    int layer_no = 0;
    VAStatus status = vaExportSurfaceHandle(p->display, entry->key, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                            VA_EXPORT_SURFACE_COMPOSED_LAYERS | VA_EXPORT_SURFACE_READ_ONLY, &desc);
    if (status == VA_STATUS_ERROR_INVALID_SURFACE) {
        MP_VERBOSE(entry->vo, "VA export to composed layers not supported.\n");
    } else if (!vo_wayland_supported_format(entry->vo, desc.layers[0].drm_format, desc.objects[0].drm_format_modifier)) {
        MP_VERBOSE(entry->vo, "%s(%016lx) is not supported.\n",
                   mp_tag_str(desc.layers[0].drm_format), desc.objects[0].drm_format_modifier);
    } else if (CHECK_VA_STATUS(entry->vo, "vaExportSurfaceHandle()")) {
        entry->drm_format = desc.layers[layer_no].drm_format;
        for (int plane_no = 0; plane_no < desc.layers[layer_no].num_planes; ++plane_no) {
            int object = desc.layers[layer_no].object_index[plane_no];
            uint64_t modifier = desc.objects[object].drm_format_modifier;
            zwp_linux_buffer_params_v1_add(params, desc.objects[object].fd, plane_no, desc.layers[layer_no].offset[plane_no],
                                           desc.layers[layer_no].pitch[plane_no], modifier >> 32, modifier & 0xffffffff);
        }
        dmabuf_imported = true;
    }

    /* clean up descriptor */
    if (status != VA_STATUS_ERROR_INVALID_SURFACE) {
        for (int i = 0; i < desc.num_objects; i++) {
            close(desc.objects[i].fd);
            desc.objects[i].fd = 0;
        }
    }

    return dmabuf_imported;
}
#endif

#if HAVE_DRM

static uintptr_t drmprime_key_provider(struct mp_image *src)
{
    struct AVDRMFrameDescriptor *desc = (AVDRMFrameDescriptor *)src->planes[0];

    AVDRMObjectDescriptor object = desc->objects[0];
    return (uintptr_t)object.fd;
}

static bool drmprime_dmabuf_importer(struct mp_image *src, struct wlbuf_pool_entry *entry,
                                     struct zwp_linux_buffer_params_v1 *params)
{
    int layer_no, plane_no;
    const AVDRMFrameDescriptor *avdesc = (AVDRMFrameDescriptor *)src->planes[0];

    for (layer_no = 0; layer_no < avdesc->nb_layers; layer_no++) {
        AVDRMLayerDescriptor layer = avdesc->layers[layer_no];

        entry->drm_format = layer.format;
        for (plane_no = 0; plane_no < layer.nb_planes; ++plane_no) {
            AVDRMPlaneDescriptor plane = layer.planes[plane_no];
            int object_index = plane.object_index;
            AVDRMObjectDescriptor object = avdesc->objects[object_index];
            uint64_t modifier = object.format_modifier;

            zwp_linux_buffer_params_v1_add(params, object.fd, plane_no, plane.offset,
                                           plane.pitch, modifier >> 32, modifier & 0xffffffff);
        }
    }

    return true;
}
#endif

static void resize(struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wl;
    struct mp_rect src;
    struct mp_rect dst;
    struct mp_osd_res osd;
    const int width = wl->scaling * mp_rect_w(wl->geometry);
    const int height = wl->scaling * mp_rect_h(wl->geometry);
    
    vo_wayland_set_opaque_region(wl, 0);
    vo->dwidth = width;
    vo->dheight = height;
    vo_get_src_dst_rects(vo, &src, &dst, &osd);

    if (wl->viewport)
        wp_viewport_set_destination(wl->viewport, 2 * dst.x0 + mp_rect_w(dst), 2 * dst.y0 + mp_rect_h(dst));

    if (wl->video_viewport)
        wp_viewport_set_destination(wl->video_viewport, mp_rect_w(dst), mp_rect_h(dst));
    wl_subsurface_set_position(wl->video_subsurface, dst.x0, dst.y0);
    vo->want_redraw = true;
}

static void draw_frame(struct vo *vo, struct vo_frame *frame)
{
    struct priv *p = vo->priv;
    struct vo_wayland_state *wl = vo->wl;
    struct wlbuf_pool_entry *entry;
    
    if (!vo_wayland_check_visible(vo))
        return;

    // ensure the pool is reset after hwdec seek,
    // to avoid stutter artifact
    p->reset_count++;
    if (p->want_reset &&  p->reset_count <= 2){
        wlbuf_pool_clean(p->wlbuf_pool);
        if (p->reset_count == 2)
            p->want_reset = false;
    }

    /* lazy initialization of buffer pool */
    if (!p->wlbuf_pool) {
#if HAVE_VAAPI
        p->display = (VADisplay)ra_get_native_resource(p->ctx->ra, "VADisplay");
        if (p->display)
            p->wlbuf_pool = wlbuf_pool_alloc(vo, wl, vaapi_key_provider, vaapi_dmabuf_importer);
#endif
#if HAVE_DRM
        if (!p->wlbuf_pool)
            p->wlbuf_pool = wlbuf_pool_alloc(vo, wl, drmprime_key_provider, drmprime_dmabuf_importer);
#endif
    }
    entry = wlbuf_pool_get_entry(p->wlbuf_pool, frame->current);
    if (!entry)
        return;

    MP_VERBOSE(entry->vo, "Schedule buffer pool entry : %lu\n",entry->key );
    wl_surface_attach(wl->video_surface, entry->buffer, 0, 0);
    wl_surface_damage_buffer(wl->video_surface, 0, 0, INT32_MAX, INT32_MAX);
}

static void flip_page(struct vo *vo)
{
    struct vo_wayland_state *wl = vo->wl;

    wl_surface_commit(wl->video_surface);
    wl_surface_commit(wl->surface);
    if (!wl->opts->disable_vsync)
        vo_wayland_wait_frame(wl);
    if (wl->use_present)
       present_sync_swap(wl->present);
}

static void get_vsync(struct vo *vo, struct vo_vsync_info *info)
{
    struct vo_wayland_state *wl = vo->wl;

    if (wl->use_present)
        present_sync_get_info(wl->present, info);
}

static bool is_supported_fmt(int fmt)
{
    return  (fmt == IMGFMT_DRMPRIME || fmt == IMGFMT_VAAPI);
}

static int query_format(struct vo *vo, int format)
{
    return  is_supported_fmt(format);
}

static int reconfig(struct vo *vo, struct mp_image_params *params)
{
    struct priv *p = vo->priv;
    struct vo_wayland_state *wl = vo->wl;

    if (!p->solid_buffer_pool) {
        int width = 1;
        int height = 1;
        int stride = MP_ALIGN_UP(width * 4, 16);
        int fd = vo_wayland_allocate_memfd(vo, stride);
        if (fd < 0)
            return VO_ERROR;
        p->solid_buffer_pool = wl_shm_create_pool(wl->shm, fd, height * stride);
        if (!p->solid_buffer_pool)
            return VO_ERROR;
        p->solid_buffer = wl_shm_pool_create_buffer(p->solid_buffer_pool, 0, width, height, stride, WL_SHM_FORMAT_XRGB8888);
        if (!p->solid_buffer)
            return VO_ERROR;
        wl_surface_attach(wl->surface, p->solid_buffer, 0, 0);
    }
    if (!vo_wayland_reconfig(vo))
        return VO_ERROR;

    return 0;
}

static void call_request_hwdec_api(void *ctx, struct hwdec_imgfmt_request *params)
{
    // Roundabout way to run hwdec loading on the VO thread.
    // Redirects to request_hwdec_api().
    vo_control(ctx, VOCTRL_LOAD_HWDEC_API, params);
}

static int control(struct vo *vo, uint32_t request, void *data)
{
    struct priv *p = vo->priv;
    int events = 0;
    int ret;

    switch (request) {
    case VOCTRL_LOAD_HWDEC_API:
        assert(p->hwdec_ctx.ra);
        struct hwdec_imgfmt_request* req = (struct hwdec_imgfmt_request*)data;
        if (!is_supported_fmt(req->imgfmt))
            return 0;
        ra_hwdec_ctx_load_fmt(&p->hwdec_ctx, vo->hwdec_devs, req);
        return (p->hwdec_ctx.num_hwdecs > 0);
        break;
	case VOCTRL_RESET:
        p->want_reset = true;
        p->reset_count = 0;
	    return VO_TRUE;
	    break;
    }

    ret = vo_wayland_control(vo, &events, request, data);
    if (events & VO_EVENT_RESIZE)
        resize(vo);
    if (events & VO_EVENT_EXPOSE)
        vo->want_redraw = true;
    vo_event(vo, events);

    return ret;
}

static void uninit(struct vo *vo)
{
    struct priv *p = vo->priv;

    if (p->solid_buffer_pool)
        wl_shm_pool_destroy(p->solid_buffer_pool);
    if (p->solid_buffer)
        wl_buffer_destroy(p->solid_buffer);
    ra_hwdec_ctx_uninit(&p->hwdec_ctx);
    if (vo->hwdec_devs) {
        hwdec_devices_set_loader(vo->hwdec_devs, NULL, NULL);
        hwdec_devices_destroy(vo->hwdec_devs);
    }
    wlbuf_pool_free(p->wlbuf_pool);
    vo_wayland_uninit(vo);
    ra_ctx_destroy(&p->ctx);
}

static int preinit(struct vo *vo)
{
    struct priv *p = vo->priv;

    p->log = vo->log;
    p->global = vo->global;
    p->ctx = ra_ctx_create_by_name(vo, "wldmabuf");
    if (!p->ctx)
       goto err_out;
    assert(p->ctx->ra);
    vo->hwdec_devs = hwdec_devices_create();
    hwdec_devices_set_loader(vo->hwdec_devs, call_request_hwdec_api, vo);
    assert(!p->hwdec_ctx.ra);
    p->hwdec_ctx = (struct ra_hwdec_ctx) {
        .log = p->log,
        .global = p->global,
        .ra = p->ctx->ra,
    };
    ra_hwdec_ctx_init(&p->hwdec_ctx, vo->hwdec_devs, NULL, true);

    return 0;
err_out:
    uninit(vo);

    return VO_ERROR;
}

const struct vo_driver video_out_dmabuf_wayland = {
    .description = "Wayland dmabuf video output",
    .name = "dmabuf-wayland",
    .preinit = preinit,
    .query_format = query_format,
    .reconfig = reconfig,
    .control = control,
    .draw_frame = draw_frame,
    .flip_page = flip_page,
    .get_vsync = get_vsync,
    .wakeup = vo_wayland_wakeup,
    .wait_events = vo_wayland_wait_events,
    .uninit = uninit,
    .priv_size = sizeof(struct priv),
};
