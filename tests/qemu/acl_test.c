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
#define ACL_USER 0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

struct acl_entry {
	uint16_t tag;
	uint16_t perm;
	uint32_t id;
};

struct acl_blob {
	uint32_t version;
	struct acl_entry entries[5];
} __attribute__((packed));

static int fail(const char *what)
{
	fprintf(stderr, "ACL_TEST_FAIL %s: %s\n", what, strerror(errno));
	return 1;
}

static int acl_perm(const void *blob, ssize_t n, uint16_t tag)
{
	const unsigned char *p = blob;
	uint32_t version, count, i;

	if (n < 4)
		return -1;
	memcpy(&version, p, 4);
	if (version != 2)
		return -1;
	count = (n - 4) / 8;
	for (i = 0; i < count; i++) {
		struct acl_entry e;

		memcpy(&e, p + 4 + i * 8, sizeof(e));
		if (e.tag == tag)
			return e.perm;
	}
	return -1;
}

static int test_inherit(const char *target, const struct acl_blob *acl)
{
	char dir[256], file[256], sub[256], buf[512];
	int fd;
	ssize_t n;

	snprintf(dir, sizeof(dir), "%s/adir", target);
	if (mkdir(dir, 0700) && errno != EEXIST)
		return fail("inherit mkdir");
	if (setxattr(dir, "system.posix_acl_default", acl, sizeof(*acl), 0) < 0)
		return fail("inherit set default acl");

	snprintf(file, sizeof(file), "%s/f", dir);
	fd = open(file, O_CREAT | O_RDWR, 0666);
	if (fd < 0)
		return fail("inherit create");
	close(fd);
	n = getxattr(file, "system.posix_acl_access", buf, sizeof(buf));
	if (n < 0)
		return fail("inherit access acl missing");
	if (acl_perm(buf, n, ACL_USER) < 0)
		return fail("inherit named user missing");
	if (acl_perm(buf, n, ACL_MASK) < 0)
		return fail("inherit mask missing");

	if (chmod(file, 0600) < 0)
		return fail("inherit chmod");
	n = getxattr(file, "system.posix_acl_access", buf, sizeof(buf));
	if (n < 0)
		return fail("chmod acl missing");
	if (acl_perm(buf, n, ACL_MASK) != 0) {
		fprintf(stderr, "ACL_TEST_FAIL chmod mask %d\n",
			acl_perm(buf, n, ACL_MASK));
		return 1;
	}

	snprintf(sub, sizeof(sub), "%s/sub", dir);
	if (mkdir(sub, 0700) && errno != EEXIST)
		return fail("inherit subdir");
	n = getxattr(sub, "system.posix_acl_default", buf, sizeof(buf));
	if (n < 0)
		return fail("subdir default acl missing");
	printf("ACL_INHERIT_OK\n");
	return 0;
}

int main(int argc, char **argv)
{
	const char *target;
	char path[256], buf[256] = {0};
	struct acl_blob acl = {
		.version = 2,
		.entries = {
			{ ACL_USER_OBJ, 6, 0 },
			{ ACL_USER, 4, 1000 },
			{ ACL_GROUP_OBJ, 4, 0 },
			{ ACL_MASK, 6, 0 },
			{ ACL_OTHER, 4, 0 },
		},
	};
	int fsfd, mfd, fd;
	bool existing;
	bool inherit;
	ssize_t n;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <mountpoint> [existing|inherit]\n",
			argv[0]);
		return 2;
	}
	target = argv[1];
	existing = argc == 3 && !strcmp(argv[2], "existing");
	inherit = argc == 3 && !strcmp(argv[2], "inherit");

	if (!existing && !inherit) {
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

	if (inherit)
		return test_inherit(target, &acl);

	snprintf(path, sizeof(path), "%s/file", target);
	fd = open(path, O_CREAT | O_RDWR, 0600);
	if (fd < 0)
		return fail("create");
	close(fd);

	if (setxattr(path, "system.posix_acl_access", &acl, sizeof(acl), 0) < 0)
		return fail("setxattr acl");
	n = getxattr(path, "system.posix_acl_access", buf, sizeof(buf));
	if (n < (ssize_t)sizeof(acl))
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
