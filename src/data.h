/* Dynamic element sources: clock, battery, network, commands. */
#ifndef NIBAR_DATA_H
#define NIBAR_DATA_H

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

static void bar_changed(void);

#define US_SEP '\x1f'
#define RS_SEP '\x1e'

static const char *
kv_get(const BarElemCfg *c, const char *key)
{
	size_t i;

	for (i = c->nkv; i >= 2; i -= 2)
		if (!strcmp(c->kv[i - 2], key))
			return c->kv[i - 1];
	return NULL;
}

static char *
kv_str(const BarElemCfg *c, const char *key, const char *def)
{
	const char *v = kv_get(c, key);

	if (!v || !strcmp(v, "NULL"))
		return def ? estrdup(def) : NULL;
	return estrdup(v);
}

static int
kv_int(const BarElemCfg *c, const char *key, int def)
{
	const char *v = kv_get(c, key);
	long l;

	return (v && parse_num(v, &l) == 0) ? (int)MAX(-100000, MIN(100000, l)) : def;
}

static int
kv_bool(const BarElemCfg *c, const char *key, int def)
{
	const char *v = kv_get(c, key);
	int b;

	return (v && parse_bool(v, &b) == 0) ? b : def;
}

static uint32_t
kv_color(const BarElemCfg *c, const char *key, uint32_t def, int *has)
{
	const char *v = kv_get(c, key);
	uint32_t col;

	if (v && parse_color(v, &col) == 0) {
		if (has)
			*has = 1;
		return col;
	}
	if (has)
		*has = 0;
	return def;
}

static char **
list_split(const char *v, int *n)
{
	char **out = NULL;
	const char *p = v, *q;
	int cnt = 0;

	*n = 0;
	if (!v || !*v || !strcmp(v, "NULL"))
		return NULL;
	while (1) {
		q = strchr(p, US_SEP);
		out = erealloc(out, (size_t)(cnt + 1) * sizeof(char *));
		out[cnt] = q ? strndup(p, (size_t)(q - p)) : estrdup(p);
		if (!out[cnt])
			die("out of memory");
		cnt++;
		if (!q)
			break;
		p = q + 1;
	}
	*n = cnt;
	return out;
}

static void
list_free(char **l, int n)
{
	int i;

	for (i = 0; i < n; i++)
		free(l[i]);
	free(l);
}

static void
kv_pair(const BarElemCfg *c, const char *key, int *l, int *r)
{
	const char *v = kv_get(c, key);
	char **it;
	int n;
	long a, b;

	if (!v)
		return;
	it = list_split(v, &n);
	if (n >= 2 && parse_num(it[0], &a) == 0 && parse_num(it[1], &b) == 0) {
		*l = (int)MAX(0, MIN(a, 1000));
		*r = (int)MAX(0, MIN(b, 1000));
	} else if (n == 1 && parse_num(it[0], &a) == 0) {
		*l = *r = (int)MAX(0, MIN(a, 1000));
	}
	list_free(it, n);
}

typedef struct {
	const char *name;
	char val[160];
} FmtVar;

static void
fmt_set(FmtVar *v, const char *name, const char *val)
{
	v->name = name;
	snprintf(v->val, sizeof(v->val), "%s", val ? val : "");
}

static void
fmt_expand(const char *fmt, const FmtVar *v, int nv, char *out, size_t cap)
{
	size_t o = 0;
	const char *p = fmt;

	if (!cap)
		return;
	while (*p && o + 1 < cap) {
		if (*p == '{') {
			const char *e = strchr(p + 1, '}');

			if (e) {
				size_t nl = (size_t)(e - p - 1);
				int i;

				for (i = 0; i < nv; i++)
					if (strlen(v[i].name) == nl && !strncmp(v[i].name, p + 1, nl))
						break;
				if (i < nv) {
					size_t vl = strlen(v[i].val);

					if (o + vl + 1 > cap)
						vl = cap - o - 1;
					memcpy(out + o, v[i].val, vl);
					o += vl;
					p = e + 1;
					continue;
				}
			}
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
}

static void
clock_expand(const char *fmt, char *out, size_t cap)
{
	time_t t = time(NULL);
	struct tm tm;
	size_t o = 0;
	const char *p = fmt;

	localtime_r(&t, &tm);
	if (!cap)
		return;
	if (!strchr(fmt, '{')) {
		if (!strchr(fmt, '%'))
			snprintf(out, cap, "%s", fmt);
		else if (!strftime(out, cap, fmt, &tm))
			out[0] = '\0';
		return;
	}
	while (*p && o + 1 < cap) {
		if (p[0] == '{' && p[1] == ':') {
			const char *e = strchr(p + 2, '}');
			char pat[96], piece[160];
			const char *in = p + 2;
			size_t n;

			if (e) {
				if (*in == 'L')
					in++;
				n = (size_t)(e - in);
				if (n >= sizeof(pat))
					n = sizeof(pat) - 1;
				memcpy(pat, in, n);
				pat[n] = '\0';
				piece[0] = '\0';
				if (strftime(piece, sizeof(piece), pat, &tm)) {
					size_t pl = strlen(piece);

					if (o + pl + 1 > cap)
						pl = cap - o - 1;
					memcpy(out + o, piece, pl);
					o += pl;
				}
				p = e + 1;
				continue;
			}
		}
		out[o++] = *p++;
	}
	out[o] = '\0';
}

static const struct {
	const char *name;
	uint32_t cp;
} glyph_names[] = {
	{"logo_void_nerdfont", 0xf32e},
	{"logo_arch_nerdfont", 0xf303},
	{"logo_debian_nerdfont", 0xf306},
	{"logo_fedora_nerdfont", 0xf30a},
	{"logo_gentoo_nerdfont", 0xf30d},
	{"logo_nixos_nerdfont", 0xf313},
	{"logo_ubuntu_nerdfont", 0xf31b},
	{"logo_linux_nerdfont", 0xf17c},
	{"icon_power_nerdfont", 0xf011},
	{"icon_volume_nerdfont", 0xf028},
	{"icon_wifi_nerdfont", 0xf1eb},
};

static int
utf8_encode(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xc0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3f));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xe0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
		out[2] = (char)(0x80 | (cp & 0x3f));
		return 3;
	}
	out[0] = (char)(0xf0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
	out[3] = (char)(0x80 | (cp & 0x3f));
	return 4;
}

static char *
glyph_alias(const char *s)
{
	size_t i;
	char buf[8];

	for (i = 0; i < LENGTH(glyph_names); i++)
		if (!strcmp(s, glyph_names[i].name)) {
			buf[utf8_encode(glyph_names[i].cp, buf)] = '\0';
			return estrdup(buf);
		}
	return estrdup(s);
}

static int
read_file(const char *path, char *buf, size_t cap)
{
	FILE *fp = fopen(path, "r");
	size_t n;

	if (!fp)
		return -1;
	n = fread(buf, 1, cap - 1, fp);
	fclose(fp);
	buf[n] = '\0';
	while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r' || buf[n - 1] == ' '))
		buf[--n] = '\0';
	return 0;
}

static int
read_in(const char *dir, const char *file, char *buf, size_t cap)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/%s", dir, file);
	return read_file(path, buf, cap);
}

static long
read_in_long(const char *dir, const char *file, long def)
{
	char buf[64];
	long l;

	if (read_in(dir, file, buf, sizeof(buf)) < 0 || parse_long(buf, &l) < 0)
		return def;
	return l;
}

#ifndef PS_DIR
#define PS_DIR "/sys/class/power_supply"
#endif

typedef struct {
	int ok;
	int capacity;
	char status[32];
	long now, full, rate;
	int ac;
} BatInfo;

static int
bat_find(const char *want, char *out, size_t cap)
{
	DIR *d = opendir(PS_DIR);
	struct dirent *de;
	int found = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		char dir[PATH_MAX], type[32], scope[32];

		if (de->d_name[0] == '.')
			continue;
		if (want && *want && strcmp(want, de->d_name))
			continue;
		snprintf(dir, sizeof(dir), PS_DIR "/%s", de->d_name);
		if (read_in(dir, "type", type, sizeof(type)) < 0 || strcasecmp(type, "Battery"))
			continue;
		if (read_in(dir, "scope", scope, sizeof(scope)) == 0 && !strcasecmp(scope, "Device"))
			continue;
		snprintf(out, cap, "%s", dir);
		found = 1;
		break;
	}
	closedir(d);
	return found;
}

static int
ac_online(void)
{
	DIR *d = opendir(PS_DIR);
	struct dirent *de;
	int on = 0;

	if (!d)
		return 0;
	while ((de = readdir(d))) {
		char dir[PATH_MAX], type[32];

		if (de->d_name[0] == '.')
			continue;
		snprintf(dir, sizeof(dir), PS_DIR "/%s", de->d_name);
		if (read_in(dir, "type", type, sizeof(type)) == 0 && !strcasecmp(type, "Mains") &&
		    read_in_long(dir, "online", 0) == 1) {
			on = 1;
			break;
		}
	}
	closedir(d);
	return on;
}

static void
bat_read(const char *want, BatInfo *b)
{
	char dir[PATH_MAX];

	memset(b, 0, sizeof(*b));
	if (!bat_find(want, dir, sizeof(dir)))
		return;
	b->ok = 1;
	b->capacity = (int)MAX(0, MIN(100, read_in_long(dir, "capacity", 0)));
	if (read_in(dir, "status", b->status, sizeof(b->status)) < 0)
		snprintf(b->status, sizeof(b->status), "Unknown");
	b->now = read_in_long(dir, "energy_now", -1);
	b->full = read_in_long(dir, "energy_full", -1);
	b->rate = read_in_long(dir, "power_now", -1);
	if (b->now < 0 || b->rate < 0) {
		b->now = read_in_long(dir, "charge_now", 0);
		b->full = read_in_long(dir, "charge_full", 0);
		b->rate = read_in_long(dir, "current_now", 0);
	}
	if (b->rate < 0)
		b->rate = -b->rate;
	b->ac = ac_online();
}

enum { NET_DOWN, NET_WIFI, NET_ETH };

static int
iface_is_virtual(const char *name)
{
	char path[PATH_MAX], target[PATH_MAX];
	ssize_t n;

	snprintf(path, sizeof(path), "/sys/class/net/%s", name);
	n = readlink(path, target, sizeof(target) - 1);
	if (n < 0)
		return 0;
	target[n] = '\0';
	return strstr(target, "/virtual/") != NULL;
}

static int
iface_is_up(const char *name)
{
	char dir[PATH_MAX], st[32];

	snprintf(dir, sizeof(dir), "/sys/class/net/%s", name);
	if (read_in(dir, "operstate", st, sizeof(st)) < 0)
		return 0;
	if (!strcmp(st, "up"))
		return 1;
	return !strcmp(st, "unknown") && read_in_long(dir, "carrier", 0) == 1;
}

static int
iface_kind(const char *name)
{
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "/sys/class/net/%s/wireless", name);
	if (access(path, F_OK) == 0)
		return NET_WIFI;
	snprintf(path, sizeof(path), "/sys/class/net/%s/phy80211", name);
	return access(path, F_OK) == 0 ? NET_WIFI : NET_ETH;
}

static int
default_route_iface(char *out, size_t cap)
{
	FILE *fp = fopen("/proc/net/route", "r");
	char line[256], best[64] = "";
	unsigned long best_metric = ~0ul;

	if (!fp)
		return 0;
	if (!fgets(line, sizeof(line), fp)) {
		fclose(fp);
		return 0;
	}
	while (fgets(line, sizeof(line), fp)) {
		char name[64], dest[16];
		unsigned long flags, metric;

		if (sscanf(line, "%63s %15s %*s %lx %*s %*s %lu", name, dest, &flags, &metric) == 4 &&
		    !strcmp(dest, "00000000") && (flags & 1) && metric < best_metric) {
			best_metric = metric;
			snprintf(best, sizeof(best), "%s", name);
		}
	}
	fclose(fp);
	if (!*best)
		return 0;
	snprintf(out, cap, "%s", best);
	return 1;
}

static int
net_read(const char *want, char *ifname, size_t cap)
{
	DIR *d;
	struct dirent *de;
	char name[64];
	int kind = NET_DOWN, best_kind = NET_DOWN;

	ifname[0] = '\0';
	if (want && *want) {
		if (iface_is_up(want)) {
			snprintf(ifname, cap, "%s", want);
			return iface_kind(want);
		}
		return NET_DOWN;
	}
	if (default_route_iface(name, sizeof(name)) && iface_is_up(name) &&
	    !iface_is_virtual(name)) {
		snprintf(ifname, cap, "%s", name);
		return iface_kind(name);
	}
	d = opendir("/sys/class/net");
	if (!d)
		return NET_DOWN;
	while ((de = readdir(d))) {
		if (de->d_name[0] == '.' || !strcmp(de->d_name, "lo") ||
		    iface_is_virtual(de->d_name) || !iface_is_up(de->d_name))
			continue;
		kind = iface_kind(de->d_name);
		if (best_kind == NET_DOWN || kind == NET_ETH) {
			best_kind = kind;
			snprintf(ifname, cap, "%s", de->d_name);
		}
	}
	closedir(d);
	return best_kind;
}

typedef struct {
	pid_t pid;
	int fd;
	char buf[2048];
	size_t blen;
	char line[256];
	uint64_t next_ms;
} ExecState;

typedef struct BarElem BarElem;

static void exec_line_changed(BarElem *e);
static void exec_finish(BarElem *e);

#endif
