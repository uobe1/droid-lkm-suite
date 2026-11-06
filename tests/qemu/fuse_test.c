// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * Minimal FUSE daemon: serves one read-only file from a user namespace.
 * Hand-rolled protocol so the test needs no libfuse or musl toolchain.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#define MNT "/mnt/fusetest"
#define FUSE_ROOT_ID 1
#define FILE_NODEID 2

#define FUSE_LOOKUP 1
#define FUSE_FORGET 2
#define FUSE_GETATTR 3
#define FUSE_OPEN 14
#define FUSE_READ 15
#define FUSE_STATFS 17
#define FUSE_RELEASE 18
#define FUSE_FLUSH 25
#define FUSE_INIT 26
#define FUSE_OPENDIR 27
#define FUSE_READDIR 28
#define FUSE_RELEASEDIR 29
#define FUSE_ACCESS 34
#define FUSE_DESTROY 38

struct fuse_in_header {
	uint32_t len;
	uint32_t opcode;
	uint64_t unique;
	uint64_t nodeid;
	uint32_t uid;
	uint32_t gid;
	uint32_t pid;
	uint32_t padding;
};

struct fuse_out_header {
	uint32_t len;
	int32_t error;
	uint64_t unique;
};

struct fuse_attr {
	uint64_t ino;
	uint64_t size;
	uint64_t blocks;
	uint64_t atime;
	uint64_t mtime;
	uint64_t ctime;
	uint32_t atimensec;
	uint32_t mtimensec;
	uint32_t ctimensec;
	uint32_t mode;
	uint32_t nlink;
	uint32_t uid;
	uint32_t gid;
	uint32_t rdev;
	uint32_t blksize;
	uint32_t flags;
};

struct fuse_entry_out {
	uint64_t nodeid;
	uint64_t generation;
	uint64_t entry_valid;
	uint64_t attr_valid;
	uint32_t entry_valid_nsec;
	uint32_t attr_valid_nsec;
	struct fuse_attr attr;
};

struct fuse_attr_out {
	uint64_t attr_valid;
	uint32_t attr_valid_nsec;
	uint32_t dummy;
	struct fuse_attr attr;
};

struct fuse_init_in {
	uint32_t major;
	uint32_t minor;
	uint32_t max_readahead;
	uint32_t flags;
	uint32_t flags2;
	uint32_t unused[11];
};

struct fuse_init_out {
	uint32_t major;
	uint32_t minor;
	uint32_t max_readahead;
	uint32_t flags;
	uint16_t max_background;
	uint16_t congestion_threshold;
	uint32_t max_write;
	uint32_t time_gran;
	uint16_t max_pages;
	uint16_t map_alignment;
	uint32_t flags2;
	uint32_t unused[7];
};

struct fuse_open_out {
	uint64_t fh;
	uint32_t open_flags;
	uint32_t padding;
};

struct fuse_read_in {
	uint64_t fh;
	uint64_t offset;
	uint32_t size;
	uint32_t read_flags;
	uint64_t lock_owner;
	uint32_t flags;
	uint32_t padding;
};

struct fuse_kstatfs {
	uint64_t blocks;
	uint64_t bfree;
	uint64_t bavail;
	uint64_t files;
	uint64_t ffree;
	uint32_t bsize;
	uint32_t namelen;
	uint32_t frsize;
	uint32_t padding;
	uint32_t spare[6];
};

struct fuse_statfs_out {
	struct fuse_kstatfs st;
};

struct fuse_dirent {
	uint64_t ino;
	uint64_t off;
	uint32_t namelen;
	uint32_t type;
	char name[];
};

#define FUSE_DIRENT_ALIGN(x) (((x) + 7) & ~7ULL)
#define FUSE_NAME_OFFSET offsetof(struct fuse_dirent, name)

static const char file_data[] = "hello\n";

static int write_str(const char *path, const char *value)
{
	int fd = open(path, O_WRONLY);
	ssize_t n;

	if (fd < 0)
		return -1;
	n = write(fd, value, strlen(value));
	close(fd);
	return n == (ssize_t)strlen(value) ? 0 : -1;
}

static void fill_attr(struct fuse_attr *a, uint64_t nodeid)
{
	memset(a, 0, sizeof(*a));
	a->ino = nodeid;
	a->blksize = 4096;
	a->uid = 0;
	a->gid = 0;
	if (nodeid == FUSE_ROOT_ID) {
		a->mode = S_IFDIR | 0755;
		a->nlink = 2;
	} else {
		a->mode = S_IFREG | 0644;
		a->nlink = 1;
		a->size = sizeof(file_data) - 1;
		a->blocks = 1;
	}
}

static int reply(int fd, uint64_t unique, int error, const void *payload,
		 size_t len)
{
	struct fuse_out_header out = {
		.len = sizeof(out) + (error ? 0 : len),
		.error = error,
		.unique = unique,
	};
	struct iovec iov[2] = {
		{ .iov_base = &out, .iov_len = sizeof(out) },
		{ .iov_base = (void *)payload, .iov_len = error ? 0 : len },
	};
	ssize_t n = writev(fd, iov, error ? 1 : 2);

	return n == (ssize_t)out.len ? 0 : -1;
}

static int reply_error(int fd, uint64_t unique, int err)
{
	return reply(fd, unique, -err, NULL, 0);
}

static int handle_request(int fd, const char *buf, ssize_t len)
{
	const struct fuse_in_header *in = (const void *)buf;
	const char *payload = buf + sizeof(*in);

	if (len < (ssize_t)sizeof(*in) || in->len > (uint32_t)len)
		return -1;

	switch (in->opcode) {
	case FUSE_INIT: {
		const struct fuse_init_in *init = (const void *)payload;
		struct fuse_init_out out;

		memset(&out, 0, sizeof(out));
		out.major = 7;
		out.minor = init->minor < 31 ? init->minor : 31;
		out.max_readahead = init->max_readahead;
		out.max_write = 4096;
		out.time_gran = 1;
		out.max_pages = 1;
		return reply(fd, in->unique, 0, &out, sizeof(out));
	}
	case FUSE_LOOKUP: {
		struct fuse_entry_out out;

		if (strcmp(payload, "hello"))
			return reply_error(fd, in->unique, ENOENT);
		memset(&out, 0, sizeof(out));
		out.nodeid = FILE_NODEID;
		out.entry_valid = 60;
		out.attr_valid = 60;
		fill_attr(&out.attr, FILE_NODEID);
		return reply(fd, in->unique, 0, &out, sizeof(out));
	}
	case FUSE_GETATTR:
	case 4 /* FUSE_SETATTR */: {
		struct fuse_attr_out out;

		memset(&out, 0, sizeof(out));
		out.attr_valid = 60;
		fill_attr(&out.attr, in->nodeid);
		return reply(fd, in->unique, 0, &out, sizeof(out));
	}
	case FUSE_OPEN: {
		struct fuse_open_out out = { 0 };

		return reply(fd, in->unique, 0, &out, sizeof(out));
	}
	case FUSE_READ: {
		const struct fuse_read_in *ri = (const void *)payload;
		uint64_t off = ri->offset;
		uint32_t size = ri->size;

		if (in->nodeid != FILE_NODEID)
			return reply_error(fd, in->unique, EISDIR);
		if (off >= sizeof(file_data) - 1)
			return reply(fd, in->unique, 0, NULL, 0);
		if (size > sizeof(file_data) - 1 - off)
			size = sizeof(file_data) - 1 - off;
		return reply(fd, in->unique, 0, file_data + off, size);
	}
	case FUSE_OPENDIR: {
		struct fuse_open_out out = { 0 };

		return reply(fd, in->unique, 0, &out, sizeof(out));
	}
	case FUSE_READDIR: {
		const struct fuse_read_in *ri = (const void *)payload;
		char ents[256];
		size_t pos = 0;
		unsigned long off = ri->offset;
		int i;

		for (i = 0; i < 3; i++) {
			const char *name = i == 0 ? "." : i == 1 ? ".." : "hello";
			uint64_t ino = i == 2 ? FILE_NODEID : FUSE_ROOT_ID;
			uint32_t type = i == 2 ? 8 : 4;
			size_t nlen = strlen(name);
			size_t ent = FUSE_DIRENT_ALIGN(FUSE_NAME_OFFSET + nlen);
			struct fuse_dirent *de;

			if ((unsigned long)(i + 1) <= off)
				continue;
			if (pos + ent > sizeof(ents) || pos + ent > ri->size)
				break;
			de = (void *)(ents + pos);
			memset(de, 0, ent);
			de->ino = ino;
			de->off = i + 1;
			de->namelen = nlen;
			de->type = type;
			memcpy(de->name, name, nlen);
			pos += ent;
		}
		return reply(fd, in->unique, 0, ents, pos);
	}
	case FUSE_STATFS: {
		struct fuse_statfs_out out;

		memset(&out, 0, sizeof(out));
		out.st.bsize = 4096;
		out.st.namelen = 255;
		out.st.frsize = 4096;
		return reply(fd, in->unique, 0, &out, sizeof(out));
	}
	case FUSE_FLUSH:
	case FUSE_RELEASE:
	case FUSE_RELEASEDIR:
	case FUSE_ACCESS:
		return reply(fd, in->unique, 0, NULL, 0);
	case FUSE_FORGET:
		return 0;
	case FUSE_DESTROY:
		reply(fd, in->unique, 0, NULL, 0);
		return 1;
	default:
		return reply_error(fd, in->unique, ENOSYS);
	}
}

static int serve_loop(int fd, int start_fd)
{
	char buf[8192];

	if (start_fd >= 0) {
		char c;

		if (read(start_fd, &c, 1) != 1)
			return 1;
		close(start_fd);
	}

	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));
		int ret;

		if (n <= 0)
			return n < 0 ? 1 : 0;
		ret = handle_request(fd, buf, n);
		if (ret > 0)
			return 0;
		if (ret < 0)
			return 1;
	}
}

static int map_id(const char *file, const char *setgroups, unsigned int id)
{
	char map[64];

	if (setgroups && write_str(setgroups, "deny") < 0 && errno != ENOENT)
		return -1;
	snprintf(map, sizeof(map), "0 %u 1\n", id);
	return write_str(file, map);
}

int main(void)
{
	char opts[128], buf[32] = {0};
	int fd, pipefd[2], i, ret;
	uid_t outer_uid = getuid();
	gid_t outer_gid = getgid();
	pid_t pid;

	if (access("/dev/fuse", F_OK) < 0) {
		if (mknod("/dev/fuse", S_IFCHR | 0666, makedev(10, 229)) < 0) {
			perror("mknod /dev/fuse");
			return 1;
		}
	}

	if (mkdir(MNT, 0755) < 0 && errno != EEXIST) {
		perror("fuse mkdir");
		return 1;
	}

	if (unshare(CLONE_NEWUSER) < 0) {
		perror("fuse unshare user");
		return 1;
	}
	if (map_id("/proc/self/uid_map", "/proc/self/setgroups", outer_uid) < 0 ||
	    map_id("/proc/self/gid_map", NULL, outer_gid) < 0) {
		perror("fuse id map");
		return 1;
	}
	if (unshare(CLONE_NEWNS) < 0) {
		perror("fuse unshare mount");
		return 1;
	}

	fd = open("/dev/fuse", O_RDWR);
	if (fd < 0) {
		perror("open /dev/fuse");
		return 1;
	}

	if (pipe(pipefd) < 0) {
		perror("fuse pipe");
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		perror("fuse fork");
		return 1;
	}
	if (pid == 0) {
		close(pipefd[1]);
		_exit(serve_loop(fd, pipefd[0]));
	}
	close(pipefd[0]);

	snprintf(opts, sizeof(opts),
		 "fd=%d,rootmode=40000,user_id=0,group_id=0", fd);
	if (mount("fuse", MNT, "fuse", MS_NOSUID | MS_NODEV, opts) < 0) {
		perror("fuse mount");
		goto fail;
	}
	if (write(pipefd[1], "x", 1) != 1)
		goto fail_umount;
	close(pipefd[1]);
	pipefd[1] = -1;

	for (i = 0; i < 50; i++) {
		int t = open(MNT "/hello", O_RDONLY);

		if (t >= 0) {
			close(t);
			break;
		}
		usleep(100000);
	}
	if (i == 50) {
		fprintf(stderr, "USERNS_TEST_FAIL fuse no file\n");
		goto fail_umount;
	}

	ret = open(MNT "/hello", O_RDONLY);
	if (ret < 0) {
		perror("fuse open hello");
		goto fail_umount;
	}
	if (read(ret, buf, sizeof(buf)) != (ssize_t)(sizeof(file_data) - 1) ||
	    memcmp(buf, file_data, sizeof(file_data) - 1)) {
		fprintf(stderr, "USERNS_TEST_FAIL fuse content\n");
		close(ret);
		goto fail_umount;
	}
	close(ret);

	if (umount2(MNT, 0) < 0 && umount2(MNT, MNT_DETACH) < 0) {
		perror("fuse umount");
		goto fail;
	}
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	printf("USERNS_FUSE_OK\n");
	return 0;

fail_umount:
	umount2(MNT, MNT_DETACH);
fail:
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return 1;
}
