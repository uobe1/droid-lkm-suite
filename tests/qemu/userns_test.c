// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
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

static int map_id(const char *file, const char *setgroups, unsigned int id)
{
	char map[64];

	if (setgroups && write_str(setgroups, "deny") < 0 && errno != ENOENT)
		return -1;
	snprintf(map, sizeof(map), "0 %u 1\n", id);
	return write_str(file, map);
}

int main(void)
{
	unsigned int uid = getuid();
	unsigned int gid = getgid();

	if (unshare(CLONE_NEWUSER) < 0) {
		perror("unshare user");
		return 1;
	}
	if (map_id("/proc/self/uid_map", "/proc/self/setgroups", uid) < 0) {
		perror("uid_map");
		return 1;
	}
	if (map_id("/proc/self/gid_map", NULL, gid) < 0) {
		perror("gid_map");
		return 1;
	}
	if (getuid() != 0 || getgid() != 0) {
		fprintf(stderr, "USERNS_TEST_FAIL unmapped uid=%u gid=%u\n",
			getuid(), getgid());
		return 1;
	}

	if (unshare(CLONE_NEWUSER) < 0) {
		perror("nested unshare user");
		return 1;
	}
	if (map_id("/proc/self/uid_map", "/proc/self/setgroups", 0) < 0) {
		perror("nested uid_map");
		return 1;
	}
	if (map_id("/proc/self/gid_map", NULL, 0) < 0) {
		perror("nested gid_map");
		return 1;
	}
	if (getuid() != 0 || getgid() != 0) {
		fprintf(stderr, "USERNS_TEST_FAIL nested uid=%u gid=%u\n",
			getuid(), getgid());
		return 1;
	}

	printf("USERNS_TEST_OK uid=%u gid=%u\n", getuid(), getgid());
	return 0;
}
