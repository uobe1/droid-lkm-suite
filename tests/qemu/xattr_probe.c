/*
 * xattr_probe - set one user.* xattr on a path and report whether the kernel
 * accepted it.
 *
 *   xattr_probe <path>        exit 0 if the set succeeded, 1 if the kernel
 *                             refused, 2 on unexpected errors
 *
 * Used by the unload gate to tell "tmpfs is back to stock (no xattr support)"
 * from "the module's handler is still wired".  A shell one-liner cannot do this
 * portably: the minimal initramfs has no setfattr and no python.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>

int main(int argc, char **argv)
{
	const char *path;
	const char *name = "user.probe";
	const char *value = "1";
	ssize_t ret;

	if (argc < 2) {
		fprintf(stderr, "usage: %s <path>\n", argv[0]);
		return 2;
	}
	path = argv[1];

	ret = setxattr(path, name, value, strlen(value), 0);
	if (ret == 0) {
		/*
		 * Accepted: the xattr handler is present where stock would have
		 * none.  The plain-text line is kept for the existing gates; the
		 * XATTR_* keys are what the semantic differential compares, and they
		 * cover the whole contract (set/get/list/remove), not just "set
		 * returned 0".
		 */
		char buf[64];
		ssize_t got;
		ssize_t listed;

		printf("setxattr succeeded on %s\n", path);
		printf("XATTR_SET_RC=0\n");

		got = getxattr(path, name, buf, sizeof(buf));
		if (got < 0)
			printf("XATTR_GET_RC=%d\n", errno);
		else
			printf("XATTR_GET_VALUE=%.*s\n", (int)got, buf);

		listed = listxattr(path, buf, sizeof(buf));
		if (listed < 0)
			printf("XATTR_LIST_RC=%d\n", errno);
		else
			printf("XATTR_LIST_HAS_PROBE=%d\n",
			       memmem(buf, (size_t)listed, name, strlen(name)) ? 1 : 0);

		if (removexattr(path, name) < 0)
			printf("XATTR_REMOVE_RC=%d\n", errno);
		else
			printf("XATTR_REMOVE_RC=0\n");

		got = getxattr(path, name, buf, sizeof(buf));
		if (got < 0)
			printf("XATTR_GET_AFTER_REMOVE_RC=%d\n", errno);
		else
			printf("XATTR_GET_AFTER_REMOVE_RC=0\n");

		/*
		 * trusted.* goes through a different handler than user.* and needs
		 * CAP_SYS_ADMIN, so it cannot be inferred from the user.* result.
		 * security.* is deliberately not tested here: the oracle runs with
		 * SELinux disabled while the device side runs permissive with the
		 * device policy loaded, so a difference would be environment, not
		 * tmpfs semantics.
		 */
		if (setxattr(path, "trusted.probe", value, strlen(value), 0) == 0) {
			printf("XATTR_TRUSTED_SET_RC=0\n");
			if (removexattr(path, "trusted.probe") < 0)
				printf("XATTR_TRUSTED_REMOVE_RC=%d\n", errno);
			else
				printf("XATTR_TRUSTED_REMOVE_RC=0\n");
		} else {
			printf("XATTR_TRUSTED_SET_RC=%d\n", errno);
		}
		return 0;
	}

	/*
	 * EOPNOTSUPP and ENOTSUP are the same value on Linux, so only one case.
	 * Either way this is exactly what stock tmpfs without CONFIG_TMPFS_XATTR
	 * returns: no handler for the namespace.
	 */
	if (errno == EOPNOTSUPP) {
		printf("setxattr refused (EOPNOTSUPP), as expected on stock tmpfs\n");
		printf("XATTR_SET_RC=1\n");
		return 1;
	}
	printf("XATTR_SET_RC=2\n");
	fprintf(stderr, "setxattr failed unexpectedly: %s\n", strerror(errno));
	return 2;
}
