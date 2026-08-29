#define _GNU_SOURCE

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define PRODUCTION_SOCKET "/run/rg40xxv-volume/control.sock"

static void usage(const char *program)
{
	(void)fprintf(stderr,
		"usage: %s get\n"
		"       %s set 0..100\n"
		"       %s up|down|mute-toggle\n",
		program, program, program);
}

static int parse_percent(const char *text)
{
	unsigned int value = 0U;

	if (text == NULL || *text == '\0')
		return -1;
	for (const unsigned char *cursor = (const unsigned char *)text;
	     *cursor != '\0'; ++cursor) {
		if (*cursor < (unsigned char)'0' || *cursor > (unsigned char)'9')
			return -1;
		value = value * 10U + (unsigned int)(*cursor - (unsigned char)'0');
		if (value > 100U)
			return -1;
	}
	return (int)value;
}

#ifdef RG40XXV_VOLUME_TESTING
static int test_socket_path(char path[sizeof(((struct sockaddr_un *)0)->sun_path)])
{
	static const char prefix[] = "/tmp/rg40xxv-device-control-test.volume.";
	const char *root = getenv("RG40XXV_VOLUME_TEST_ROOT");
	struct stat status;
	char resolved[PATH_MAX];
	char parent[PATH_MAX];
	int length;

	if (root == NULL || strncmp(root, prefix, sizeof(prefix) - 1U) != 0 ||
	    strchr(root + sizeof(prefix) - 1U, '/') != NULL ||
	    realpath(root, resolved) == NULL || strcmp(root, resolved) != 0 ||
	    lstat(root, &status) != 0 || !S_ISDIR(status.st_mode) ||
	    S_ISLNK(status.st_mode) || (status.st_mode & 0022) != 0) {
		errno = EINVAL;
		return -1;
	}
	length = snprintf(parent, sizeof(parent), "%s/run/rg40xxv-volume", root);
	if (length < 0 || (size_t)length >= sizeof(parent) ||
	    realpath(parent, resolved) == NULL || strcmp(parent, resolved) != 0 ||
	    lstat(parent, &status) != 0 || !S_ISDIR(status.st_mode) ||
	    S_ISLNK(status.st_mode) || (status.st_mode & 0022) != 0) {
		errno = EINVAL;
		return -1;
	}
	length = snprintf(path, sizeof(((struct sockaddr_un *)0)->sun_path),
			  "%s/control.sock", parent);
	if (length < 0 ||
	    (size_t)length >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	return 0;
}
#endif

int main(int argc, char **argv)
{
	char request[64];
	char response[128];
	char socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	struct sockaddr_un address;
	struct stat status;
	int volume;
	int muted;
	int request_length;
	ssize_t response_length;
	int fd;
	char extra;

	if (argc == 2 && (strcmp(argv[1], "get") == 0 ||
			   strcmp(argv[1], "up") == 0 ||
			   strcmp(argv[1], "down") == 0 ||
			   strcmp(argv[1], "mute-toggle") == 0)) {
		request_length = snprintf(request, sizeof(request), "%s", argv[1]);
	} else if (argc == 3 && strcmp(argv[1], "set") == 0 &&
		   parse_percent(argv[2]) >= 0) {
		request_length = snprintf(request, sizeof(request), "set %s", argv[2]);
	} else {
		usage(argv[0]);
		return 2;
	}
	if (request_length < 0 || (size_t)request_length >= sizeof(request))
		return 2;
#ifdef RG40XXV_VOLUME_TESTING
	if (test_socket_path(socket_path) != 0) {
		(void)fprintf(stderr, "rg40xxv-volume-ctl: invalid test root\n");
		return 1;
	}
#else
	(void)snprintf(socket_path, sizeof(socket_path), "%s", PRODUCTION_SOCKET);
#endif
	if (lstat(socket_path, &status) != 0 || !S_ISSOCK(status.st_mode) ||
	    S_ISLNK(status.st_mode) || status.st_uid != 0 ||
	    (status.st_mode & 0077) != 0) {
		(void)fprintf(stderr, "rg40xxv-volume-ctl: secure daemon socket unavailable\n");
		return 1;
	}
	fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
	if (fd < 0) {
		perror("rg40xxv-volume-ctl: socket");
		return 1;
	}
	memset(&address, 0, sizeof(address));
	address.sun_family = AF_UNIX;
	(void)strcpy(address.sun_path, socket_path);
	if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
	    send(fd, request, (size_t)request_length, MSG_NOSIGNAL) != request_length) {
		perror("rg40xxv-volume-ctl: connect/send");
		close(fd);
		return 1;
	}
	response_length = recv(fd, response, sizeof(response) - 1U, MSG_TRUNC);
	close(fd);
	if (response_length <= 0 || (size_t)response_length >= sizeof(response)) {
		(void)fprintf(stderr, "rg40xxv-volume-ctl: invalid daemon response\n");
		return 1;
	}
	response[response_length] = '\0';
	if (sscanf(response, "OK volume_percent=%d muted=%d%c", &volume, &muted,
		   &extra) != 3 || extra != '\n' || volume < 0 || volume > 100 ||
	    (muted != 0 && muted != 1) ||
	    strchr(response, '\n') != response + response_length - 1) {
		(void)fprintf(stderr, "rg40xxv-volume-ctl: %s", response);
		if (response[response_length - 1] != '\n')
			(void)fputc('\n', stderr);
		return 1;
	}
	(void)printf("volume_percent=%d\nmuted=%d\n", volume, muted);
	return 0;
}
