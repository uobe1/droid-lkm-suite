/*
 * setns_pidfd_probe - join the IPC namespace of an already-running task by
 * pidfd, without knowing its pid namespace.
 *
 * This is the pidfd path, not /proc/PID/ns: the ns module has a separate hook
 * there, and a target task created before the module was loaded may have no
 * ipc_ns to join on a kernel that ships IPC_NS=n.  Failing closed (EINVAL) is
 * then deliberate; silently dereferencing NULL is not.  Printing rc and errno
 * makes that contract comparable with the native oracle instead of hiding it
 * behind "the syscall returned something".
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/sched.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef SYS_pidfd_open
#define SYS_pidfd_open 434
#endif

int main(void)
{
	int fd;
	long ret;
	int err;

	fd = (int)syscall(SYS_pidfd_open, 1, 0);
	if (fd < 0) {
		printf("SETNS_PIDFD_IPC_RC=pidfd_open_failed\n");
		printf("SETNS_PIDFD_IPC_ERRNO=%d\n", errno);
		return 0;
	}

	errno = 0;
	ret = syscall(SYS_setns, fd, CLONE_NEWIPC);
	err = errno;
	printf("SETNS_PIDFD_IPC_RC=%ld\n", ret);
	printf("SETNS_PIDFD_IPC_ERRNO=%d\n", ret < 0 ? err : 0);
	close(fd);
	return 0;
}