// SPDX-License-Identifier: GPL-3.0-or-later
/* Unprivileged (uid 1000) user-namespace smoke for the userns oracle/gate. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

static int write_str(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, strlen(value));
	close(fd);
	return n == (ssize_t)strlen(value) ? 0 : -1;
}

int main(void)
{
	char map[64];
	uid_t outer;
	int fd;

	if (getuid() == 0) {
		setgroups(0, NULL);
		if (setgid(1000) || setuid(1000)) {
			perror("setuid 1000");
			return 1;
		}
	}
	outer = getuid();
	if (outer == 0) {
		fprintf(stderr, "UNPRIV_USERNS_FAIL still root\n");
		return 1;
	}
	if (unshare(CLONE_NEWUSER) < 0) {
		perror("unshare user");
		return 1;
	}
	if (write_str("/proc/self/setgroups", "deny") < 0 &&
	    errno != ENOENT) {
		perror("setgroups deny");
		return 1;
	}
	snprintf(map, sizeof(map), "0 %u 1\n", (unsigned)outer);
	if (write_str("/proc/self/uid_map", map) < 0) {
		perror("uid_map");
		return 1;
	}
	if (write_str("/proc/self/gid_map", map) < 0) {
		perror("gid_map");
		return 1;
	}
	if (getuid() != 0 || geteuid() != 0) {
		fprintf(stderr, "UNPRIV_USERNS_FAIL mapped uid=%u\n",
			(unsigned)getuid());
		return 1;
	}
	fd = open("/tmp/unpriv_file", O_CREAT | O_RDWR, 0600);
	if (fd < 0) {
		perror("create /tmp/unpriv_file");
		return 1;
	}
	close(fd);
	unlink("/tmp/unpriv_file");
	printf("UNPRIV_USERNS_OK\n");
	return 0;
}
