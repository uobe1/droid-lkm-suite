// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * selinuxfs only accepts a complete policy image in one write(2); a shell
 * redirection writes in chunks and the second chunk fails with EINVAL.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	struct stat st;
	char *buf;
	ssize_t n, off = 0;
	int in, out;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <policy> <load>\n", argv[0]);
		return 2;
	}

	in = open(argv[1], O_RDONLY);
	if (in < 0) {
		perror(argv[1]);
		return 1;
	}
	if (fstat(in, &st) < 0 || st.st_size <= 0) {
		perror("fstat");
		return 1;
	}

	buf = malloc(st.st_size);
	if (!buf) {
		perror("malloc");
		return 1;
	}
	while (off < st.st_size) {
		n = read(in, buf + off, st.st_size - off);
		if (n <= 0) {
			perror("read");
			return 1;
		}
		off += n;
	}
	close(in);

	out = open(argv[2], O_WRONLY);
	if (out < 0) {
		perror(argv[2]);
		return 1;
	}
	n = write(out, buf, st.st_size);
	if (n != st.st_size) {
		fprintf(stderr, "write %s: %zd/%ld (%s)\n", argv[2], n,
			(long)st.st_size, n < 0 ? strerror(errno) : "short");
		return 1;
	}
	close(out);
	free(buf);
	return 0;
}
