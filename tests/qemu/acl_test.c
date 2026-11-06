// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/xattr.h>
#include <unistd.h>

#ifndef FSCONFIG_SET_FLAG
#define FSCONFIG_SET_FLAG 0
#endif
#ifndef FSCONFIG_CMD_CREATE
#define FSCONFIG_CMD_CREATE 6
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
#endif

#define ACL_USER_OBJ 0x01
#define ACL_GROUP_OBJ 0x04
#define ACL_OTHER 0x20

struct acl_entry {
	uint16_t tag;
	uint16_t perm;
	uint32_t id;
};

struct acl_blob {
	uint32_t version;
	struct acl_entry entries[3];
} __attribute__((packed));

static int fail(const char *what)
{
	fprintf(stderr, "ACL_TEST_FAIL %s: %s\n", what, strerror(errno));
	return 1;
}

int main(int argc, char **argv)
{
	const char *target;
	char path[256], buf[256] = {0};
	struct acl_blob acl = {
		.version = 2,
		.entries = {
			{ ACL_USER_OBJ, 6, 0 },
			{ ACL_GROUP_OBJ, 4, 0 },
			{ ACL_OTHER, 4, 0 },
		},
	};
	int fsfd, mfd, fd;
	bool existing;
	ssize_t n;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <mountpoint> [existing]\n", argv[0]);
		return 2;
	}
	target = argv[1];
	existing = argc == 3 && !strcmp(argv[2], "existing");

	if (!existing) {
		fsfd = syscall(SYS_fsopen, "tmpfs", 0);
		if (fsfd < 0)
			return fail("fsopen");
		if (syscall(SYS_fsconfig, fsfd, FSCONFIG_SET_FLAG, "acl",
			    NULL, 0) < 0)
			return fail("fsconfig acl");
		if (syscall(SYS_fsconfig, fsfd, FSCONFIG_CMD_CREATE, NULL,
			    NULL, 0) < 0)
			return fail("fsconfig create");
		mfd = syscall(SYS_fsmount, fsfd, 0, 0);
		if (mfd < 0)
			return fail("fsmount");
		if (syscall(SYS_move_mount, mfd, "", AT_FDCWD, target,
			    MOVE_MOUNT_F_EMPTY_PATH) < 0)
			return fail("move_mount");
		close(mfd);
		close(fsfd);
	}

	snprintf(path, sizeof(path), "%s/file", target);
	fd = open(path, O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return fail("create");
	close(fd);

	if (setxattr(path, "system.posix_acl_access", &acl, sizeof(acl), 0) < 0)
		return fail("setxattr acl");
	n = getxattr(path, "system.posix_acl_access", buf, sizeof(buf));
	if (n < (ssize_t)(sizeof(uint32_t) + 3 * sizeof(struct acl_entry)))
		return fail("getxattr acl short");
	if (memcmp(buf, &acl, sizeof(acl)))
		fprintf(stderr, "note: ACL blob was normalized\n");
	if (removexattr(path, "system.posix_acl_access") < 0)
		return fail("removexattr acl");
	if (getxattr(path, "system.posix_acl_access", buf, sizeof(buf)) >= 0 ||
	    errno != ENODATA) {
		fprintf(stderr, "ACL_TEST_FAIL acl still present\n");
		return 1;
	}

	printf("ACL_TEST_OK\n");
	return 0;
}
