// SPDX-License-Identifier: GPL-3.0-only
//
// Print the raw listxattr() result for one path, plus how many times each
// POSIX ACL name appears.
//
// Why this exists: a native tmpfs serves an ACL through the inode cache, so
// listxattr() reports "system.posix_acl_access" exactly once (and only while
// the cache holds an ACL).  An xattr-backed superset reports it twice: once
// from posix_acl_listxattr() and once from the xattr tree walk.  That duplicate
// is the observable that separates the B decision (native only) from the
// superset, and no shell tool in the guest can see it.
//
// Contract:
//   LISTXATTR_RC=<0 or errno>
//   LISTXATTR_COUNT=<number of entries>      -- parsed from the buffer, by NUL
//   LISTXATTR_NAMES=<comma separated, sorted> -- exactly COUNT entries
//   LISTXATTR_SECURITY_SKIPPED=<n>           -- entries removed (security.*)
//   ACL_ACCESS_COUNT / ACL_DEFAULT_COUNT     -- occurrences among the kept names
//
// COUNT and NAMES come from the same parse, so they can never disagree; that
// inconsistency (COUNT=14 with an empty NAMES) is what the previous version
// produced and this rewrite removes.
//
// security.* is excluded because the differential runs the oracle with
// selinux=0 and the device with a policy loaded: a label difference says
// nothing about tmpfs ACL/xattr semantics.  The excluded count is reported so
// the omission stays visible.
//
// Usage: listxattr_probe <path>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/xattr.h>

#define MAX_ENTRIES 256

static int cmp_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/*
 * Fetch the raw name list.  Returns 0 on success and stores the byte count in
 * *len; returns -1 with errno set otherwise.
 *
 * listxattr() can legitimately fail with ERANGE between the sizing call and the
 * fetch (a lazily materialised ACL adds a name), so the whole size/fetch pair is
 * retried, up to three times, always with a freshly measured size.
 */
static int fetch_names(const char *path, char **buf_out, ssize_t *len_out)
{
	char *buf = NULL;
	int attempt;

	for (attempt = 0; attempt < 3; attempt++) {
		ssize_t size, n;

		size = listxattr(path, NULL, 0);
		if (size < 0)
			goto fail;
		if (size == 0) {
			*buf_out = NULL;
			*len_out = 0;
			return 0;
		}

		buf = malloc((size_t)size);
		if (!buf) {
			errno = ENOMEM;
			return -1;
		}

		n = listxattr(path, buf, (size_t)size);
		if (n >= 0) {
			*buf_out = buf;
			*len_out = n;
			return 0;
		}
		free(buf);
		buf = NULL;
		if (errno != ERANGE)
			goto fail;
	}

	errno = ERANGE;
fail:
	return -1;
}

int main(int argc, char **argv)
{
	char *buf = NULL;
	ssize_t len = 0;
	const char *names[MAX_ENTRIES];
	int total = 0, kept = 0, skipped = 0, access = 0, deflt = 0;
	char *p, *end;
	int i;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <path>\n", argv[0]);
		return 2;
	}

	if (fetch_names(argv[1], &buf, &len) < 0) {
		printf("LISTXATTR_RC=%d\n", errno);
		return 0;
	}
	printf("LISTXATTR_RC=0\n");

	/* Parse every NUL-separated entry; total is the true entry count. */
	p = buf;
	end = buf + len;
	while (p && p < end) {
		size_t l = strlen(p);

		if (l == 0)
			break;
		if (total < MAX_ENTRIES)
			names[total] = p;
		total++;
		p += l + 1;
	}

	if (total > MAX_ENTRIES)
		total = MAX_ENTRIES;

	qsort((void *)names, (size_t)total, sizeof(names[0]), cmp_name);

	printf("LISTXATTR_NAMES=");
	for (i = 0; i < total; i++) {
		if (!strncmp(names[i], "security.", 9)) {
			skipped++;
			continue;
		}
		printf("%s%s", kept ? "," : "", names[i]);
		kept++;
		if (!strcmp(names[i], "system.posix_acl_access"))
			access++;
		if (!strcmp(names[i], "system.posix_acl_default"))
			deflt++;
	}
	printf("\n");
	printf("LISTXATTR_COUNT=%d\n", kept);
	printf("LISTXATTR_SECURITY_SKIPPED=%d\n", skipped);
	printf("ACL_ACCESS_COUNT=%d\n", access);
	printf("ACL_DEFAULT_COUNT=%d\n", deflt);

	free(buf);
	return 0;
}
