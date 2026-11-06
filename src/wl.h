/* Wayland side: layer-shell surfaces, shm buffers, pointer, compositor IPC. */
#ifndef NIBAR_WL_H
#define NIBAR_WL_H

#include <sys/mman.h>

static struct wl_display *dpy;
static struct wl_registry *registry;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static int layer_shell_ver;
static struct zdwl_ipc_manager_v2 *ipc_mgr;
static struct zofwc_ipc_v1 *ext;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static Output *ptr_out;

static int
shm_file(size_t size)
{
	int fd = -1;

#ifdef MFD_CLOEXEC
	fd = memfd_create("nibar", MFD_CLOEXEC);
#endif
	if (fd < 0) {
		char name[64];

		snprintf(name, sizeof(name), "/nibar-%d-%lu", (int)getpid(), (unsigned long)now_ms());
		fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
		if (fd < 0)
			return -1;
		shm_unlink(name);
		fcntl(fd, F_SETFD, FD_CLOEXEC);
	}
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static void
buf_release(void *data, struct wl_buffer *wl)
{
	ShmBuf *b = data;

	(void)wl;
	b->busy = 0;
	if (b->owner && b->owner->need_draw)
		bar_changed();
}

static const struct wl_buffer_listener buf_listener = {.release = buf_release};

static void
buf_destroy(ShmBuf *b)
{
	if (b->wl)
		wl_buffer_destroy(b->wl);
	if (b->wld)
		wld_buffer_unreference(b->wld);
	if (b->map)
		munmap(b->map, b->size);
	memset(b, 0, sizeof(*b));
}

static int
buf_create(ShmBuf *b, int w, int h)
{
	size_t stride = (size_t)w * 4, size = stride * (size_t)h;
	struct wl_shm_pool *pool;
	union wld_object obj;
	int fd;

	memset(b, 0, sizeof(*b));
	if (w < 1 || h < 1 || !shm || gfx_init() < 0)
		return -1;
	fd = shm_file(size);
	if (fd < 0)
		return -1;
	b->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (b->map == MAP_FAILED) {
		b->map = NULL;
		close(fd);
		return -1;
	}
	b->size = size;
	pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	close(fd);
	if (!pool) {
		buf_destroy(b);
		return -1;
	}
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	obj.ptr = b->map;
	b->wld = wld_import_buffer(gfx_ctx, WLD_OBJECT_DATA, obj, (uint32_t)w, (uint32_t)h,
	                           WLD_FORMAT_ARGB8888, (uint32_t)stride);
	if (!b->wl || !b->wld) {
		buf_destroy(b);
		return -1;
	}
	b->w = w;
	b->h = h;
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	return 0;
}

static int bars_ready;

static int
out_wants_bar(const Output *o)
{
	(void)o;
	return bars_ready && layer_shell && compositor && shm && cfg.enabled && cfg.height > 0;
}

static int
out_layer(const Output *o)
{
	if (o->fullscreen && cfg.layer > LAYER_BOTTOM && layer_shell_ver >= 2)
		return LAYER_BOTTOM;
	return cfg.layer;
}

static void
out_apply_geometry(Output *o, int want_w)
{
	int pt, pr, pb, pl;
	uint32_t edge = cfg.top ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;

	bar_pads(&pt, &pr, &pb, &pl);
	zwlr_layer_surface_v1_set_anchor(o->ls, o->narrowed ? edge
	                                 : edge | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
	                                       ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_size(o->ls, o->narrowed ? (uint32_t)want_w : 0,
	                               (uint32_t)cfg.height);
	zwlr_layer_surface_v1_set_margin(o->ls, pt, pr, pb, pl);
	zwlr_layer_surface_v1_set_exclusive_zone(o->ls, pt + cfg.height + pb);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
	    o->ls, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
}

static void out_draw(Output *o);

static void
out_destroy_surface(Output *o)
{
	int i;

	if (ptr_out == o)
		ptr_out = NULL;
	if (o->ls)
		zwlr_layer_surface_v1_destroy(o->ls);
	if (o->surf)
		wl_surface_destroy(o->surf);
	o->ls = NULL;
	o->surf = NULL;
	o->configured = o->narrowed = 0;
	o->hover_el = o->hover_btn = -1;
	for (i = 0; i < 2; i++)
		buf_destroy(&o->buf[i]);
}

static void
layer_configure(void *data, struct zwlr_layer_surface_v1 *ls, uint32_t serial, uint32_t w,
                uint32_t h)
{
	Output *o = data;

	if (ls != o->ls)
		return;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (!h)
		h = (uint32_t)MAX(1, cfg.height);
	if (!w)
		w = 800;
	w = MIN(w, 1u << 16);
	h = MIN(h, 1u << 12);
	if (cfg.width < 100 && !o->narrowed) {
		int want = MAX(1, (int)w * cfg.width / 100);

		if (want != (int)w) {
			o->narrowed = 1;
			o->configured = 0;
			out_apply_geometry(o, want);
			wl_surface_commit(o->surf);
			return;
		}
	}
	o->sw = (int)w;
	o->sh = (int)h;
	o->configured = 1;
	out_draw(o);
}

static void
layer_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	Output *o = data;

	if (ls != o->ls)
		return;
	nb_dbg("the compositor closed the bar of an output");
	out_destroy_surface(o);
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure,
	.closed = layer_closed,
};

static void
out_create_surface(Output *o)
{
	if (o->ls || !out_wants_bar(o) || o->hidden || !o->wl)
		return;
	o->surf = wl_compositor_create_surface(compositor);
	if (!o->surf)
		return;
	o->layer = out_layer(o);
	o->ls = zwlr_layer_shell_v1_get_layer_surface(layer_shell, o->surf, o->wl,
	                                              (uint32_t)o->layer, "nibar");
	if (!o->ls) {
		wl_surface_destroy(o->surf);
		o->surf = NULL;
		return;
	}
	zwlr_layer_surface_v1_add_listener(o->ls, &layer_listener, o);
	o->narrowed = o->configured = 0;
	o->hover_el = o->hover_btn = -1;
	out_apply_geometry(o, 0);
	wl_surface_commit(o->surf);
}

static void
out_regeometry(Output *o)
{
	if (!o->ls)
		return;
	o->narrowed = 0;
	out_apply_geometry(o, 0);
	wl_surface_commit(o->surf);
}

static void
out_update_layer(Output *o)
{
	int want;

	if (!o->ls)
		return;
	want = out_layer(o);
	if (want == o->layer || layer_shell_ver < 2)
		return;
	zwlr_layer_surface_v1_set_layer(o->ls, (uint32_t)want);
	o->layer = want;
	wl_surface_commit(o->surf);
}

static void
out_draw(Output *o)
{
	ShmBuf *b = NULL;
	int i, W = o->sw, H = o->sh;

	if (!o->ls || !o->surf || !o->configured || o->hidden || W < 1 || H < 1)
		return;
	for (i = 0; i < 2; i++)
		if (!o->buf[i].busy && o->buf[i].wl && o->buf[i].w == W && o->buf[i].h == H) {
			b = &o->buf[i];
			break;
		}
	if (!b) {
		for (i = 0; i < 2; i++)
			if (!o->buf[i].busy) {
				buf_destroy(&o->buf[i]);
				if (buf_create(&o->buf[i], W, H) == 0)
					b = &o->buf[i];
				break;
			}
	}
	if (!b) {
		o->need_draw = 1;
		return;
	}
	b->owner = o;
	o->need_draw = 0;
	if (!gfx_rend && gfx_init() < 0)
		return;
	memset(b->map, 0, b->size);
	if (!wld_set_target_buffer(gfx_rend, b->wld))
		return;
	bar_paint(o, W, H);
	wld_flush(gfx_rend);
	wl_surface_attach(o->surf, b->wl, 0, 0);
	wl_surface_damage(o->surf, 0, 0, W, H);
	wl_surface_commit(o->surf);
	b->busy = 1;
}

static void
bars_draw_all(void)
{
	Output *o;

	for (o = outputs; o; o = o->next)
		out_draw(o);
}

static void
ipco_toggle_visibility(void *data, struct zdwl_ipc_output_v2 *ipc)
{
	Output *o = data;

	(void)ipc;
	o->hidden = !o->hidden;
	if (o->hidden)
		out_destroy_surface(o);
	else
		out_create_surface(o);
	bar_changed();
}

static void
ipco_active(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t active)
{
	Output *o = data;

	(void)ipc;
	o->active = active != 0;
}

static void
ipco_tag(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t tag, uint32_t state,
         uint32_t clients, uint32_t focused)
{
	Output *o = data;

	(void)ipc;
	if (tag >= MAXTAGS)
		return;
	o->tstate[tag] = state;
	o->tclients[tag] = clients;
	o->tfocus[tag] = focused;
}

static void
ipco_layout(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t layout)
{
	(void)data;
	(void)ipc;
	(void)layout;
}

static void
set_str(char **dst, const char *s)
{
	free(*dst);
	*dst = estrdup(s ? s : "");
}

static void
ipco_title(void *data, struct zdwl_ipc_output_v2 *ipc, const char *title)
{
	(void)ipc;
	set_str(&((Output *)data)->title, title);
}

static void
ipco_appid(void *data, struct zdwl_ipc_output_v2 *ipc, const char *appid)
{
	(void)ipc;
	set_str(&((Output *)data)->appid, appid);
}

static void
ipco_layout_symbol(void *data, struct zdwl_ipc_output_v2 *ipc, const char *symbol)
{
	(void)ipc;
	set_str(&((Output *)data)->symbol, symbol);
}

static void
ipco_frame(void *data, struct zdwl_ipc_output_v2 *ipc)
{
	(void)data;
	(void)ipc;
	bar_changed();
}

static void
ipco_fullscreen(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t fs)
{
	Output *o = data;

	(void)ipc;
	o->fullscreen = fs != 0;
	out_update_layer(o);
}

static void
ipco_floating(void *data, struct zdwl_ipc_output_v2 *ipc, uint32_t fl)
{
	Output *o = data;

	(void)ipc;
	o->floating = fl != 0;
}

static const struct zdwl_ipc_output_v2_listener ipco_listener = {
	.toggle_visibility = ipco_toggle_visibility,
	.active = ipco_active,
	.tag = ipco_tag,
	.layout = ipco_layout,
	.title = ipco_title,
	.appid = ipco_appid,
	.layout_symbol = ipco_layout_symbol,
	.frame = ipco_frame,
	.fullscreen = ipco_fullscreen,
	.floating = ipco_floating,
};

static void
ipcm_tags(void *data, struct zdwl_ipc_manager_v2 *mgr, uint32_t amount)
{
	(void)data;
	(void)mgr;
	ipc_ntags = (int)MIN(amount, (uint32_t)MAXTAGS);
	bar_changed();
}

static void
ipcm_layout(void *data, struct zdwl_ipc_manager_v2 *mgr, const char *name)
{
	(void)data;
	(void)mgr;
	(void)name;
}

static const struct zdwl_ipc_manager_v2_listener ipcm_listener = {
	.tags = ipcm_tags,
	.layout = ipcm_layout,
};

static void
out_attach_ipc(Output *o)
{
	if (o->ipc || !ipc_mgr || !o->wl)
		return;
	o->ipc = zdwl_ipc_manager_v2_get_output(ipc_mgr, o->wl);
	if (o->ipc)
		zdwl_ipc_output_v2_add_listener(o->ipc, &ipco_listener, o);
}

static void
ext_keyboard_layouts(void *data, struct zofwc_ipc_v1 *x, const char *layouts)
{
	(void)data;
	(void)x;
	free(ext_layouts);
	ext_layouts = estrdup(layouts ? layouts : "");
	if (bar_refresh(0))
		bar_changed();
}

static void
ext_keyboard_layout(void *data, struct zofwc_ipc_v1 *x, uint32_t index)
{
	(void)data;
	(void)x;
	ext_index = index;
	if (bar_refresh(0))
		bar_changed();
}

static const struct zofwc_ipc_v1_listener ext_listener = {
	.keyboard_layouts = ext_keyboard_layouts,
	.keyboard_layout = ext_keyboard_layout,
};

static void
wlout_geometry(void *data, struct wl_output *wl, int32_t x, int32_t y, int32_t pw, int32_t ph,
               int32_t sub, const char *make, const char *model, int32_t transform)
{
	(void)data;
	(void)wl;
	(void)x;
	(void)y;
	(void)pw;
	(void)ph;
	(void)sub;
	(void)make;
	(void)model;
	(void)transform;
}

static void
wlout_mode(void *data, struct wl_output *wl, uint32_t flags, int32_t w, int32_t h,
           int32_t refresh)
{
	Output *o = data;

	(void)wl;
	(void)h;
	(void)refresh;
	if (!(flags & WL_OUTPUT_MODE_CURRENT))
		return;
	if (o->mode_w && o->mode_w != w && o->narrowed)
		out_regeometry(o);
	o->mode_w = w;
}

static void
wlout_done(void *data, struct wl_output *wl)
{
	(void)data;
	(void)wl;
}

static void
wlout_scale(void *data, struct wl_output *wl, int32_t factor)
{
	(void)data;
	(void)wl;
	(void)factor;
}

static const struct wl_output_listener wlout_listener = {
	.geometry = wlout_geometry,
	.mode = wlout_mode,
	.done = wlout_done,
	.scale = wlout_scale,
};

static void
output_add(uint32_t name, uint32_t version)
{
	Output *o = ecalloc(1, sizeof(*o));

	o->gname = name;
	o->wl = wl_registry_bind(registry, name, &wl_output_interface, MIN(version, 2u));
	o->hover_el = o->hover_btn = -1;
	o->title = estrdup("");
	o->appid = estrdup("");
	o->symbol = estrdup("");
	o->next = outputs;
	outputs = o;
	if (o->wl)
		wl_output_add_listener(o->wl, &wlout_listener, o);
}

static void
output_destroy(Output *o)
{
	Output **pp;

	out_destroy_surface(o);
	if (o->ipc)
		zdwl_ipc_output_v2_release(o->ipc);
	if (o->wl) {
		if (wl_output_get_version(o->wl) >= WL_OUTPUT_RELEASE_SINCE_VERSION)
			wl_output_release(o->wl);
		else
			wl_output_destroy(o->wl);
	}
	for (pp = &outputs; *pp; pp = &(*pp)->next)
		if (*pp == o) {
			*pp = o->next;
			break;
		}
	free(o->title);
	free(o->appid);
	free(o->symbol);
	free(o);
}

static void
outputs_ensure(void)
{
	Output *o;

	for (o = outputs; o; o = o->next) {
		out_attach_ipc(o);
		if (out_wants_bar(o))
			out_create_surface(o);
		else if (o->ls)
			out_destroy_surface(o);
	}
}

static void
outputs_reconfigure(void)
{
	Output *o;

	for (o = outputs; o; o = o->next) {
		if (!out_wants_bar(o)) {
			out_destroy_surface(o);
			continue;
		}
		if (o->ls) {
			out_update_layer(o);
			out_regeometry(o);
		} else {
			out_create_surface(o);
		}
		o->hover_el = o->hover_btn = -1;
	}
	bar_changed();
}

static Output *
output_of_surface(struct wl_surface *s)
{
	Output *o;

	for (o = outputs; o; o = o->next)
		if (o->surf && o->surf == s)
			return o;
	return NULL;
}

static void
ptr_enter(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
          wl_fixed_t sx, wl_fixed_t sy)
{
	(void)data;
	(void)p;
	(void)serial;
	(void)sy;
	ptr_out = output_of_surface(s);
	if (ptr_out) {
		int btn, el = bar_hit(ptr_out, wl_fixed_to_int(sx), &btn);

		ptr_out->hover_el = el;
		ptr_out->hover_btn = btn;
		ptr_out->hover_x = wl_fixed_to_int(sx);
		if (el >= 0 && bar_el[el].type == BE_TAGS)
			bar_changed();
	}
}

static void
ptr_leave(void *data, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	Output *o = output_of_surface(s);

	(void)data;
	(void)p;
	(void)serial;
	if (o) {
		if (o->hover_el >= 0 && o->hover_el < bar_nel && bar_el[o->hover_el].type == BE_TAGS)
			bar_changed();
		o->hover_el = o->hover_btn = -1;
	}
	if (ptr_out == o)
		ptr_out = NULL;
}

static void
ptr_motion(void *data, struct wl_pointer *p, uint32_t time, wl_fixed_t sx, wl_fixed_t sy)
{
	Output *o = ptr_out;
	int el, btn, was_tags, is_tags;

	(void)data;
	(void)p;
	(void)time;
	(void)sy;
	if (!o)
		return;
	o->hover_x = wl_fixed_to_int(sx);
	el = bar_hit(o, o->hover_x, &btn);
	if (el == o->hover_el && btn == o->hover_btn)
		return;
	was_tags = o->hover_el >= 0 && o->hover_el < bar_nel && bar_el[o->hover_el].type == BE_TAGS;
	is_tags = el >= 0 && bar_el[el].type == BE_TAGS;
	o->hover_el = el;
	o->hover_btn = btn;
	if (was_tags || is_tags)
		bar_changed();
}

static void
bar_run(Output *o, const char *cmd)
{
	if (!cmd || !*cmd)
		return;
	if (!strncmp(cmd, "ofwc:", 5)) {
		char *copy = estrdup(cmd + 5), *arg;

		arg = copy + strcspn(copy, " \t");
		if (*arg)
			*arg++ = '\0';
		while (*arg == ' ' || *arg == '\t')
			arg++;
		if (ext)
			zofwc_ipc_v1_action(ext, copy, *arg ? arg : NULL, o->wl);
		else
			nb_log("'%s' needs a compositor with ofwc-ipc", cmd);
		free(copy);
		return;
	}
	spawn_cmd(cmd);
}

static uint32_t
tags_active_mask(const Output *o)
{
	uint32_t m = 0;
	int t;

	for (t = 0; t < MAXTAGS; t++)
		if (tag_active(o, t))
			m |= 1u << t;
	return m;
}

static void
bar_click(Output *o, int x, uint32_t button)
{
	BarElem *e;
	int el, btn;
	const char *cmd;

	el = bar_hit(o, x, &btn);
	if (el < 0)
		return;
	e = &bar_el[el];

	if (e->type == BE_TAGS) {
		uint32_t bit;

		if (btn < 0 || !o->ipc)
			return;
		bit = 1u << o->span[el].btn[btn].tag;
		if (button == BTN_LEFT) {
			zdwl_ipc_output_v2_set_tags(o->ipc, bit, 1);
		} else if (button == BTN_RIGHT) {
			uint32_t now = tags_active_mask(o) ^ bit;

			if (now)
				zdwl_ipc_output_v2_set_tags(o->ipc, now, 0);
		} else if (button == BTN_MIDDLE) {
			zdwl_ipc_output_v2_set_client_tags(o->ipc, 0, bit);
		}
		return;
	}
	if (e->type == BE_BATTERY && button == BTN_LEFT && e->fmt_alt) {
		int changed = 0;

		e->alt = !e->alt;
		battery_update(e, &changed);
		bar_changed();
		return;
	}
	cmd = button == BTN_LEFT ? e->on_click : button == BTN_RIGHT ? e->on_right : e->on_middle;
	if (e->type == BE_KEYBOARD && button == BTN_LEFT && !cmd) {
		int n = 0;
		char code[64];

		while (kb_code(ext_layouts, (uint32_t)n, code, sizeof(code)))
			n++;
		if (ext && n > 1)
			zofwc_ipc_v1_set_keyboard_layout(ext, (ext_index + 1) % (uint32_t)n);
		return;
	}
	bar_run(o, cmd);
}

static void
ptr_button(void *data, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button,
           uint32_t state)
{
	Output *o = ptr_out;

	(void)data;
	(void)p;
	(void)serial;
	(void)time;
	if (!o || state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	bar_click(o, o->hover_x, button);
}

static void
ptr_axis(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t value)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
	(void)value;
}

/* wl_pointer v5: every event needs a handler (libwayland aborts on NULL). */
static void
ptr_frame(void *data, struct wl_pointer *p)
{
	(void)data;
	(void)p;
}

static void
ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
	(void)data;
	(void)p;
	(void)source;
}

static void
ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time, uint32_t axis)
{
	(void)data;
	(void)p;
	(void)time;
	(void)axis;
}

static void
ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis, int32_t discrete)
{
	(void)data;
	(void)p;
	(void)axis;
	(void)discrete;
}

static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void
seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	(void)data;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		if (pointer)
			wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(pointer);
		else
			wl_pointer_destroy(pointer);
		pointer = NULL;
		ptr_out = NULL;
	}
}

static void
seat_name(void *data, struct wl_seat *s, const char *name)
{
	(void)data;
	(void)s;
	(void)name;
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

static void
reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface,
           uint32_t version)
{
	(void)data;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, MIN(version, 4u));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell_ver = (int)MIN(version, 3u);
		layer_shell = wl_registry_bind(reg, name, &zwlr_layer_shell_v1_interface,
		                               (uint32_t)layer_shell_ver);
	} else if (!strcmp(iface, zdwl_ipc_manager_v2_interface.name)) {
		ipc_mgr = wl_registry_bind(reg, name, &zdwl_ipc_manager_v2_interface, MIN(version, 2u));
		if (ipc_mgr)
			zdwl_ipc_manager_v2_add_listener(ipc_mgr, &ipcm_listener, NULL);
	} else if (!strcmp(iface, zofwc_ipc_v1_interface.name)) {
		ext = wl_registry_bind(reg, name, &zofwc_ipc_v1_interface, 1);
		if (ext) {
			ext_have = 1;
			zofwc_ipc_v1_add_listener(ext, &ext_listener, NULL);
		}
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(reg, name, &wl_seat_interface, MIN(version, 5u));
		if (seat)
			wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		output_add(name, version);
	}
	outputs_ensure();
}

static void
reg_global_remove(void *data, struct wl_registry *reg, uint32_t name)
{
	Output *o;

	(void)data;
	(void)reg;
	for (o = outputs; o; o = o->next)
		if (o->gname == name) {
			output_destroy(o);
			bar_changed();
			return;
		}
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_global_remove,
};

#endif
