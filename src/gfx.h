/* Fonts and pixel drawing. */
#ifndef NIBAR_GFX_H
#define NIBAR_GFX_H

#include <fontconfig/fontconfig.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#include <wld/pixman.h>
#include <wld/wld.h>
#pragma GCC diagnostic pop

#define GFX_MAXFONTS 96
#define GFX_MAXMISS 256

typedef struct {
	const char *family;
	int px;
	int bold;
} TextStyle;

typedef struct {
	char key[320];
	struct wld_font *font;
} FontEntry;

typedef struct {
	uint32_t cp;
	int px, bold;
	struct wld_font *font;
} AutoEntry;

static struct wld_font_context *gfx_fctx;
static struct wld_context *gfx_ctx;
static struct wld_renderer *gfx_rend;
static FontEntry gfx_fonts[GFX_MAXFONTS];
static int gfx_nfonts;
static AutoEntry gfx_auto[GFX_MAXMISS];
static int gfx_nauto;
static char **gfx_fallbacks;
static int gfx_nfallbacks;

static int
gfx_init(void)
{
	if (gfx_fctx && gfx_ctx && gfx_rend)
		return 0;
	if (!FcInit())
		return -1;
	if (!gfx_fctx)
		gfx_fctx = wld_font_create_context();
	if (!gfx_ctx)
		gfx_ctx = wld_pixman_create_context();
	if (gfx_ctx && !gfx_rend)
		gfx_rend = wld_create_renderer(gfx_ctx);
	if (!gfx_fctx || !gfx_ctx || !gfx_rend) {
		nb_log("cannot set up the font/drawing context");
		return -1;
	}
	return 0;
}

static void
gfx_flush_fonts(void)
{
	int i;

	for (i = 0; i < gfx_nfonts; i++)
		if (gfx_fonts[i].font)
			wld_font_close(gfx_fonts[i].font);
	gfx_nfonts = 0;
	for (i = 0; i < gfx_nauto; i++)
		if (gfx_auto[i].font)
			wld_font_close(gfx_auto[i].font);
	gfx_nauto = 0;
	if (gfx_rend) {
		wld_destroy_renderer(gfx_rend);
		gfx_rend = NULL;
	}
}

static void
gfx_set_fallbacks(char **list, int n)
{
	int i;

	for (i = 0; i < gfx_nfallbacks; i++)
		free(gfx_fallbacks[i]);
	free(gfx_fallbacks);
	gfx_fallbacks = NULL;
	gfx_nfallbacks = 0;
	if (n <= 0)
		return;
	gfx_fallbacks = ecalloc((size_t)n, sizeof(char *));
	for (i = 0; i < n; i++)
		gfx_fallbacks[gfx_nfallbacks++] = estrdup(list[i]);
}

static struct wld_font *
gfx_open_font(const char *family, int px, int bold)
{
	char key[320];
	int i;

	if (!gfx_fctx)
		return NULL;
	if (!family || !*family)
		family = "monospace";
	if (px < 4)
		px = 4;
	snprintf(key, sizeof(key), "%s:pixelsize=%d%s", family, px, bold ? ":bold" : "");
	for (i = 0; i < gfx_nfonts; i++)
		if (!strcmp(gfx_fonts[i].key, key))
			return gfx_fonts[i].font;
	if (gfx_nfonts >= GFX_MAXFONTS)
		return NULL;
	snprintf(gfx_fonts[gfx_nfonts].key, sizeof(gfx_fonts[0].key), "%s", key);
	gfx_fonts[gfx_nfonts].font = wld_font_open_name(gfx_fctx, key);
	if (!gfx_fonts[gfx_nfonts].font)
		nb_log("cannot open the font '%s'", key);
	return gfx_fonts[gfx_nfonts++].font;
}

static struct wld_font *
gfx_auto_font(uint32_t cp, int px, int bold)
{
	FcPattern *pat, *match;
	FcCharSet *cs, *mcs;
	FcResult res;
	struct wld_font *font = NULL;
	int i;

	for (i = 0; i < gfx_nauto; i++)
		if (gfx_auto[i].cp == cp && gfx_auto[i].px == px && gfx_auto[i].bold == bold)
			return gfx_auto[i].font;

	pat = FcPatternCreate();
	cs = FcCharSetCreate();
	if (pat && cs) {
		FcCharSetAddChar(cs, cp);
		FcPatternAddCharSet(pat, FC_CHARSET, cs);
		FcPatternAddDouble(pat, FC_PIXEL_SIZE, px);
		FcPatternAddBool(pat, FC_SCALABLE, FcTrue);
		if (bold)
			FcPatternAddInteger(pat, FC_WEIGHT, FC_WEIGHT_BOLD);
		FcConfigSubstitute(NULL, pat, FcMatchPattern);
		FcDefaultSubstitute(pat);
		match = FcFontMatch(NULL, pat, &res);
		if (match) {
			if (FcPatternGetCharSet(match, FC_CHARSET, 0, &mcs) == FcResultMatch &&
			    FcCharSetHasChar(mcs, cp))
				font = wld_font_open_pattern(gfx_fctx, match);
			FcPatternDestroy(match);
		}
	}
	if (cs)
		FcCharSetDestroy(cs);
	if (pat)
		FcPatternDestroy(pat);
	if (gfx_nauto < GFX_MAXMISS) {
		gfx_auto[gfx_nauto].cp = cp;
		gfx_auto[gfx_nauto].px = px;
		gfx_auto[gfx_nauto].bold = bold;
		gfx_auto[gfx_nauto].font = font;
		gfx_nauto++;
	} else if (font) {
		wld_font_close(font);
		font = NULL;
	}
	return font;
}

static uint32_t
utf8_next(const char **ps)
{
	const unsigned char *s = (const unsigned char *)*ps;
	uint32_t c = s[0];
	int extra = 0, i;

	if (c < 0x80) {
		(*ps)++;
		return c;
	}
	if ((c & 0xe0) == 0xc0) {
		c &= 0x1f;
		extra = 1;
	} else if ((c & 0xf0) == 0xe0) {
		c &= 0x0f;
		extra = 2;
	} else if ((c & 0xf8) == 0xf0) {
		c &= 0x07;
		extra = 3;
	} else {
		(*ps)++;
		return 0xfffd;
	}
	for (i = 1; i <= extra; i++) {
		if ((s[i] & 0xc0) != 0x80) {
			(*ps)++;
			return 0xfffd;
		}
		c = (c << 6) | (s[i] & 0x3f);
	}
	*ps += extra + 1;
	return c;
}

static struct wld_font *
gfx_font_for(const TextStyle *st, struct wld_font *primary, uint32_t cp)
{
	struct wld_font *f;
	int i;

	if (primary && wld_font_ensure_char(primary, cp))
		return primary;
	for (i = 0; i < gfx_nfallbacks; i++) {
		f = gfx_open_font(gfx_fallbacks[i], st->px, st->bold);
		if (f && f != primary && wld_font_ensure_char(f, cp))
			return f;
	}
	f = gfx_auto_font(cp, st->px, st->bold);
	return f ? f : primary;
}

static int
gfx_walk(const TextStyle *st, uint32_t color, int x, int y, const char *s, int draw)
{
	struct wld_font *primary = gfx_open_font(st->family, st->px, st->bold);
	const char *run = s, *p = s;
	struct wld_font *cur = NULL;
	int start_x = x;

	if (!primary || !s)
		return 0;
	while (1) {
		const char *before = p;
		uint32_t cp = *p ? utf8_next(&p) : 0;
		struct wld_font *f = cp ? gfx_font_for(st, primary, cp) : NULL;

		if (cur && (f != cur || !cp)) {
			struct wld_extents ext = {0};
			size_t len = (size_t)(before - run);

			if (draw)
				wld_draw_text(gfx_rend, cur, color, x, y, run, (uint32_t)len, &ext);
			else
				wld_font_text_extents_n(cur, run, (int32_t)len, &ext);
			x += (int)ext.advance;
			run = before;
		}
		if (!cp)
			break;
		if (!cur || f != cur) {
			cur = f;
			run = before;
		}
	}
	return x - start_x;
}

static int
gfx_text_width(const TextStyle *st, const char *s)
{
	return gfx_walk(st, 0, 0, 0, s, 0);
}

static int
gfx_line_height(const TextStyle *st)
{
	struct wld_font *f = gfx_open_font(st->family, st->px, st->bold);

	return f ? (int)f->height : st->px;
}

static int
gfx_text_draw(const TextStyle *st, uint32_t color, int x, int top, int box_h,
              const char *s)
{
	struct wld_font *f = gfx_open_font(st->family, st->px, st->bold);
	int base;

	if (!f)
		return 0;
	base = top + (box_h - (int)f->height) / 2 + (int)f->ascent;
	return gfx_walk(st, color, x, base, s, 1);
}

static void
gfx_fill(int x, int y, int w, int h, uint32_t color)
{
	if (w > 0 && h > 0)
		wld_fill_rectangle(gfx_rend, color, x, y, (uint32_t)w, (uint32_t)h);
}

static int
utf8_len(const char *s)
{
	int n = 0;

	while (*s) {
		utf8_next(&s);
		n++;
	}
	return n;
}

static void
utf8_ellipsize(char *s, size_t cap, int max)
{
	const char *p = s;
	int n = 0;

	if (max < 1 || cap < 4 || utf8_len(s) <= max)
		return;
	while (*p && n < max - 1 && (size_t)(p - s) + 4 <= cap) {
		const char *q = p;

		utf8_next(&q);
		if ((size_t)(q - s) + 4 > cap)
			break;
		p = q;
		n++;
	}
	memcpy(s + (p - s), "\xe2\x80\xa6", 4);
}

#endif
