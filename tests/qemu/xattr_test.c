// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/xattr.h>

static int fail(const char *what)
{
	fprintf(stderr, "XATTR_FAIL %s: %s\n", what, strerror(errno));
	return 1;
}

int main(int argc, char **argv)
{
	const char *path;
	char value[64] = {0};
	char list[256] = {0};
	ssize_t n;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path> [keep]\n", argv[0]);
		return 2;
	}
	path = argv[1];

	if (setxattr(path, "user.dnsk", "hello", 5, 0) < 0)
		return fail("setxattr");
	if (argc > 2 && !strcmp(argv[2], "keep"))
		return 0;

	n = getxattr(path, "user.dnsk", value, sizeof(value));
	if (n != 5 || memcmp(value, "hello", 5))
		return fail("getxattr");
	if (setxattr(path, "user.dnsk", "world", 5, 0) < 0)
		return fail("replace");
	n = getxattr(path, "user.dnsk", value, sizeof(value));
	if (n != 5 || memcmp(value, "world", 5))
		return fail("getxattr-replaced");

	n = listxattr(path, list, sizeof(list));
	if (n < 0)
		return fail("listxattr");
	if (!memmem(list, n, "user.dnsk", 9)) {
		fprintf(stderr, "XATTR_FAIL list missing user.dnsk\n");
		return 1;
	}
	if (removexattr(path, "user.dnsk") < 0)
		return fail("removexattr");
	if (getxattr(path, "user.dnsk", value, sizeof(value)) >= 0 ||
	    errno != ENODATA) {
		fprintf(stderr, "XATTR_FAIL remove not effective\n");
		return 1;
	}
	printf("XATTR_OK %s\n", path);
	return 0;
}
