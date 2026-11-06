/* nibar - status bar for ofwc and other dwl-ipc compositors (layer-shell client). */
#define _GNU_SOURCE
#include <fcntl.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-client.h>

#include "xdg-shell-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "dwl-ipc-unstable-v2-client-protocol.h"
#include "ofwc-ipc-unstable-v1-client-protocol.h"

#include "util.h"
#include "parser.h"
#include "config.h"
#include "gfx.h"
#include "data.h"
#include "bar.h"
#include "wl.h"

#ifndef VERSION
#define VERSION "0.1"
#endif

#define RELOAD_DEBOUNCE_MS 150
#define WATCH_MAXDIRS 2

static int running = 1;
static int sigfd = -1;
static uint64_t reload_at;

static int watch_fd = -1;
static char *watch_name[WATCH_MAXDIRS];
static int watch_n;

static void
do_reload(void)
{
	char *path = user_config_path(cli_config);
	Config next, old;

	reload_at = 0;
	if (config_load(&next, path, 1) < 0) {
		nb_log("config: not reloaded, the current configuration stays");
		free(path);
		return;
	}
	free(path);
	old = cfg;
	cfg = next;
	if (bar_reconfigure() < 0) {
		nb_log("config: the fonts do not work, the current configuration stays");
		config_free(&cfg);
		cfg = old;
		bar_reconfigure();
		return;
	}
	config_free(&old);
	outputs_reconfigure();
	nb_log("config reloaded");
}

static void
watch_add(const char *path)
{
	char dir[PATH_MAX];
	const char *slash = strrchr(path, '/');
	size_t dlen;
	int wd, i;

	if (!slash || watch_n >= WATCH_MAXDIRS || !slash[1])
		return;
	dlen = slash == path ? 1 : (size_t)(slash - path);
	if (dlen >= sizeof(dir))
		return;
	memcpy(dir, path, dlen);
	dir[dlen] = '\0';
	wd = inotify_add_watch(watch_fd, dir, IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE);
	if (wd < 0) {
		nb_log("config watch: cannot watch %s: %s", dir, strerror(errno));
		return;
	}
	for (i = 0; i < watch_n; i++)
		if (watch_name[i] && !strcmp(watch_name[i], slash + 1))
			return;
	watch_name[watch_n++] = estrdup(slash + 1);
}

static void
watch_init(void)
{
	char *path = user_config_path(cli_config);
	char real[PATH_MAX];

	if (!path)
		return;
	watch_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (watch_fd < 0) {
		nb_log("config watch: inotify_init1 failed: %s, no automatic reload", strerror(errno));
		free(path);
		return;
	}
	watch_add(path);
	if (realpath(path, real) && strcmp(real, path))
		watch_add(real);
	if (watch_n == 0) {
		close(watch_fd);
		watch_fd = -1;
	}
	free(path);
}

static int
watch_matches(const char *name)
{
	int i;

	for (i = 0; i < watch_n; i++)
		if (!strcmp(watch_name[i], name))
			return 1;
	return 0;
}

static void
watch_read(void)
{
	char buf[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t n;
	char *p;
	int matched = 0;

	while ((n = read(watch_fd, buf, sizeof(buf))) > 0) {
		for (p = buf; p < buf + n;) {
			struct inotify_event *ev = (struct inotify_event *)p;

			if ((ev->mask & (IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE)) && ev->len > 0 &&
			    watch_matches(ev->name))
				matched = 1;
			p += sizeof(struct inotify_event) + ev->len;
		}
	}
	if (matched)
		reload_at = now_ms() + RELOAD_DEBOUNCE_MS;
}

static void
signals_read(void)
{
	struct signalfd_siginfo si;

	while (read(sigfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
		switch (si.ssi_signo) {
		case SIGHUP:
			reload_at = now_ms();
			break;
		case SIGINT:
		case SIGTERM:
			running = 0;
			break;
		case SIGCHLD: {
			pid_t pid;
			int i;

			while ((pid = waitpid(-1, NULL, WNOHANG)) > 0)
				for (i = 0; i < bar_nel; i++)
					if (bar_el[i].x.pid == pid)
						bar_el[i].x.pid = 0;
			break;
		}
		}
	}
}

static void
signals_init(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGINT);
	sigaddset(&set, SIGTERM);
	sigaddset(&set, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &set, NULL) < 0)
		die("sigprocmask: %s", strerror(errno));
	sigfd = signalfd(-1, &set, SFD_NONBLOCK | SFD_CLOEXEC);
	if (sigfd < 0)
		die("signalfd: %s", strerror(errno));
	signal(SIGPIPE, SIG_IGN);
}

static int
ms_to_next_second(void)
{
	struct timespec ts;
	long ms;

	clock_gettime(CLOCK_REALTIME, &ts);
	ms = 1000 - ts.tv_nsec / 1000000L + 2;
	return (int)MAX(20, MIN(ms, 1000));
}

static void
usage(void)
{
	puts("usage: nibar [-c config.py] [-d] [-v]\n"
	     "  -c file  use this configuration instead of ~/.config/nibar/config.py\n"
	     "  -d       debug messages\n"
	     "  -v       version");
}

int
main(int argc, char *argv[])
{
	char *upath;
	int i, ret = 0;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-c") && i + 1 < argc)
			cli_config = argv[++i];
		else if (!strcmp(argv[i], "-d"))
			nb_debug = 1;
		else if (!strcmp(argv[i], "-v")) {
			puts("nibar " VERSION);
			return 0;
		} else {
			usage();
			return !strcmp(argv[i], "-h") ? 0 : 1;
		}
	}
	setlocale(LC_TIME, "");

	upath = user_config_path(cli_config);
	if (!cli_config)
		config_seed_user_file(upath);
	if (config_load(&cfg, upath, 0) < 0) {
		nb_log("using built-in defaults");
		if (config_load(&cfg, NULL, 0) < 0)
			die("cannot load the built-in configuration");
	}
	free(upath);

	signals_init();

	dpy = wl_display_connect(NULL);
	if (!dpy)
		die("cannot connect to a Wayland compositor (is WAYLAND_DISPLAY set?)");
	registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm)
		die("the compositor lacks wl_compositor or wl_shm");
	if (!layer_shell)
		die("the compositor does not offer wlr-layer-shell");
	if (!ipc_mgr)
		nb_log("the compositor does not offer dwl-ipc-unstable-v2: no tags, no window title");
	if (!ext)
		nb_dbg("no ofwc-ipc: no keyboard layouts, no ofwc: actions");
	if (bar_reconfigure() < 0)
		die("cannot set up the drawing (fonts, pixman)");
	watch_init();
	bars_ready = 1;
	outputs_ensure();

	while (running) {
		struct pollfd pfd[3 + BAR_MAXEL];
		BarElem *pe[BAR_MAXEL];
		int npe = 0, n = 0, timeout, r, want_out = 0;
		uint64_t now;

		while (wl_display_prepare_read(dpy) != 0)
			if (wl_display_dispatch_pending(dpy) < 0)
				goto lost;
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				goto lost;
			}
			want_out = 1;
		}

		pfd[n].fd = wl_display_get_fd(dpy);
		pfd[n].events = POLLIN | (want_out ? POLLOUT : 0);
		pfd[n++].revents = 0;
		pfd[n].fd = sigfd;
		pfd[n].events = POLLIN;
		pfd[n++].revents = 0;
		if (watch_fd >= 0) {
			pfd[n].fd = watch_fd;
			pfd[n].events = POLLIN;
			pfd[n++].revents = 0;
		}
		for (i = 0; i < bar_nel; i++)
			if (bar_el[i].type == BE_EXEC && bar_el[i].x.fd >= 0) {
				pe[npe++] = &bar_el[i];
				pfd[n].fd = bar_el[i].x.fd;
				pfd[n].events = POLLIN;
				pfd[n++].revents = 0;
			}

		timeout = ms_to_next_second();
		if (reload_at) {
			now = now_ms();
			timeout = reload_at <= now ? 0 : (int)MIN((uint64_t)timeout, reload_at - now);
		}
		if (bar_dirty)
			timeout = 0;

		r = poll(pfd, (nfds_t)n, timeout);
		if (r < 0 && errno != EINTR) {
			wl_display_cancel_read(dpy);
			goto lost;
		}
		if (r > 0 && (pfd[0].revents & POLLIN)) {
			if (wl_display_read_events(dpy) < 0)
				goto lost;
		} else {
			wl_display_cancel_read(dpy);
		}
		if (wl_display_dispatch_pending(dpy) < 0)
			goto lost;
		if (r > 0 && (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)))
			goto lost;

		if (r > 0) {
			int k = 1;

			if (pfd[k++].revents & POLLIN)
				signals_read();
			if (watch_fd >= 0 && (pfd[k++].revents & POLLIN))
				watch_read();
			for (i = 0; i < npe; i++, k++)
				if (pfd[k].revents & (POLLIN | POLLHUP | POLLERR))
					exec_read(pe[i]);
		}

		now = now_ms();
		if (reload_at && now >= reload_at)
			do_reload();
		if (bar_refresh(0))
			bar_changed();
		exec_poll(now);
		if (bar_dirty) {
			bar_dirty = 0;
			bars_draw_all();
		}
	}
	goto out;

lost:
	if (wl_display_get_error(dpy))
		nb_dbg("lost the connection to the compositor: %s", strerror(wl_display_get_error(dpy)));
out:
	while (outputs)
		output_destroy(outputs);
	bar_elems_clear();
	wl_display_disconnect(dpy);
	return ret;
}
