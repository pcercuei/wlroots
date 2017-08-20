#define _XOPEN_SOURCE 700
#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <errno.h>
#include <wayland-server.h>
#include "wlr/util/log.h"
#include "wlr/xwayland.h"
#include "sockets.h"
#include "xwm.h"

static void safe_close(int fd) {
	if (fd >= 0) {
		close(fd);
	}
}

static int unset_cloexec(int fd) {
	if (fcntl(fd, F_SETFD, 0) != 0) {
		wlr_log_errno(L_ERROR, "fcntl() failed on fd %d", fd);
		return -1;
	}
	return 0;
}

static void exec_xwayland(struct wlr_xwayland *wlr_xwayland) {
	if (unset_cloexec(wlr_xwayland->x_fd[0]) ||
			unset_cloexec(wlr_xwayland->x_fd[1]) ||
			unset_cloexec(wlr_xwayland->wm_fd[1]) ||
			unset_cloexec(wlr_xwayland->wl_fd[1])) {
		exit(EXIT_FAILURE);
	}

	/* Make Xwayland signal us when it's ready */
	signal(SIGUSR1, SIG_IGN);

	char *argv[11] = { 0 };
	argv[0] = "Xwayland";
	if (asprintf(&argv[1], ":%d", wlr_xwayland->display) < 0) {
		wlr_log_errno(L_ERROR, "asprintf failed");
		exit(EXIT_FAILURE);
	}
	argv[2] = "-rootless";
	argv[3] = "-terminate";
	argv[4] = "-listen";
	if (asprintf(&argv[5], "%d", wlr_xwayland->x_fd[0]) < 0) {
		wlr_log_errno(L_ERROR, "asprintf failed");
		exit(EXIT_FAILURE);
	}
	argv[6] = "-listen";
	if (asprintf(&argv[7], "%d", wlr_xwayland->x_fd[1]) < 0) {
		wlr_log_errno(L_ERROR, "asprintf failed");
		exit(EXIT_FAILURE);
	}
	argv[8] = "-wm";
	if (asprintf(&argv[9], "%d", wlr_xwayland->wm_fd[1]) < 0) {
		wlr_log_errno(L_ERROR, "asprintf failed");
		exit(EXIT_FAILURE);
	}

	const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
	if (!xdg_runtime) {
		wlr_log(L_ERROR, "XDG_RUNTIME_DIR is not set");
		exit(EXIT_FAILURE);
	}

	char *envp[3] = { 0 };
	if (asprintf(&envp[0], "XDG_RUNTIME_DIR=%s", xdg_runtime) < 0 ||
			asprintf(&envp[1], "WAYLAND_SOCKET=%d", wlr_xwayland->wl_fd[1]) < 0) {
		wlr_log_errno(L_ERROR, "asprintf failed");
		exit(EXIT_FAILURE);
	}

	wlr_log(L_INFO, "Xwayland :%d -rootless -terminate -listen %d -listen %d -wm %d",
			wlr_xwayland->display, wlr_xwayland->x_fd[0], wlr_xwayland->x_fd[1],
			wlr_xwayland->wm_fd[1]);

	// TODO: close stdout/err depending on log level

	execvpe("Xwayland", argv, envp);
}

static bool wlr_xwayland_init(struct wlr_xwayland *wlr_xwayland,
		struct wl_display *wl_display, struct wlr_compositor *compositor);
static void wlr_xwayland_finish(struct wlr_xwayland *wlr_xwayland);

static void xwayland_destroy_event(struct wl_listener *listener, void *data) {
	struct wl_client *client = data;
	struct wlr_xwayland *wlr_xwayland = wl_container_of(client, wlr_xwayland, client);

	/* don't call client destroy */
	wlr_xwayland->client = NULL;
	wlr_xwayland_finish(wlr_xwayland);

	if (wlr_xwayland->server_start - time(NULL) > 5) {
		wlr_xwayland_init(wlr_xwayland, wlr_xwayland->wl_display,
				wlr_xwayland->compositor);
	}
}

static struct wl_listener xwayland_destroy_listener = {
	.notify = xwayland_destroy_event,
};

static void wlr_xwayland_finish(struct wlr_xwayland *wlr_xwayland) {

	if (wlr_xwayland->client) {
		wl_list_remove(&xwayland_destroy_listener.link);
		wl_client_destroy(wlr_xwayland->client);
	}

	if (wlr_xwayland->sigusr1_source) {
		wl_event_source_remove(wlr_xwayland->sigusr1_source);
	}

	xwm_destroy(wlr_xwayland->xwm);

	safe_close(wlr_xwayland->x_fd[0]);
	safe_close(wlr_xwayland->x_fd[1]);
	safe_close(wlr_xwayland->wl_fd[0]);
	safe_close(wlr_xwayland->wl_fd[1]);
	safe_close(wlr_xwayland->wm_fd[0]);
	safe_close(wlr_xwayland->wm_fd[1]);

	unlink_display_sockets(wlr_xwayland->display);
	unsetenv("DISPLAY");
	/* kill Xwayland process? */
}

static int xserver_handle_ready(int signal_number, void *data) {
	struct wlr_xwayland *wlr_xwayland = data;

	wlr_log(L_DEBUG, "Xserver is ready");

	wlr_xwayland->xwm = xwm_create(wlr_xwayland);
	if (!wlr_xwayland->xwm) {
		wlr_xwayland_finish(wlr_xwayland);
		return 1;
	}

	wl_event_source_remove(wlr_xwayland->sigusr1_source);
	wlr_xwayland->sigusr1_source = NULL;

	char display_name[16];
	snprintf(display_name, sizeof(display_name), ":%d", wlr_xwayland->display);
	setenv("DISPLAY", display_name, true);

	return 1;
}

static bool wlr_xwayland_init(struct wlr_xwayland *wlr_xwayland,
		struct wl_display *wl_display, struct wlr_compositor *compositor) {
	memset(wlr_xwayland, 0, sizeof(struct wlr_xwayland));
	wlr_xwayland->wl_display = wl_display;
	wlr_xwayland->compositor = compositor;
	wlr_xwayland->x_fd[0] = wlr_xwayland->x_fd[1] = -1;
	wlr_xwayland->wl_fd[0] = wlr_xwayland->wl_fd[1] = -1;
	wlr_xwayland->wm_fd[0] = wlr_xwayland->wm_fd[1] = -1;

	wlr_xwayland->display = open_display_sockets(wlr_xwayland->x_fd);
	if (wlr_xwayland->display < 0) {
		wlr_xwayland_finish(wlr_xwayland);
		return false;
	}
	if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wlr_xwayland->wl_fd) != 0 ||
			socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, wlr_xwayland->wm_fd) != 0) {
		wlr_log_errno(L_ERROR, "failed to create socketpair");
		wlr_xwayland_finish(wlr_xwayland);
		return false;
	}

	if ((wlr_xwayland->pid = fork()) == 0) {
		exec_xwayland(wlr_xwayland);
		wlr_log_errno(L_ERROR, "execvpe failed");
		exit(EXIT_FAILURE);
	}

	if (wlr_xwayland->pid < 0) {
		wlr_log_errno(L_ERROR, "fork failed");
		wlr_xwayland_finish(wlr_xwayland);
		return false;
	}

	/* close child fds */
	close(wlr_xwayland->x_fd[0]);
	close(wlr_xwayland->x_fd[1]);
	close(wlr_xwayland->wl_fd[1]);
	close(wlr_xwayland->wm_fd[1]);
	wlr_xwayland->x_fd[0] = wlr_xwayland->x_fd[1] = -1;
	wlr_xwayland->wl_fd[1] = wlr_xwayland->wm_fd[1] = -1;

	wlr_xwayland->server_start = time(NULL);

	if (!(wlr_xwayland->client = wl_client_create(wl_display, wlr_xwayland->wl_fd[0]))) {
		wlr_log_errno(L_ERROR, "wl_client_create failed");
		wlr_xwayland_finish(wlr_xwayland);
		return false;
	}

	wl_client_add_destroy_listener(wlr_xwayland->client, &xwayland_destroy_listener);

	struct wl_event_loop *loop = wl_display_get_event_loop(wl_display);
	wlr_xwayland->sigusr1_source = wl_event_loop_add_signal(loop, SIGUSR1, xserver_handle_ready, wlr_xwayland);

	return true;
}

void wlr_xwayland_destroy(struct wlr_xwayland *wlr_xwayland) {
	wlr_xwayland_finish(wlr_xwayland);
	free(wlr_xwayland);
}

struct wlr_xwayland *wlr_xwayland_create(struct wl_display *wl_display,
		struct wlr_compositor *compositor) {
	struct wlr_xwayland *wlr_xwayland = calloc(1, sizeof(struct wlr_xwayland));
	if (wlr_xwayland_init(wlr_xwayland, wl_display, compositor)) {
		return wlr_xwayland;
	}
	free(wlr_xwayland);
	return NULL;
}
