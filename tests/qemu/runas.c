// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Drop to the Android shell uid so KernelSU's --allow-shell grant applies,
 * then exec the su binary.  initramfs init runs as uid 0, which KSU does
 * not necessarily allow-list.
 */
#include <grp.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <program> [args...]\n", argv[0]);
		return 2;
	}
	if (setgroups(0, NULL) < 0 || setgid(2000) < 0 || setuid(2000) < 0) {
		perror("drop privileges");
		return 1;
	}
	execv(argv[1], &argv[1]);
	perror(argv[1]);
	return 127;
}
