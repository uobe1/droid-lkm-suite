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
		/* Accepted: the xattr handler is present where stock would have none. */
		printf("setxattr succeeded on %s\n", path);
		return 0;
	}

	/*
	 * EOPNOTSUPP and ENOTSUP are the same value on Linux, so only one case.
	 * Either way this is exactly what stock tmpfs without CONFIG_TMPFS_XATTR
	 * returns: no handler for the namespace.
	 */
	if (errno == EOPNOTSUPP) {
		printf("setxattr refused (EOPNOTSUPP), as expected on stock tmpfs\n");
		return 1;
	}
	fprintf(stderr, "setxattr failed unexpectedly: %s\n", strerror(errno));
	return 2;
}
