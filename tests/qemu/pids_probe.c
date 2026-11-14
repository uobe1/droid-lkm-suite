/*
 * pids_probe - report whether a fork succeeds in the caller's pids cgroup.
 *
 * The caller has already moved itself into a cgroup whose pids.max is 1, so
 * this process is the only charged task.  A fork must fail with EAGAIN (11)
 * there; printing the errno makes the cgroup-pids contract comparable between
 * the native oracle and the virtual controller without letting a failed fork
 * kill the shell that runs the rest of the probe.
 *
 * errno=0 means the fork unexpectedly succeeded.  A different number means
 * the controller enforced a different boundary than the kernel's own pids
 * controller, which is a real semantic difference.
 */
#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <unistd.h>

int main(void)
{
	pid_t pid;
	int status;

	errno = 0;
	pid = fork();
	if (pid < 0) {
		printf("PIDS_FORK_ERRNO=%d\n", errno);
		return 0;
	}
	if (pid == 0)
		_exit(0);

	if (waitpid(pid, &status, 0) < 0) {
		printf("PIDS_FORK_ERRNO=%d\n", errno);
		return 0;
	}
	printf("PIDS_FORK_ERRNO=0\n");
	return 0;
}