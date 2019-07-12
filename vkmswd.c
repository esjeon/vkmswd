/*
 * Copyright © 2019 Eon S. Jeon <esjeon@hyunmu.am>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the “Software”), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */


#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <libgen.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <libevdev/libevdev.h>
#include <libevdev/libevdev-uinput.h>

#define SOCKET_PATH "/var/run/vkmswd.socket"

enum DeviceType {
	DEVTYPE_KBD = 'K',
	DEVTYPE_MOUSE = 'M',
};

enum ConnResponseCode {
	CONN_RESP_OKAY,
	CONN_RESP_FAIL,
	CONN_RESP_EPARSE,
	CONN_RESP_ECOMMAND,
	CONN_RESP_EPARAM,
	//TODO: CONN_RESP_ENUMPARAM,
};

enum ConnReadlineStatus {
	CONN_RLINE_EOF = 0,
	CONN_RLINE_AGAIN = -1,
	CONN_RLINE_ERR = -2,
	CONN_RLINE_EBUFSIZE = -3,
};


typedef struct SrcDevice {
	struct libevdev *dev;
	char *devpath;
	int fd;
	int type; 

	struct SrcDevice *next;
} SrcDevice;

typedef struct DestDevice {
	struct libevdev_uinput *uidev;
	char *linkpath;
} DestDevice;

typedef struct DestPair {
	char *name;
	DestDevice *mouse;
	DestDevice *kbd;

	struct DestPair *next;
} DestPair;

typedef struct Conn {
	int sock;
	char rbuf[1024];
	int rbuflen;
	char wbuf[1024];
	int wbuflen;

	struct Conn *next;
} Conn;


static void app_atexit(void);
static void app_cycle(void);
static void app_usage(char *argv0);
static int conn_accept();
// TODO: conn_flush(Conn *conn);
static int conn_handle(Conn *conn);
static int conn_listen(void);
static int conn_readline(Conn *conn, char *buf, size_t bufsize);
static int conn_write(Conn *conn, const char *str);
static int conn_writeresp(Conn *conn, int code, const char *data);
static int destdev_create(int type, const char *name, DestDevice **ret_ddev);
static void destdev_free(DestDevice *ddev);
static int destpair_add(const char *name);
static void destpair_free(DestPair *dest);
static void destpair_rm(DestPair *dest, const char *name);
static void event_loop();
static void event_oninput(SrcDevice *src, struct input_event ev);
static int signal_block(void);
static void signal_noop(int n);
static int srcdev_add(int type, const char *devpath);
static void srcdev_free(SrcDevice *src);
static void srcdev_rm(SrcDevice *src, const char *devpath);
//static int src_readev(SrcDevice *src, struct input_event *ev);
static int strsplit(char *str, char *parts[], int nparts);


static struct option longopts[] = {
	{"keyboard", required_argument, 0, 'K'},
	{"mouse", required_argument, 0, 'M'},
	{0, 0, 0, 0},
};

static SrcDevice *srcs;
static DestPair *dests;
static DestPair *current;

static bool keydown[KEY_CNT];
static int keycnt;

static Conn *conns;
static int lsock;

static int should_quit;


void
app_atexit(void)
{
	SrcDevice *src;
	DestPair *dest;
	Conn *conn;
	void *next;

	if (lsock != 0)
		close(lsock);

	unlink(SOCKET_PATH);

	for (conn = conns; conn; conn = next) {
		next = conn->next;
		shutdown(conn->sock, SHUT_RDWR);
		free(conn);
	}

	for (src = srcs; src; src = next) {
		next = src->next;
		srcdev_free(src);
	}

	for (dest = dests; dest; dest = next) {
		next = dest->next;
		destpair_free(dest);
	}
}

void
app_cycle(void)
{
	SrcDevice *p;
	int key;
	
	// TODO: always grab source devices (UI env must use destination devices)

	for (key = 0; key < KEY_CNT && keycnt > 0; key++) {
		if (keydown[key]) {
			if (current) {
				libevdev_uinput_write_event(current->kbd->uidev,
						EV_KEY, key, 0);
			}
			keydown[key] = false;
			keycnt--;
		}
	}
	if (current)
		libevdev_uinput_write_event(current->kbd->uidev, EV_SYN, SYN_REPORT, 0);

	// TODO: enable/disable destination
	current = (current == NULL)? dests: current->next;
	fprintf(stderr, "current=%s\n", (current)? current->name: "NULL");

	for (p = srcs; p; p = p->next)
		libevdev_grab(p->dev, (current)? LIBEVDEV_GRAB: LIBEVDEV_UNGRAB);

	// TODO: restore the previous keypress states on the current destination
}

void
app_usage(char *argv0)
{
	char *progname = basename(argv0);
	fprintf(stderr,
		"%s: [-M|--mouse NODE] [-K|--keyboard NODE] [-s SOCKET]\n"
		"    [-D DEST-ID] [-h|--help]\n",
		progname);
}

int
conn_accept()
{
	Conn *conn;
	int sock;

	if ((sock = accept(lsock, NULL, NULL)) == -1) {
		perror("accept");
		return 1;
	}

	if ((conn = (Conn *)calloc(1, sizeof(Conn))) == NULL) {
		perror("calloc");
		shutdown(sock, SHUT_RDWR);
		return 1;
	}

	conn->sock = sock;

	conn->next = conns;
	conns = conn;

	return 0;
}

int
conn_handle(Conn *conn)
{
	char buf[128];
	char *parts[64];
	char *msg;
	char c;
	int nparts;
	int code;
	int rc;

	for(;;) {
		rc = conn_readline(conn, buf, sizeof(buf));
		if (rc == CONN_RLINE_AGAIN)
			break;
		else if (rc <= 0)
			return -1;
		buf[rc - 1] = '\0';

		nparts = strsplit(buf, parts, 64);
		if (nparts < 0) {
			conn_writeresp(conn, CONN_RESP_EPARSE, NULL);
			continue;
		}

		code = CONN_RESP_OKAY;
		msg = NULL;

		// TODO: implement commands
		if (0) {
		} else if (strcmp(parts[0], "src_add") == 0) {
			if (nparts != 3)
				goto err_param;

			c = parts[1][0];
			if (c == 'k' || c == 'K')
				rc = srcdev_add(DEVTYPE_KBD, parts[2]);
			else if (c == 'm' || c == 'M')
				rc = srcdev_add(DEVTYPE_MOUSE, parts[2]);
			else
				goto err_param;

			code = (rc == 0)? CONN_RESP_OKAY: CONN_RESP_FAIL;

		} else if (strcmp(parts[0], "src_rm") == 0) {
			if (nparts != 2)
				goto err_param;
			srcdev_rm(NULL, parts[1]);

		} else if (strcmp(parts[0], "dest_add") == 0) {
			if (nparts != 2)
				goto err_param;
			if (destpair_add(parts[1]) != 0)
				code = CONN_RESP_FAIL;

		} else if (strcmp(parts[0], "dest_rm") == 0) {
			if (nparts != 2)
				goto err_param;
			destpair_rm(NULL, parts[1]);

		} else if (strcmp(parts[0], "quit") == 0) {
			should_quit = 1;
			break;

		} else
			goto err_command;
		// TODO: "get_namespace"

		rc = conn_writeresp(conn, code, NULL);
		continue;

err_param:
		rc = conn_writeresp(conn, CONN_RESP_EPARAM, NULL);
		continue;

err_command:
		rc = conn_writeresp(conn, CONN_RESP_ECOMMAND, NULL);
		continue;
	}

	return 0;
}

int
conn_listen(void)
{
	// TODO: support systemd socket activation? (man 3 sd_lsocks)

	struct sockaddr_un addr;

	if ((lsock = socket(AF_UNIX, SOCK_STREAM, 0)) == -1) {
		perror("socket");
		return 1;
	}

	memset(&addr, '\0', sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

	if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
		close(lsock);
		perror("bind");
		return 1;
	}

	if (listen(lsock, 5) == -1) {
		shutdown(lsock, SHUT_RDWR);
		perror("listen");
		return 1;
	}

	return 0;
}

int
conn_readline(Conn *conn, char *buf, size_t bufsize)
{
	int cnt;
	int i;

	for(;;) {
		for (i = 0; i < conn->rbuflen && conn->rbuf[i] != '\n'; i++);
		if (conn->rbuf[i] == '\n')
			break;

		cnt = recv(conn->sock, conn->rbuf + conn->rbuflen,
			sizeof(conn->rbuf) - conn->rbuflen, MSG_DONTWAIT);
		if (cnt == 0)
			return CONN_RLINE_EOF;
		else if (cnt == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return CONN_RLINE_AGAIN;
			return CONN_RLINE_ERR;
		}

		conn->rbuflen += cnt;
	}

	if ((i += 1) >= bufsize)
		return CONN_RLINE_EBUFSIZE;
	strncpy(buf, conn->rbuf, i);
	buf[i] = '\0';

	conn->rbuflen -= i;
	memmove(conn->rbuf, &conn->rbuf[i], conn->rbuflen);
	return i;
}

int
conn_write(Conn *conn, const char *str)
{
	int len;

	len = strlen(str);
	// TODO: write to wbuf, and flush when ready
	send(conn->sock, str, len, 0);

	return 0;
}

int
conn_writeresp(Conn *conn, int code, const char *data)
{
	char buf[128];
	int rc;

	snprintf(buf, sizeof(buf), "!%d\n", code);
	if ((rc = conn_write(conn, buf)) != 0)
		return rc;

	switch((enum ConnResponseCode)code) {
	case CONN_RESP_OKAY:
	case CONN_RESP_FAIL:
		return 0;
	case CONN_RESP_EPARSE:
		snprintf(buf, sizeof(buf), "#parsing failed\n");
		break;
	case CONN_RESP_ECOMMAND:
		snprintf(buf, sizeof(buf), "#invalid command\n");
		break;
	case CONN_RESP_EPARAM:
		snprintf(buf, sizeof(buf), "#invalid parameter\n");
		break;
	}

	return conn_write(conn, buf);
}

int
destdev_create(int type, const char *name, DestDevice **ret_ddev)
{
	static char namebuf[128];
	static char pathbuf[256];

	const char *typestr = (type == DEVTYPE_MOUSE)? "mouse": "kbd";
	DestDevice *ddev;
	struct libevdev *dev = NULL;
	int key;
	int rc;

	if ((ddev = (DestDevice *)calloc(1, sizeof(DestDevice))) == NULL) {
		perror("calloc");
		return 1;
	}

	if ((dev = libevdev_new()) == NULL) {
		perror("libevdev_new");
		free(ddev);
		return 1;
	}

	if (type == DEVTYPE_MOUSE) {
		libevdev_enable_event_type(dev, EV_ABS);
		libevdev_enable_event_code(dev, EV_ABS, ABS_X, NULL);
		libevdev_enable_event_code(dev, EV_ABS, ABS_Y, NULL);

		libevdev_enable_event_type(dev, EV_REL);
		libevdev_enable_event_code(dev, EV_REL, REL_X, NULL);
		libevdev_enable_event_code(dev, EV_REL, REL_Y, NULL);
		libevdev_enable_event_code(dev, EV_REL, REL_WHEEL, NULL);

		libevdev_enable_event_type(dev, EV_KEY);
		libevdev_enable_event_code(dev, EV_KEY, BTN_LEFT     , NULL);
		libevdev_enable_event_code(dev, EV_KEY, BTN_RIGHT    , NULL);
		libevdev_enable_event_code(dev, EV_KEY, BTN_MIDDLE   , NULL);
		libevdev_enable_event_code(dev, EV_KEY, BTN_GEAR_UP  , NULL);
		libevdev_enable_event_code(dev, EV_KEY, BTN_GEAR_DOWN, NULL);
		libevdev_enable_event_code(dev, EV_KEY, BTN_SIDE     , NULL);
		libevdev_enable_event_code(dev, EV_KEY, BTN_EXTRA    , NULL);
	} else /* keyboard */ {
		// TODO: better keyboard device init logic
		libevdev_enable_event_type(dev, EV_KEY);
		for (key = 0; key < 255; key++)
			libevdev_enable_event_code(dev, EV_KEY, key, NULL);
	}

	// TODO: use daemon namespace - allow running multiple daemons
	//       -> exclusive lock on the name
	snprintf(namebuf, sizeof(namebuf), "vkmswd-%s-%s", name, typestr);
	libevdev_set_name(dev, namebuf);

	/* create uinput device */
	rc = libevdev_uinput_create_from_device(dev, LIBEVDEV_UINPUT_OPEN_MANAGED, &ddev->uidev);
	if (rc != 0) {
		fprintf(stderr, "can't create uinput device \"%s\": code %d\n", name, rc);
		goto cleanup;
	} else {
		libevdev_free(dev);
	}

	/* create link */
	// TODO: configurable link path?
	snprintf(pathbuf, sizeof(pathbuf), "/dev/input/by-id/vkmswd-%s-event-%s", name, typestr);
	if ((ddev->linkpath = strdup(pathbuf)) == NULL) {
		perror("strdup");
		goto cleanup;
	}

	unlink(ddev->linkpath);
	if (symlink(libevdev_uinput_get_devnode(ddev->uidev), ddev->linkpath) != 0) {
		perror("symlink");
		goto cleanup;
	}

	*ret_ddev = ddev;

	return 0;

cleanup:
	libevdev_free(dev);
	destdev_free(ddev);
	return 1;
}

void
destdev_free(DestDevice *ddev)
{
	if (ddev->linkpath) {
		unlink(ddev->linkpath);
		free(ddev->linkpath);
	}
	if (ddev->uidev)
		libevdev_uinput_destroy(ddev->uidev);
	free(ddev);
}

int
destpair_add(const char *name)
{
	DestPair *dest;
	DestPair **p;

	if ((dest = (DestPair *)calloc(1, sizeof(DestPair))) == NULL) {
		perror("calloc");
		return 1;
	}

	if ((dest->name = strdup(name)) == NULL) {
		perror("strdup");
		goto cleanup;
	}

	if (destdev_create(DEVTYPE_MOUSE, name, &dest->mouse) != 0)
		goto cleanup;
	if (destdev_create(DEVTYPE_KBD, name, &dest->kbd) != 0)
		goto cleanup;

	/* append to the list */
	for (p = &dests; *p; p = &(*p)->next);
	*p = dest;

	return 0;

cleanup:
	destpair_free(dest);
	return 1;
}

void
destpair_free(DestPair *dest)
{
	if (dest->mouse)
		destdev_free(dest->mouse);
	if (dest->kbd)
		destdev_free(dest->kbd);
	if (dest->name)
		free(dest->name);
	free(dest);
}

void
destpair_rm(DestPair *dest, const char *name)
{
	DestPair **p;

	for(p = &dests; *p; p = &(*p)->next) {
		if (dest && (*p) == dest)
			break;
		if (name && strcmp((*p)->name, name) == 0)
			break;
	}

	if (*p) {
		dest = *p;
		*p = (*p)->next;
		destpair_free(dest);
	}
}

void
event_loop()
{
	struct input_event ev;
	sigset_t emptyset;
	fd_set rfds;
	SrcDevice *src;
	Conn **connp;
	Conn *conn;
	int max;
	int rc;

	// TODO: remove srcdev which is removed from system

	sigemptyset(&emptyset);

	while(!should_quit) {
		// TODO: init wfds
		FD_ZERO(&rfds);
		FD_SET(lsock, &rfds);
		for (src = srcs, max = lsock; src; src = src->next) {
			FD_SET(src->fd, &rfds);
			max = (src->fd > max)? src->fd: max;
		}
		for (conn = conns; conn; conn = conn->next) {
			FD_SET(conn->sock, &rfds);
			max = (conn->sock > max)? conn->sock: max;
		}

		if (pselect(max + 1, &rfds, NULL, NULL, NULL, &emptyset) == -1) {
			if (errno != EINTR)
				perror("pselect");
			break;
		}

		if (FD_ISSET(lsock, &rfds))
			conn_accept();

		for (src = srcs; src; src = src->next) {
			if (!FD_ISSET(src->fd, &rfds))
				continue;

			for(;;) {
				rc = libevdev_next_event(src->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
				if (rc == LIBEVDEV_READ_STATUS_SYNC)
					continue;
				if (rc == -EAGAIN)
					break;

				event_oninput(src, ev);
			}
		}

		for (connp = &conns; *connp; connp = &(*connp)->next) {
			if (!FD_ISSET((*connp)->sock, &rfds))
				continue;

			conn = *connp;
			if (conn_handle(conn) != 0) {
				*connp = (*connp)->next;

				shutdown(conn->sock, SHUT_RDWR);
				free(conn);
				continue;
			}
		}

		// TODO: ISSET wfds -> flush
	}
}

void
event_oninput(SrcDevice *src, struct input_event ev)
{
	/* ignore scan code */
	if (ev.type == EV_MSC)
		return;

	if (ev.type == EV_KEY && ev.code == KEY_SCROLLLOCK) {
		if (ev.value == 0) {
			app_cycle();
		}
	} else {
		/* save key state */
		if (src->type == DEVTYPE_KBD && ev.type == EV_KEY) {
			if (!keydown[ev.code] && ev.value == 1) {
				keydown[ev.code] = true;
				keycnt++;
			} else if (keydown[ev.code] && ev.value == 0) {
				keydown[ev.code] = false;
				keycnt--;
			}
		}

		/* pass event */
		if (current) {
			// TODO: check return code?
			libevdev_uinput_write_event(
					(src->type == DEVTYPE_MOUSE)?
						current->mouse->uidev:
						current->kbd->uidev,
					ev.type, ev.code, ev.value);
		}
	}
}

int
signal_block(void)
{
	struct sigaction sa;
	sigset_t blockset;
	int rc;

	/* block terminating signals */
	sigemptyset(&blockset);
	sigaddset(&blockset, SIGINT);
	sigaddset(&blockset, SIGTERM);
	sigaddset(&blockset, SIGUSR1);
	sigaddset(&blockset, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &blockset, NULL) < 0) {
		perror("sigprocmask");
		return 1;
	}

	sa.sa_handler = signal_noop,
	sa.sa_flags = 0,
	sigemptyset(&sa.sa_mask);
	rc = sigaction(SIGINT, &sa, NULL);
	rc |= sigaction(SIGTERM, &sa, NULL);
	rc |= sigaction(SIGUSR1, &sa, NULL);
	rc |= sigaction(SIGUSR2, &sa, NULL);
	if (rc != 0) {
		perror("sigaction");
		return 1;
	}

	return 0;
}

void
signal_noop(int n)
{
	/* do nothing */
}


int
srcdev_add(int type, const char *devpath)
{
	SrcDevice *src;
	int rc;

	// TODO: dedup - readlink? major/minor?
	for (src = srcs; src != NULL; src = src->next) {
		if (strcmp(src->devpath, devpath) == 0)
			return 0;
	}

	if ((src = (SrcDevice *)calloc(1, sizeof(SrcDevice))) == NULL) {
		perror("calloc");
		return 1;
	}

	if ((src->devpath = strdup(devpath)) == NULL) {
		perror("strdup");
		goto cleanup;
	}

	if ((src->fd = open(devpath, O_RDONLY | O_NONBLOCK)) < 0) {
		fprintf(stderr, "can't open %s: %s\n", devpath, strerror(errno));
		goto cleanup;
	}

	if ((rc = libevdev_new_from_fd(src->fd, &src->dev)) != 0) {
		fprintf(stderr, "can't create evdev device from %s: rc=%d\n", devpath, rc);
		goto cleanup;
	}

	src->type = type;

	src->next = srcs;
	srcs = src;

	return 0;

cleanup:
	srcdev_free(src);
	return 1;
}

void
srcdev_free(SrcDevice *src)
{
	if (src->dev != NULL)
		libevdev_free(src->dev);
	if (src->fd != 0)
		close(src->fd);
	if (src->devpath != NULL)
		free(src->devpath);
	free(src);
}

void
srcdev_rm(SrcDevice *src, const char *devpath)
{
	SrcDevice *p;
	SrcDevice *prev;

	for (p = srcs, prev = NULL; p; p = p->next) {
		if (src && p == src)
			break;
		if (devpath && strcmp(src->devpath, devpath) == 0)
			break;
		prev = p;
	}

	if (p) {
		prev->next = p->next;
		srcdev_free(p);
	}
}

int
strsplit(char *str, char *parts[], int nparts)
{
	char *tok;
	char *save = NULL;
	int i;

	for (i = 0; i < nparts; i++, str = NULL) {
		tok = strtok_r(str, " ", &save);
		if (tok == NULL)
			break;
		parts[i] = tok;
	}
	if (i == nparts)
		return -1;

	return i;
}

int
main(int argc, char *argv[])
{
	int rc;

	if (signal_block() != 0)
		exit(EXIT_FAILURE);

	if (atexit(app_atexit) != 0) {
		fprintf(stderr, "atexit failed\n");
		exit(EXIT_FAILURE);
	}

	/* parse arguments */
	for(;;) {
		if((rc = getopt_long(argc, argv, "hK:M:N:s:", longopts, NULL)) == -1)
			break;

		switch(rc) {
		//case 'd': // daemonize
		case 'h':
			app_usage(argv[0]);
			exit(EXIT_SUCCESS);
		case 'K':
			if (srcdev_add(DEVTYPE_KBD, optarg) != 0)
				exit(EXIT_FAILURE);
			break;
		case 'M':
			if (srcdev_add(DEVTYPE_MOUSE, optarg) != 0)
				exit(EXIT_FAILURE);
			break;
		case 'N': // Name
			fprintf(stderr, "creating destpair \"%s\"\n", optarg);
			if (destpair_add(optarg) != 0)
				exit(EXIT_FAILURE);
		case 's':
			// TODO: socket open
			break;
		case '?':
		default:
			app_usage(argv[0]);
			exit(EXIT_FAILURE);
		}
	}

	if (conn_listen() != 0)
		exit(EXIT_FAILURE);

	event_loop();
	fprintf(stderr, "main loop terminated...\n");

	exit(EXIT_SUCCESS);
}

