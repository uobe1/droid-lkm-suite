// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
// Minimal finit_module loader for QEMU: caller patches vermagic and the
// module_layout CRC.  Do NOT pass IGNORE_MODVERSIONS: it sets versindex=0,
// which makes check_modstruct_version call try_to_force_load() and fail with
// ENOEXEC when CONFIG_MODULE_FORCE_LOAD is off.
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
	const char *args;

	if (argc < 2 || argc > 3) {
		fprintf(stderr, "usage: %s <module.ko> [module-args]\n", argv[0]);
		return 2;
	}
	args = argc == 3 ? argv[2] : "";
	fd = open(argv[1], O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	ret = syscall(SYS_finit_module, fd, args, 0);
	if (ret) {
		fprintf(stderr, "finit_module %s: %s\n", argv[1], strerror(errno));
		close(fd);
		return 1;
	}
	close(fd);
	return 0;
}
