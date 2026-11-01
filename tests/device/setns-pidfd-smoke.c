// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * pidfd setns regression: a target task created before dnsk_ns may have
 * nsproxy->ipc_ns == NULL on kernels without IPC_NS/POSIX_MQUEUE.  The hook
 * must fail closed instead of dereferencing it.
 *
 * Build: cc -O2 -static -o setns-pidfd-smoke setns-pidfd-smoke.c
 * Expect on the target device: ret=-1 errno=EINVAL, exit 0.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/sched.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	int fd = (int)syscall(SYS_pidfd_open, 1, 0);
	long ret;

	if (fd < 0) {
		perror("pidfd_open");
		return 2;
	}
	errno = 0;
	ret = syscall(SYS_setns, fd, CLONE_NEWIPC);
	printf("pidfd_setns_ipc ret=%ld errno=%d (%s)\n", ret, errno,
	       strerror(errno));
	close(fd);
	return ret < 0 && errno == EINVAL ? 0 : 1;
}
