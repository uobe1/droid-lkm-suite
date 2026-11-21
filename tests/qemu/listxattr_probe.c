// SPDX-License-Identifier: GPL-3.0-or-later
//
// Print the raw listxattr() result for one path, plus how many times each
// POSIX ACL name appears.
//
// Why this exists: a native tmpfs serves an ACL through the inode cache, so
// listxattr() reports "system.posix_acl_access" exactly once (and only while
// the cache holds an ACL).  A superset implementation that also stores the ACL
// as a real xattr reports it twice: once from posix_acl_listxattr() and once
// from the xattr tree walk.  That duplicate is the observable difference the
// B decision (native only) is about, and no shell tool in the guest can see it.
//
// Usage: listxattr_probe <path>
// Output (one key=value per line, sorted names):
//   LISTXATTR_RC=<0 or errno>
//   LISTXATTR_COUNT=<n>
//   LISTXATTR_NAMES=<comma separated, sorted>
//   ACL_ACCESS_COUNT=<n>     occurrences of system.posix_acl_access
//   ACL_DEFAULT_COUNT=<n>    occurrences of system.posix_acl_default

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/xattr.h>

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

int main(int argc, char **argv)
{
	char *buf;
	char *names[128];
	ssize_t n;
	int i, count = 0, access = 0, deflt = 0;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <path>\n", argv[0]);
		return 2;
	}

	n = listxattr(argv[1], NULL, 0);
	if (n < 0) {
		printf("LISTXATTR_RC=%d\n", errno);
		return 0;
	}
	printf("LISTXATTR_RC=0\n");

	if (n == 0) {
		printf("LISTXATTR_COUNT=0\n");
		printf("LISTXATTR_NAMES=\n");
		printf("ACL_ACCESS_COUNT=0\n");
		printf("ACL_DEFAULT_COUNT=0\n");
		return 0;
	}

	buf = malloc((size_t)n);
	if (!buf)
		return 3;

	n = listxattr(argv[1], buf, (size_t)n);
	if (n < 0) {
		printf("LISTXATTR_RC=%d\n", errno);
		free(buf);
		return 0;
	}

	{
		char *p = buf;
		char *end = buf + n;

		while (p < end && count < (int)(sizeof(names) / sizeof(names[0]))) {
			names[count++] = p;
			p += strlen(p) + 1;
		}
	}

	qsort(names, (size_t)count, sizeof(names[0]), cmp_name);

	printf("LISTXATTR_COUNT=%d\n", count);
	printf("LISTXATTR_NAMES=");
	for (i = 0; i < count; i++) {
		printf("%s%s", i ? "," : "", names[i]);
		if (!strcmp(names[i], "system.posix_acl_access"))
			access++;
		if (!strcmp(names[i], "system.posix_acl_default"))
			deflt++;
	}
	printf("\n");
	printf("ACL_ACCESS_COUNT=%d\n", access);
	printf("ACL_DEFAULT_COUNT=%d\n", deflt);

	free(buf);
	return 0;
}
