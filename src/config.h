/* Configuration (~/.config/nibar/config.py) and its reload. */
#ifndef NIBAR_CONFIG_H
#define NIBAR_CONFIG_H

#include <stddef.h>
#include <sys/stat.h>
#include <unistd.h>

enum { LAYER_BACKGROUND, LAYER_BOTTOM, LAYER_TOP, LAYER_OVERLAY };

typedef struct {
	char *name;
	char **kv;
	size_t nkv;
} BarElemCfg;

typedef struct {
	int enabled;
	int height, width;
	int top;
	int layer;
	char *padding;
	uint32_t bg, fg;
	char *font;
	int font_px;
	char *fallback;
	int elem_margin, elem_pad;
	char **tags;
	int ntags;
	BarElemCfg *belems;
	int nbelems;
} Config;

static Config cfg;
static const char *cli_config;

#include "default_config.h"

enum { K_INT, K_STR, K_COLOR, K_BOOL, K_POS, K_LAYER };

typedef struct {
	const char *key;
	int kind;
	size_t off;
} Scalar;

#define S(k, kind, field) {k, kind, offsetof(Config, field)}
static const Scalar scalars[] = {
	S("bar_enabled", K_BOOL, enabled),
	S("bar_height", K_INT, height),
	S("bar_width", K_INT, width),
	S("bar_position_bar", K_POS, top),
	S("bar_layer", K_LAYER, layer),
	S("bar_padding", K_STR, padding),
	S("bar_bg", K_COLOR, bg),
	S("bar_rootcolor", K_COLOR, bg),
	S("bar_fg", K_COLOR, fg),
	S("bar_font", K_STR, font),
	S("bar_font_size_px", K_INT, font_px),
	S("bar_fallback_fonts", K_STR, fallback),
	S("bar_element_margin", K_INT, elem_margin),
	S("bar_element_padding", K_INT, elem_pad),
};
#undef S

static int
config_set_scalar(Config *c, const char *key, const char *val)
{
	size_t i;

	for (i = 0; i < LENGTH(scalars); i++) {
		char *base = (char *)c + scalars[i].off;
		long l;
		int b;

		if (strcmp(scalars[i].key, key))
			continue;
		switch (scalars[i].kind) {
		case K_INT:
			if (parse_num(val, &l) < 0)
				return -1;
			*(int *)base = (int)MAX(-100000, MIN(100000, l));
			return 1;
		case K_STR:
			free(*(char **)base);
			*(char **)base = estrdup(!strcmp(val, "NULL") ? "" : val);
			return 1;
		case K_COLOR:
			return parse_color(val, (uint32_t *)base) < 0 ? -1 : 1;
		case K_BOOL:
			if (parse_bool(val, &b) < 0)
				return -1;
			*(int *)base = b;
			return 1;
		case K_POS:
			if (!strcasecmp(val, "top"))
				*(int *)base = 1;
			else if (!strcasecmp(val, "bottom"))
				*(int *)base = 0;
			else
				return -1;
			return 1;
		case K_LAYER:
			if (!strcasecmp(val, "top"))
				*(int *)base = LAYER_TOP;
			else if (!strcasecmp(val, "bottom"))
				*(int *)base = LAYER_BOTTOM;
			else if (!strcasecmp(val, "overlay"))
				*(int *)base = LAYER_OVERLAY;
			else
				return -1;
			return 1;
		}
	}
	return 0;
}

static void
config_defaults(Config *c)
{
	memset(c, 0, sizeof(*c));
	c->enabled = 1;
	c->height = 30;
	c->width = 100;
	c->top = 0;
	c->layer = LAYER_TOP;
	c->padding = estrdup("0,0,0,0");
	parse_color("0x0B0B0Aff", &c->bg);
	parse_color("0xCDCDCAff", &c->fg);
	c->font = estrdup("monospace");
	c->font_px = 14;
	c->fallback = estrdup("");
	c->elem_margin = 3;
	c->elem_pad = 4;
}

static void
config_elems_free(Config *c)
{
	int i;
	size_t k;

	for (i = 0; i < c->nbelems; i++) {
		free(c->belems[i].name);
		for (k = 0; k < c->belems[i].nkv; k++)
			free(c->belems[i].kv[k]);
		free(c->belems[i].kv);
	}
	free(c->belems);
	c->belems = NULL;
	c->nbelems = 0;
}

static void
config_tags_free(Config *c)
{
	int i;

	for (i = 0; i < c->ntags; i++)
		free(c->tags[i]);
	free(c->tags);
	c->tags = NULL;
	c->ntags = 0;
}

static void
config_free(Config *c)
{
	free(c->padding);
	free(c->font);
	free(c->fallback);
	config_tags_free(c);
	config_elems_free(c);
	memset(c, 0, sizeof(*c));
}

enum { L_TAGS, L_BAR };

typedef struct {
	Config *cfg;
	int user_seen[2];
	int errors;
} Builder;

static void
list_reset_if_user(Builder *b, int list, int source)
{
	if (source != 1 || b->user_seen[list])
		return;
	b->user_seen[list] = 1;
	if (list == L_TAGS)
		config_tags_free(b->cfg);
	else
		config_elems_free(b->cfg);
}

static int
builder_entry(const NbEntry *e, void *ud)
{
	Builder *b = ud;
	Config *c = b->cfg;

	if (e->type == NB_SCALAR) {
		int rc = config_set_scalar(c, e->key, e->value);

		if (rc < 0) {
			nb_log("config: bad value for '%s': %s", e->key, e->value);
			b->errors++;
		} else if (rc == 0) {
			nb_dbg("config: ignoring unknown setting '%s'", e->key);
		}
		return 0;
	}
	if (e->count == 0)
		return 0;
	if (!strcmp(e->key, "tags")) {
		list_reset_if_user(b, L_TAGS, e->source);
		if (c->ntags < MAXTAGS) {
			c->tags = erealloc(c->tags, (size_t)(c->ntags + 1) * sizeof(char *));
			c->tags[c->ntags++] = estrdup(e->items[0]);
		}
	} else if (!strcmp(e->key, "bar_element")) {
		BarElemCfg *el;
		size_t k;

		list_reset_if_user(b, L_BAR, e->source);
		c->belems = erealloc(c->belems, (size_t)(c->nbelems + 1) * sizeof(BarElemCfg));
		el = &c->belems[c->nbelems++];
		memset(el, 0, sizeof(*el));
		el->name = estrdup(e->items[0]);
		el->nkv = (e->count - 1) & ~(size_t)1;
		el->kv = ecalloc(el->nkv ? el->nkv : 1, sizeof(char *));
		for (k = 0; k < el->nkv; k++)
			el->kv[k] = estrdup(e->items[k + 1]);
	}
	return 0;
}

static void
builder_finish(Config *c)
{
	c->height = MAX(0, MIN(c->height, 400));
	c->width = MAX(1, MIN(c->width, 100));
	c->font_px = MAX(4, MIN(c->font_px, 200));
	c->elem_margin = MAX(0, MIN(c->elem_margin, 200));
	c->elem_pad = MAX(0, MIN(c->elem_pad, 200));
}

static int
config_load(Config *out, const char *user_path, int strict)
{
	Config c;
	Builder b;
	int found = 0;

	config_defaults(&c);
	memset(&b, 0, sizeof(b));
	b.cfg = &c;

	if (nb_parse_buffer(nibar_default_config, "<nibar built-in defaults>", 0, builder_entry,
	                    &b) < 0) {
		nb_log("internal error: built-in default configuration failed");
		goto fail;
	}
	if (user_path) {
		if (nb_parse_file(user_path, 1, builder_entry, &b, &found) < 0) {
			nb_log("config: error in %s", user_path);
			goto fail;
		}
	}
	if (strict && b.errors) {
		nb_log("config: %d bad value(s) in %s", b.errors, user_path ? user_path : "config");
		goto fail;
	}
	builder_finish(&c);
	*out = c;
	return 0;

fail:
	config_free(&c);
	return -1;
}

static char *
user_config_path(const char *override)
{
	const char *home, *xdg;
	char buf[PATH_MAX];

	if (override && *override)
		return estrdup(override);
	xdg = getenv("XDG_CONFIG_HOME");
	if (xdg && *xdg && xdg[0] == '/') {
		snprintf(buf, sizeof(buf), "%s/nibar/config.py", xdg);
		return estrdup(buf);
	}
	home = getenv("HOME");
	if (!home || !*home)
		return NULL;
	snprintf(buf, sizeof(buf), "%s/.config/nibar/config.py", home);
	return estrdup(buf);
}

static void
config_seed_user_file(const char *path)
{
	char dir[PATH_MAX], *slash, *p;
	FILE *fp;

	if (!path || access(path, F_OK) == 0 || strlen(path) >= sizeof(dir))
		return;
	strcpy(dir, path);
	slash = strrchr(dir, '/');
	if (!slash)
		return;
	*slash = '\0';
	for (p = dir + 1; *p; p++) {
		if (*p == '/') {
			*p = '\0';
			mkdir(dir, 0755);
			*p = '/';
		}
	}
	mkdir(dir, 0755);
	fp = fopen(path, "wx");
	if (!fp)
		return;
	fputs(nibar_default_config, fp);
	fclose(fp);
	nb_log("created %s", path);
}

#endif
