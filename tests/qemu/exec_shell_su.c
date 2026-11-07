// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <grp.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	char *args[64];
	int i;

	if (argc < 2 || argc > 60)
		return 2;

	/* sucompat dispatches on an argv[0] ending in "/su". */
	args[0] = (char *)"/system/bin/su";
	for (i = 1; i < argc; i++)
		args[i] = argv[i];
	args[argc] = NULL;

	/* --allow-shell whitelists uid 2000, not uid 0. */
	if (setgroups(0, NULL) || setgid(2000) || setuid(2000)) {
		perror("drop to shell");
		return 1;
	}
	execv("/system/bin/su", args);
	perror("execv su");
	return 1;
}
