// SPDX-License-Identifier: GPL-3.0-or-later
/* Minimal static HTTP server for the Docker networking gate.  Alpine's
 * minirootfs busybox has no httpd or nc applet, so the container serves from
 * a bind-mounted binary instead of the image.
 */
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	int port = argc > 1 ? atoi(argv[1]) : 80;
	const char *body = argc > 2 ? argv[2] : "dns-ok\n";
	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_addr.s_addr = htonl(INADDR_ANY),
		.sin_port = htons(port),
	};
	int one = 1;
	int s;

	signal(SIGPIPE, SIG_IGN);
	s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0)
		return 1;
	setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) || listen(s, 16))
		return 1;

	for (;;) {
		char req[512], resp[256];
		int c = accept(s, NULL, NULL);
		int n;

		if (c < 0)
			continue;
		read(c, req, sizeof(req));
		n = snprintf(resp, sizeof(resp),
			     "HTTP/1.0 200 OK\r\nContent-Length: %zu\r\n"
			     "Connection: close\r\n\r\n%s",
			     strlen(body), body);
		write(c, resp, n);
		close(c);
	}
}
