/* Logging, memory, numbers and colours. */
#ifndef NIBAR_UTIL_H
#define NIBAR_UTIL_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#define LENGTH(x) (sizeof(x) / sizeof((x)[0]))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAXTAGS 31

static int nb_debug;

static void die(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
static void nb_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void nb_dbg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static void
die(const char *fmt, ...)
{
	va_list ap;

	fputs("nibar: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
	exit(EXIT_FAILURE);
}

static void
nb_log(const char *fmt, ...)
{
	va_list ap;

	fputs("nibar: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void
nb_dbg(const char *fmt, ...)
{
	va_list ap;

	if (!nb_debug)
		return;
	fputs("nibar[debug]: ", stderr);
	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

static void *
ecalloc(size_t nmemb, size_t size)
{
	void *p = calloc(nmemb ? nmemb : 1, size ? size : 1);

	if (!p)
		die("out of memory");
	return p;
}

static void *
erealloc(void *ptr, size_t size)
{
	void *p = realloc(ptr, size ? size : 1);

	if (!p)
		die("out of memory");
	return p;
}

static char *
estrdup(const char *s)
{
	size_t n = strlen(s) + 1;
	char *p = ecalloc(n, 1);

	memcpy(p, s, n);
	return p;
}

static uint64_t
now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static int
parse_color(const char *s, uint32_t *out)
{
	char *end;
	unsigned long v;
	unsigned r, g, b, a;
	size_t digits;

	if (!s)
		return -1;
	while (*s == ' ')
		s++;
	if (s[0] == '#')
		s++;
	else if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		s += 2;
	digits = strspn(s, "0123456789abcdefABCDEF");
	if (digits != 6 && digits != 8)
		return -1;
	v = strtoul(s, &end, 16);
	if (end == s)
		return -1;
	if (digits == 6) {
		r = (v >> 16) & 0xff;
		g = (v >> 8) & 0xff;
		b = v & 0xff;
		a = 0xff;
	} else {
		r = (v >> 24) & 0xff;
		g = (v >> 16) & 0xff;
		b = (v >> 8) & 0xff;
		a = v & 0xff;
	}
	if (a != 0xff) {
		r = r * a / 255;
		g = g * a / 255;
		b = b * a / 255;
	}
	*out = (a << 24) | (r << 16) | (g << 8) | b;
	return 0;
}

static int
parse_bool(const char *s, int *out)
{
	if (!strcasecmp(s, "1") || !strcasecmp(s, "true") || !strcasecmp(s, "yes") ||
	    !strcasecmp(s, "on")) {
		*out = 1;
		return 0;
	}
	if (!strcasecmp(s, "0") || !strcasecmp(s, "false") || !strcasecmp(s, "no") ||
	    !strcasecmp(s, "off")) {
		*out = 0;
		return 0;
	}
	return -1;
}

static int
parse_long(const char *s, long *out)
{
	char *end;
	long v;

	errno = 0;
	v = strtol(s, &end, 0);
	if (end == s || *end || errno)
		return -1;
	*out = v;
	return 0;
}

static int
parse_num(const char *s, long *out)
{
	char *end;
	double d;

	if (!s)
		return -1;
	if (parse_long(s, out) == 0)
		return 0;
	errno = 0;
	d = strtod(s, &end);
	if (end == s || *end || errno || d != d || d > 1e9 || d < -1e9)
		return -1;
	*out = (long)d;
	return 0;
}

#endif
