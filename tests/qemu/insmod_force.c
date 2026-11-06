// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
// Minimal finit_module loader for QEMU: ignores modversions CRCs (the flag is
// allowed without CONFIG_MODULE_FORCE_LOAD); vermagic is patched in CI.
#include <errno.h>
#include <fcntl.h>
#include <linux/module.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int fd, ret;

	if (argc != 2) {
		fprintf(stderr, "usage: %s <module.ko>\n", argv[0]);
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	ret = syscall(SYS_finit_module, fd, "", MODULE_INIT_IGNORE_MODVERSIONS);
	if (ret) {
		fprintf(stderr, "finit_module %s: %s\n", argv[1], strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}
