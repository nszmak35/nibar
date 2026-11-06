/* Bar elements: contents, layout and painting. */
#ifndef NIBAR_BAR_H
#define NIBAR_BAR_H

#include "gfx.h"
#include "data.h"

#ifndef BTN_LEFT
#define BTN_LEFT 0x110
#define BTN_RIGHT 0x111
#define BTN_MIDDLE 0x112
#endif

#define BAR_MAXEL 24

enum { BE_TEXT, BE_TAGS, BE_TITLE, BE_CLOCK, BE_BATTERY, BE_NETWORK, BE_KEYBOARD, BE_EXEC };
enum { AL_LEFT, AL_CENTER, AL_RIGHT };
enum { MK_TAGPOD, MK_COLOR, MK_SYMBOL };

typedef struct {
	int x, w;
	int tag;
} BarBtn;

typedef struct {
	int x, w;
	int nbtn;
	BarBtn btn[MAXTAGS];
} BarSpan;

typedef struct {
	struct wl_buffer *wl;
	struct wld_buffer *wld;
	void *map;
	size_t size;
	int w, h;
	int busy;
	struct Output *owner;
} ShmBuf;

typedef struct Output {
	struct Output *next;
	uint32_t gname;
	struct wl_output *wl;
	struct zdwl_ipc_output_v2 *ipc;
	struct wl_surface *surf;
	struct zwlr_layer_surface_v1 *ls;
	int need_draw;
	int configured;
	int narrowed;
	int sw, sh;
	int layer;
	int hidden;
	ShmBuf buf[2];
	int hover_el, hover_btn, hover_x;
	int mode_w;
	BarSpan span[BAR_MAXEL];
	int active, fullscreen, floating;
	uint32_t tstate[MAXTAGS], tclients[MAXTAGS], tfocus[MAXTAGS];
	char *title, *appid, *symbol;
} Output;

static Output *outputs;

static int ipc_ntags;
static int ext_have;
static char *ext_layouts;
static uint32_t ext_index;

static int bar_dirty;

static void
bar_changed(void)
{
	bar_dirty = 1;
}

typedef struct {
	char *name;
	int thr;
	int has_fg;
	uint32_t fg;
} BState;

struct BarElem {
	int type, align;
	char *name;
	char *family;
	TextStyle st;
	uint32_t fg, bg;
	int has_bg;
	int pad_l, pad_r;
	int mar_l, mar_r, has_mar;
	char *on_click, *on_right, *on_middle;
	char *fmt;
	char text[256];
	uint32_t cur_fg;
	int hidden;
	uint64_t next_ms;
	int interval_ms;
	int marker, pod_h, pod_top, hide_vacant, ntags, btn_pl, btn_pr;
	uint32_t pod_bg, fg_occ, fg_act, hover_bg, hover_fg;
	char *symbol;
	int max_len;
	char *fmt_charging, *fmt_plugged, *fmt_full, *fmt_alt, *bat_name;
	int alt;
	char **icons;
	int nicons;
	BState *states;
	int nstates;
	char *fmt_wifi, *fmt_eth, *fmt_disc, *iface;
	char **names;
	int nnames;
	char *cmd;
	int restart_ms;
	ExecState x;
};

static BarElem bar_el[BAR_MAXEL];
static int bar_nel;

static void
exec_stop(BarElem *e)
{
	ExecState *x = &e->x;

	if (x->fd >= 0) {
		close(x->fd);
		x->fd = -1;
	}
	if (x->pid > 0) {
		kill(-x->pid, SIGTERM);
		kill(x->pid, SIGTERM);
	}
	x->pid = 0;
}

static void
exec_finish(BarElem *e)
{
	ExecState *x = &e->x;

	if (x->blen) {
		size_t n = MIN(x->blen, sizeof(x->line) - 1);

		memcpy(x->line, x->buf, n);
		x->line[n] = '\0';
		x->blen = 0;
		exec_line_changed(e);
	}
	exec_stop(e);
	x->next_ms = now_ms() + (uint64_t)MAX(100, e->interval_ms ? e->interval_ms : e->restart_ms);
}

static void
exec_read(BarElem *e)
{
	ExecState *x = &e->x;
	char tmp[1024];
	ssize_t n;
	char *nl;
	int got = 0;

	if (x->fd < 0)
		return;
	n = read(x->fd, tmp, sizeof(tmp));
	if (n < 0) {
		if (errno == EAGAIN || errno == EINTR)
			return;
		n = 0;
	}
	if (n == 0) {
		exec_finish(e);
		return;
	}
	if (x->blen + (size_t)n >= sizeof(x->buf)) {
		size_t keep = sizeof(x->buf) / 2;

		if (x->blen > keep) {
			memmove(x->buf, x->buf + x->blen - keep, keep);
			x->blen = keep;
		}
		if (x->blen + (size_t)n >= sizeof(x->buf))
			n = (ssize_t)(sizeof(x->buf) - 1 - x->blen);
	}
	memcpy(x->buf + x->blen, tmp, (size_t)n);
	x->blen += (size_t)n;
	x->buf[x->blen] = '\0';
	while ((nl = memchr(x->buf, '\n', x->blen))) {
		size_t len = (size_t)(nl - x->buf);
		size_t used = len + 1;

		if (len && x->buf[len - 1] == '\r')
			len--;
		if (len) {
			len = MIN(len, sizeof(x->line) - 1);
			memcpy(x->line, x->buf, len);
			x->line[len] = '\0';
			got = 1;
		}
		memmove(x->buf, x->buf + used, x->blen - used);
		x->blen -= used;
		x->buf[x->blen] = '\0';
	}
	if (got)
		exec_line_changed(e);
}

/* What a child process must not inherit. */
static void
child_reset(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigprocmask(SIG_SETMASK, &set, NULL);
	signal(SIGPIPE, SIG_DFL);
	signal(SIGHUP, SIG_DFL);
	signal(SIGTERM, SIG_DFL);
	signal(SIGINT, SIG_DFL);
	signal(SIGCHLD, SIG_DFL);
}

static void
exec_start(BarElem *e)
{
	ExecState *x = &e->x;
	int fds[2];
	pid_t pid;

	if (x->pid > 0 || x->fd >= 0 || !e->cmd || !*e->cmd)
		return;
	if (pipe(fds) < 0)
		return;
	fcntl(fds[0], F_SETFD, FD_CLOEXEC);
	fcntl(fds[0], F_SETFL, fcntl(fds[0], F_GETFL) | O_NONBLOCK);
	pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		int nul = open("/dev/null", O_RDONLY);

		setpgid(0, 0);
		if (nul >= 0) {
			dup2(nul, 0);
			if (nul > 2)
				close(nul);
		}
		dup2(fds[1], 1);
		close(fds[0]);
		close(fds[1]);
		child_reset();
		execl("/bin/sh", "sh", "-c", e->cmd, (char *)NULL);
		_exit(127);
	}
	setpgid(pid, pid);
	close(fds[1]);
	x->pid = pid;
	x->fd = fds[0];
	x->blen = 0;
}

static void
exec_poll(uint64_t now)
{
	int i;

	for (i = 0; i < bar_nel; i++) {
		BarElem *e = &bar_el[i];

		if (e->type == BE_EXEC && e->x.pid <= 0 && now >= e->x.next_ms)
			exec_start(e);
	}
}

static void
spawn_cmd(const char *cmd)
{
	pid_t pid;

	if (!cmd || !*cmd)
		return;
	pid = fork();
	if (pid < 0)
		return;
	if (pid == 0) {
		setsid();
		child_reset();
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
}

static void
elem_set_text(BarElem *e, const char *t, uint32_t fg, int hidden, int *changed)
{
	if (strcmp(e->text, t) || e->cur_fg != fg || e->hidden != hidden)
		*changed = 1;
	snprintf(e->text, sizeof(e->text), "%s", t);
	e->cur_fg = fg;
	e->hidden = hidden;
}

static void
exec_line_changed(BarElem *e)
{
	FmtVar v[1];
	char out[256];
	int changed = 0;

	fmt_set(&v[0], "", e->x.line);
	fmt_expand(e->fmt ? e->fmt : "{}", v, 1, out, sizeof(out));
	if (e->max_len > 0)
		utf8_ellipsize(out, sizeof(out), e->max_len);
	elem_set_text(e, out, e->fg, !*out, &changed);
	if (changed)
		bar_changed();
}

static const BState *
battery_state(const BarElem *e, int capacity)
{
	const BState *best = NULL;
	int i;

	for (i = 0; i < e->nstates; i++)
		if (capacity <= e->states[i].thr && (!best || e->states[i].thr < best->thr))
			best = &e->states[i];
	return best;
}

static void
battery_update(BarElem *e, int *changed)
{
	BatInfo b;
	const char *fmt;
	const BState *st;
	FmtVar v[5];
	char icon[16] = "", capb[16], timeb[48] = "--", powb[24] = "", out[256];
	uint32_t fg = e->fg;

	bat_read(e->bat_name, &b);
	if (!b.ok) {
		elem_set_text(e, "", e->fg, 1, changed);
		return;
	}
	if (e->nicons > 0) {
		int idx = MIN(e->nicons - 1, b.capacity * e->nicons / 100);

		snprintf(icon, sizeof(icon), "%s", e->icons[idx]);
	}
	if (b.rate > 0) {
		double hours = 0;

		if (!strcasecmp(b.status, "Discharging"))
			hours = (double)b.now / (double)b.rate;
		else if (!strcasecmp(b.status, "Charging"))
			hours = (double)(b.full - b.now) / (double)b.rate;
		if (hours > 0 && hours < 1000) {
			int mins = (int)(hours * 60.0 + 0.5);

			snprintf(timeb, sizeof(timeb), "%d h %d min", mins / 60, mins % 60);
		}
		snprintf(powb, sizeof(powb), "%.1f", (double)b.rate / 1e6);
	}
	snprintf(capb, sizeof(capb), "%d", b.capacity);

	fmt = e->fmt;
	if (!strcasecmp(b.status, "Charging") && e->fmt_charging)
		fmt = e->fmt_charging;
	else if (!strcasecmp(b.status, "Full") && e->fmt_full)
		fmt = e->fmt_full;
	else if (b.ac && strcasecmp(b.status, "Discharging") && e->fmt_plugged)
		fmt = e->fmt_plugged;
	if (e->alt && e->fmt_alt)
		fmt = e->fmt_alt;

	fmt_set(&v[0], "capacity", capb);
	fmt_set(&v[1], "icon", icon);
	fmt_set(&v[2], "time", timeb);
	fmt_set(&v[3], "status", b.status);
	fmt_set(&v[4], "power", powb);
	fmt_expand(fmt ? fmt : "{capacity}%", v, 5, out, sizeof(out));

	st = battery_state(e, b.capacity);
	if (st && st->has_fg)
		fg = st->fg;
	elem_set_text(e, out, fg, 0, changed);
}

static void
network_update(BarElem *e, int *changed)
{
	char ifname[64], out[256];
	FmtVar v[1];
	int kind = net_read(e->iface, ifname, sizeof(ifname));
	const char *fmt = kind == NET_WIFI ? e->fmt_wifi : kind == NET_ETH ? e->fmt_eth : e->fmt_disc;

	if (!fmt)
		fmt = e->fmt ? e->fmt : "";
	fmt_set(&v[0], "ifname", ifname);
	fmt_expand(fmt, v, 1, out, sizeof(out));
	elem_set_text(e, out, e->fg, !*out, changed);
}

static int
kb_code(const char *layouts, uint32_t n, char *out, size_t cap)
{
	const char *p = layouts;
	uint32_t i = 0;

	out[0] = '\0';
	if (!p)
		return 0;
	while (1) {
		const char *q = strchr(p, ',');
		size_t len = q ? (size_t)(q - p) : strlen(p), k;

		if (i == n) {
			while (len && *p == ' ') {
				p++;
				len--;
			}
			for (k = 0; k < len && p[k] != '(' && k + 1 < cap; k++)
				out[k] = p[k];
			out[k] = '\0';
			return 1;
		}
		if (!q)
			return 0;
		p = q + 1;
		i++;
	}
}

static void
keyboard_update(BarElem *e, int *changed)
{
	FmtVar v[3];
	char code[64] = "", up[64], out[256];
	const char *name;
	size_t i;

	if (!ext_have || !ext_layouts) {
		elem_set_text(e, "", e->fg, 1, changed);
		return;
	}
	if (!kb_code(ext_layouts, ext_index, code, sizeof(code)))
		kb_code(ext_layouts, 0, code, sizeof(code));
	for (i = 0; code[i] && i < sizeof(up) - 1; i++)
		up[i] = (char)toupper((unsigned char)code[i]);
	up[i] = '\0';
	name = (e->names && (int)ext_index < e->nnames) ? e->names[ext_index] : up;
	fmt_set(&v[0], "layout", code);
	fmt_set(&v[1], "LAYOUT", up);
	fmt_set(&v[2], "name", name);
	fmt_expand(e->fmt ? e->fmt : "{LAYOUT}", v, 3, out, sizeof(out));
	elem_set_text(e, out, e->fg, !*out, changed);
}

static int
bar_refresh(int force)
{
	uint64_t now = now_ms();
	int changed = 0, i;

	for (i = 0; i < bar_nel; i++) {
		BarElem *e = &bar_el[i];
		char out[256];

		switch (e->type) {
		case BE_CLOCK:
			clock_expand(e->fmt, out, sizeof(out));
			if (e->max_len > 0)
				utf8_ellipsize(out, sizeof(out), e->max_len);
			elem_set_text(e, out, e->fg, 0, &changed);
			break;
		case BE_BATTERY:
			if (force || now >= e->next_ms) {
				e->next_ms = now + (uint64_t)e->interval_ms;
				battery_update(e, &changed);
			}
			break;
		case BE_NETWORK:
			if (force || now >= e->next_ms) {
				e->next_ms = now + (uint64_t)e->interval_ms;
				network_update(e, &changed);
			}
			break;
		case BE_KEYBOARD:
			keyboard_update(e, &changed);
			break;
		default:
			break;
		}
	}
	return changed;
}

static void
elem_free(BarElem *e)
{
	int i;

	exec_stop(e);
	free(e->name);
	free(e->family);
	free(e->on_click);
	free(e->on_right);
	free(e->on_middle);
	free(e->fmt);
	free(e->symbol);
	free(e->fmt_charging);
	free(e->fmt_plugged);
	free(e->fmt_full);
	free(e->fmt_alt);
	free(e->bat_name);
	free(e->fmt_wifi);
	free(e->fmt_eth);
	free(e->fmt_disc);
	free(e->iface);
	free(e->cmd);
	if (e->icons)
		list_free(e->icons, e->nicons);
	if (e->names)
		list_free(e->names, e->nnames);
	for (i = 0; i < e->nstates; i++)
		free(e->states[i].name);
	free(e->states);
	memset(e, 0, sizeof(*e));
	e->x.fd = -1;
}

static void
bar_elems_clear(void)
{
	int i;

	for (i = 0; i < bar_nel; i++)
		elem_free(&bar_el[i]);
	bar_nel = 0;
}

static int
name_has(const char *name, const char *needle)
{
	return strcasestr(name, needle) != NULL;
}

static int
elem_type(const BarElemCfg *c)
{
	const char *t = kv_get(c, "element_type");

	if (t) {
		if (!strcasecmp(t, "text") || !strcasecmp(t, "label") || !strcasecmp(t, "button"))
			return BE_TEXT;
		if (!strcasecmp(t, "tags") || !strcasecmp(t, "workspaces"))
			return BE_TAGS;
		if (!strcasecmp(t, "title") || !strcasecmp(t, "window"))
			return BE_TITLE;
		if (!strcasecmp(t, "clock") || !strcasecmp(t, "time"))
			return BE_CLOCK;
		if (!strcasecmp(t, "battery"))
			return BE_BATTERY;
		if (!strcasecmp(t, "network"))
			return BE_NETWORK;
		if (!strcasecmp(t, "keyboard") || !strcasecmp(t, "layout") || !strcasecmp(t, "language"))
			return BE_KEYBOARD;
		if (!strcasecmp(t, "exec") || !strcasecmp(t, "command") || !strcasecmp(t, "custom"))
			return BE_EXEC;
		if (!strcasecmp(t, "tray"))
			return -2;
		return -1;
	}
	if (name_has(c->name, "tray"))
		return -2;
	if (name_has(c->name, "tag") || name_has(c->name, "workspace"))
		return BE_TAGS;
	if (name_has(c->name, "title") || name_has(c->name, "window"))
		return BE_TITLE;
	if (name_has(c->name, "clock") || name_has(c->name, "time"))
		return BE_CLOCK;
	if (name_has(c->name, "batt"))
		return BE_BATTERY;
	if (name_has(c->name, "net"))
		return BE_NETWORK;
	if (name_has(c->name, "lang") || name_has(c->name, "keyboard") ||
	    name_has(c->name, "layout") || name_has(c->name, "kb"))
		return BE_KEYBOARD;
	if (kv_get(c, "exec") || kv_get(c, "command"))
		return BE_EXEC;
	if (kv_get(c, "text"))
		return BE_TEXT;
	return -1;
}

static void
elem_build(BarElem *e, const BarElemCfg *c, int type)
{
	const char *v;
	char *s;
	int n, i;

	memset(e, 0, sizeof(*e));
	e->x.fd = -1;
	e->type = type;
	e->name = estrdup(c->name);

	v = kv_get(c, "align");
	e->align = !v ? AL_LEFT : !strcasecmp(v, "right") ? AL_RIGHT
	                        : !strcasecmp(v, "center") || !strcasecmp(v, "centre") ? AL_CENTER
	                                                                              : AL_LEFT;
	s = kv_str(c, "font", NULL);
	e->family = s ? s : estrdup(cfg.font);
	e->st.family = e->family;
	e->st.px = MAX(4, kv_int(c, "font_size_px", cfg.font_px));
	v = kv_get(c, "font_weight");
	e->st.bold = kv_bool(c, "bold", v && !strcasecmp(v, "bold"));
	e->fg = kv_color(c, "fg", cfg.fg, NULL);
	e->bg = kv_color(c, "bg", 0, &e->has_bg);
	e->cur_fg = e->fg;
	e->pad_l = e->pad_r = cfg.elem_pad;
	kv_pair(c, "padding", &e->pad_l, &e->pad_r);
	if (kv_get(c, "margin")) {
		e->has_mar = 1;
		e->mar_l = e->mar_r = cfg.elem_margin;
		kv_pair(c, "margin", &e->mar_l, &e->mar_r);
	}
	e->on_click = kv_str(c, "on_click", NULL);
	e->on_right = kv_str(c, "on_right_click", NULL);
	e->on_middle = kv_str(c, "on_middle_click", NULL);
	e->max_len = MAX(0, kv_int(c, "max_length", 0));

	switch (type) {
	case BE_TEXT:
		s = kv_str(c, "text", "");
		e->fmt = glyph_alias(s);
		free(s);
		snprintf(e->text, sizeof(e->text), "%s", e->fmt);
		e->hidden = !*e->text;
		break;
	case BE_TAGS:
		v = kv_get(c, "tag_active_marker");
		e->marker = !v ? MK_TAGPOD : !strcasecmp(v, "color") ? MK_COLOR
		                           : !strcasecmp(v, "symbol") ? MK_SYMBOL : MK_TAGPOD;
		e->fg_occ = kv_color(c, "fg_occupied", e->fg, NULL);
		e->fg_act = kv_color(c, "fg_active", e->fg_occ, NULL);
		e->hover_bg = kv_color(c, "hover_bg", 0, NULL);
		e->hover_fg = kv_color(c, "hover_fg", e->fg_act, NULL);
		e->pod_bg = kv_color(c, "TagPod.bg", kv_color(c, "pod_bg", e->fg_act, NULL), NULL);
		e->pod_h = MAX(0, kv_int(c, "TagPod.thickness", kv_int(c, "pod_thickness", 2)));
		v = kv_get(c, "TagPod.position");
		if (!v)
			v = kv_get(c, "pod_position");
		e->pod_top = v && !strcasecmp(v, "top");
		e->btn_pl = e->btn_pr = 8;
		kv_pair(c, "button_padding", &e->btn_pl, &e->btn_pr);
		e->hide_vacant = kv_bool(c, "hide_vacant", 0);
		e->ntags = kv_int(c, "num_tags", 0);
		e->symbol = kv_str(c, "tag_symbol", "\xe2\x80\xa2");
		break;
	case BE_TITLE:
		e->fmt = kv_str(c, "format", "{title}");
		break;
	case BE_CLOCK:
		s = kv_str(c, "format", NULL);
		e->fmt = s ? s : kv_str(c, "text", "{:%H:%M}");
		break;
	case BE_BATTERY:
		e->fmt = kv_str(c, "format", "{capacity}% {icon}");
		e->fmt_charging = kv_str(c, "format_charging", NULL);
		e->fmt_plugged = kv_str(c, "format_plugged", NULL);
		e->fmt_full = kv_str(c, "format_full", NULL);
		e->fmt_alt = kv_str(c, "format_alt", NULL);
		e->bat_name = kv_str(c, "battery", NULL);
		e->interval_ms = 1000 * MAX(1, kv_int(c, "interval", 10));
		e->icons = list_split(kv_get(c, "format_icons"), &e->nicons);
		v = kv_get(c, "states");
		if (v) {
			char **it = list_split(v, &n);

			for (i = 0; i < n; i++) {
				char *sep = strchr(it[i], RS_SEP);
				long thr;
				char key[96];

				if (!sep)
					continue;
				*sep = '\0';
				if (parse_num(sep + 1, &thr) < 0)
					continue;
				e->states = erealloc(e->states, (size_t)(e->nstates + 1) * sizeof(BState));
				e->states[e->nstates].name = estrdup(it[i]);
				e->states[e->nstates].thr = (int)thr;
				snprintf(key, sizeof(key), "fg_%s", it[i]);
				e->states[e->nstates].fg =
				    kv_color(c, key, e->fg, &e->states[e->nstates].has_fg);
				e->nstates++;
			}
			list_free(it, n);
		}
		e->hidden = 1;
		break;
	case BE_NETWORK:
		e->fmt = kv_str(c, "format", NULL);
		e->fmt_wifi = kv_str(c, "format_wifi", e->fmt ? NULL : "\xef\x87\xab");
		e->fmt_eth = kv_str(c, "format_ethernet", e->fmt ? NULL : "\xf3\xb0\x88\x80");
		e->fmt_disc = kv_str(c, "format_disconnected", e->fmt ? NULL : "\xf3\xb0\xa4\xae");
		e->iface = kv_str(c, "interface", NULL);
		e->interval_ms = 1000 * MAX(1, kv_int(c, "interval", 5));
		break;
	case BE_KEYBOARD:
		e->fmt = kv_str(c, "format", "{LAYOUT}");
		e->names = list_split(kv_get(c, "names"), &e->nnames);
		e->hidden = 1;
		break;
	case BE_EXEC:
		e->cmd = kv_str(c, "exec", NULL);
		if (!e->cmd)
			e->cmd = kv_str(c, "command", NULL);
		e->fmt = kv_str(c, "format", "{}");
		e->interval_ms = 1000 * MAX(0, kv_int(c, "interval", 0));
		e->restart_ms = 1000 * MAX(1, kv_int(c, "restart_interval", 1));
		e->hidden = 1;
		break;
	}
}

static int
bar_reconfigure(void)
{
	char **fb;
	int nfb = 0, i;

	bar_elems_clear();
	gfx_flush_fonts();
	if (gfx_init() < 0)
		return -1;
	fb = list_split(cfg.fallback, &nfb);
	gfx_set_fallbacks(fb, nfb);
	list_free(fb, nfb);

	for (i = 0; i < cfg.nbelems && bar_nel < BAR_MAXEL; i++) {
		int type = elem_type(&cfg.belems[i]);

		if (type == -2) {
			nb_log("the tray ('%s') is not supported", cfg.belems[i].name);
			continue;
		}
		if (type < 0) {
			nb_log("element '%s' has no usable element_type, skipped", cfg.belems[i].name);
			continue;
		}
		elem_build(&bar_el[bar_nel++], &cfg.belems[i], type);
	}
	if (i < cfg.nbelems)
		nb_log("at most %d elements are shown", BAR_MAXEL);
	bar_refresh(1);
	exec_poll(now_ms());
	bar_changed();
	return 0;
}

static void
bar_pads(int *t, int *r, int *b, int *l)
{
	char **it;
	int n = 0, i;
	long v[4] = {0, 0, 0, 0};

	it = list_split(cfg.padding, &n);
	if (!it) {
		char *copy = estrdup(cfg.padding ? cfg.padding : ""), *tok, *save = NULL;

		n = 0;
		for (tok = strtok_r(copy, ", ", &save); tok && n < 4; tok = strtok_r(NULL, ", ", &save))
			if (parse_num(tok, &v[n]) == 0)
				n++;
		free(copy);
	} else {
		for (i = 0; i < n && i < 4; i++)
			parse_num(it[i], &v[i]);
		list_free(it, n);
		n = MIN(n, 4);
	}
	if (n == 1)
		v[1] = v[2] = v[3] = v[0];
	else if (n == 2) {
		v[2] = v[0];
		v[3] = v[1];
	} else if (n == 3)
		v[3] = v[1];
	*t = (int)MAX(0, MIN(v[0], 5000));
	*r = (int)MAX(0, MIN(v[1], 5000));
	*b = (int)MAX(0, MIN(v[2], 5000));
	*l = (int)MAX(0, MIN(v[3], 5000));
}

static void
bar_tag_label(const BarElem *e, int t, int active, char *out, size_t cap)
{
	const char *name = t < cfg.ntags ? cfg.tags[t] : "";
	char num[16];

	if (!*name) {
		snprintf(num, sizeof(num), "%d", t + 1);
		name = num;
	}
	if (active && e->marker == MK_SYMBOL)
		snprintf(out, cap, "%s %s", e->symbol ? e->symbol : "", name);
	else
		snprintf(out, cap, "%s", name);
}

static int
bar_tags_count(const BarElem *e)
{
	int n = e->ntags > 0 ? e->ntags : ipc_ntags;

	return MAX(0, MIN(n, MIN(ipc_ntags, MAXTAGS)));
}

static int
tag_active(const Output *o, int t)
{
	return (o->tstate[t] & ZDWL_IPC_OUTPUT_V2_TAG_STATE_ACTIVE) != 0;
}

static int
tag_occupied(const Output *o, int t)
{
	return o->tclients[t] > 0;
}

static void
bar_title_text(const Output *o, const BarElem *e, char *out, size_t cap)
{
	FmtVar v[3];

	fmt_set(&v[0], "title", o->title ? o->title : "");
	fmt_set(&v[1], "appid", o->appid ? o->appid : "");
	fmt_set(&v[2], "layout", o->symbol ? o->symbol : "");
	fmt_expand(e->fmt ? e->fmt : "{title}", v, 3, out, cap);
	if (e->max_len > 0)
		utf8_ellipsize(out, cap, e->max_len);
}

static int
bar_content(const Output *o, const BarElem *e, BarSpan *sp, char *text, size_t cap)
{
	text[0] = '\0';
	sp->nbtn = 0;
	switch (e->type) {
	case BE_TAGS: {
		int n = bar_tags_count(e), t, x = 0;

		for (t = 0; t < n; t++) {
			int active = tag_active(o, t), w;
			char lab[96];

			if (e->hide_vacant && !active && !tag_occupied(o, t))
				continue;
			bar_tag_label(e, t, active, lab, sizeof(lab));
			w = e->btn_pl + gfx_text_width(&e->st, lab) + e->btn_pr;
			sp->btn[sp->nbtn].x = x;
			sp->btn[sp->nbtn].w = w;
			sp->btn[sp->nbtn].tag = t;
			sp->nbtn++;
			x += w;
		}
		return sp->nbtn ? x : -1;
	}
	case BE_TITLE:
		bar_title_text(o, e, text, cap);
		return gfx_text_width(&e->st, text);
	default:
		if (e->hidden || !*e->text)
			return -1;
		snprintf(text, cap, "%s", e->text);
		return gfx_text_width(&e->st, text);
	}
}

typedef struct {
	int cw;
	char text[256];
} BarItem;

static void
bar_paint(Output *o, int W, int H)
{
	BarItem item[BAR_MAXEL];
	int grp[3][BAR_MAXEL], gn[3] = {0, 0, 0}, total[3] = {0, 0, 0};
	int i, g, k;
	int left_end, right_start, x;

	for (i = 0; i < BAR_MAXEL; i++)
		o->span[i].w = 0, o->span[i].nbtn = 0;

	for (i = 0; i < bar_nel; i++) {
		BarElem *e = &bar_el[i];
		BarSpan sp;

		memset(&sp, 0, sizeof(sp));
		item[i].cw = bar_content(o, e, &sp, item[i].text, sizeof(item[i].text));
		o->span[i] = sp;
		o->span[i].w = 0;
		if (item[i].cw < 0)
			continue;
		grp[e->align][gn[e->align]++] = i;
	}
	for (g = 0; g < 3; g++) {
		for (k = 0; k < gn[g]; k++) {
			BarElem *e = &bar_el[grp[g][k]];
			int ml = e->has_mar ? e->mar_l : cfg.elem_margin;
			int mr = e->has_mar ? e->mar_r : cfg.elem_margin;
			int w = e->pad_l + item[grp[g][k]].cw + e->pad_r;

			if (!e->has_mar) {
				if (k == 0)
					ml = 0;
				if (k == gn[g] - 1)
					mr = 0;
			}
			o->span[grp[g][k]].x = ml;
			o->span[grp[g][k]].w = w;
			total[g] += ml + w + mr;
		}
	}
	left_end = total[AL_LEFT];
	right_start = W - total[AL_RIGHT];
	for (g = 0; g < 3; g++) {
		if (g == AL_LEFT)
			x = 0;
		else if (g == AL_RIGHT)
			x = right_start;
		else {
			x = (W - total[AL_CENTER]) / 2;
			x = MAX(x, left_end);
			x = MIN(x, MAX(left_end, right_start - total[AL_CENTER]));
		}
		for (k = 0; k < gn[g]; k++) {
			BarElem *e = &bar_el[grp[g][k]];
			BarSpan *sp = &o->span[grp[g][k]];
			int ml = sp->x;
			int mr = e->has_mar ? e->mar_r : (k == gn[g] - 1 ? 0 : cfg.elem_margin);

			sp->x = x + ml;
			x += ml + sp->w + mr;
		}
	}

	gfx_fill(0, 0, W, H, cfg.bg);
	for (i = 0; i < bar_nel; i++) {
		BarElem *e = &bar_el[i];
		BarSpan *sp = &o->span[i];

		if (item[i].cw < 0 || sp->w <= 0)
			continue;
		if (e->has_bg)
			gfx_fill(sp->x, 0, sp->w, H, e->bg);
		if (e->type == BE_TAGS) {
			int bx = sp->x + e->pad_l, k2;

			for (k2 = 0; k2 < sp->nbtn; k2++) {
				BarBtn *bt = &sp->btn[k2];
				int t = bt->tag, active = tag_active(o, t);
				int hover = o->hover_el == i && o->hover_btn == k2;
				uint32_t col = active ? e->fg_act : tag_occupied(o, t) ? e->fg_occ : e->fg;
				char lab[96];

				bt->x += bx;
				if (hover) {
					if (e->hover_bg)
						gfx_fill(bt->x, 0, bt->w, H, e->hover_bg);
					col = e->hover_fg;
				}
				if (active && e->marker == MK_TAGPOD && e->pod_h > 0)
					gfx_fill(bt->x, e->pod_top ? 0 : H - e->pod_h, bt->w, e->pod_h, e->pod_bg);
				bar_tag_label(e, t, active, lab, sizeof(lab));
				gfx_text_draw(&e->st, col, bt->x + e->btn_pl, 0, H, lab);
			}
			continue;
		}
		gfx_text_draw(&e->st, e->type == BE_TITLE ? e->fg : e->cur_fg, sp->x + e->pad_l, 0, H,
		              item[i].text);
	}
}

static int
bar_hit(const Output *o, int x, int *btn)
{
	int i, k;

	*btn = -1;
	for (i = 0; i < bar_nel; i++) {
		const BarSpan *sp = &o->span[i];

		if (sp->w <= 0 || x < sp->x || x >= sp->x + sp->w)
			continue;
		if (bar_el[i].type == BE_TAGS)
			for (k = 0; k < sp->nbtn; k++)
				if (x >= sp->btn[k].x && x < sp->btn[k].x + sp->btn[k].w) {
					*btn = k;
					break;
				}
		return i;
	}
	return -1;
}

#endif
