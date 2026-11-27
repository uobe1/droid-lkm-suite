#define _GNU_SOURCE
// SPDX-License-Identifier: GPL-3.0-or-later
//
// ns_hammer: keep ns code paths busy while another process unloads dnsk_ns.
//
// The unload-safety gate used to run on an idle system, which is why it never
// reproduced the real-device crash: on the device a task was inside the module's
// trampoline when the module text went away.  This helper creates that
// condition on purpose: N children, each looping unshare(CLONE_NEWIPC|
// CLONE_NEWPID) and fork(), so the ns entry points are continuously in flight
// while the gate runs `rmmod dnsk_ns`.
//
// Usage: ns_hammer <children> <seconds>
// Prints DNSK_HAMMER_START n=<n> once, then DNSK_HAMMER_LOOPS=<total> when done.
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t stop_flag;

static void on_stop(int sig)
{
	(void)sig;
	stop_flag = 1;
}

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 4;
	int secs = argc > 2 ? atoi(argv[2]) : 30;
	pid_t kids[64];
	unsigned long loops = 0;
	int i;

	if (n < 1 || n > 64)
		n = 4;

	for (i = 0; i < n; i++) {
		pid_t p = fork();

		if (p < 0) {
			perror("fork");
			n = i;
			break;
		}
		if (p == 0) {
			unsigned long mine = 0;

			signal(SIGTERM, on_stop);
			signal(SIGINT, on_stop);
			while (!stop_flag) {
				pid_t q;

				/* bionic does not declare unshare() for all API levels; the
				 * raw syscall is the portable form. */
				syscall(SYS_unshare, CLONE_NEWIPC | CLONE_NEWPID);
				q = fork();
				if (q == 0)
					_exit(0);
				if (q > 0)
					waitpid(q, NULL, 0);
				mine++;
				usleep(1000);
			}
			/* Report the child's own count through the exit status is
			 * awkward; print it, the gate sums with awk if needed. */
			printf("DNSK_HAMMER_CHILD_LOOPS=%lu\n", mine);
			_exit(0);
		}
		kids[i] = p;
	}

	printf("DNSK_HAMMER_START n=%d secs=%d\n", n, secs);
	fflush(stdout);
	sleep((unsigned)secs);

	for (i = 0; i < n; i++)
		kill(kids[i], SIGTERM);
	for (i = 0; i < n; i++)
		waitpid(kids[i], NULL, 0);

	(void)loops;
	printf("DNSK_HAMMER_DONE\n");
	return 0;
}
