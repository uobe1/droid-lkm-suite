// SPDX-License-Identifier: GPL-3.0-or-later
/* Fixed-affinity getpid loop for the KPM vs LKM overhead evaluation. */
#define _GNU_SOURCE
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	long n = argc > 1 ? atol(argv[1]) : 10000000;
	cpu_set_t set;
	struct timespec a, b;
	volatile long sum = 0;
	double ns;
	long i;

	CPU_ZERO(&set);
	CPU_SET(0, &set);
	sched_setaffinity(0, sizeof(set), &set);

	clock_gettime(CLOCK_MONOTONIC, &a);
	for (i = 0; i < n; i++)
		sum += getpid();
	clock_gettime(CLOCK_MONOTONIC, &b);

	ns = (b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec);
	printf("getpid_bench n=%ld ns_total=%.0f ns_per_op=%.2f sum=%ld\n",
	       n, ns, ns / n, sum);
	return 0;
}
