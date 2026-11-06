// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifndef SYS_mount_setattr
#define SYS_mount_setattr 442
#endif
#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif

struct mount_attr {
	unsigned long long attr_set;
	unsigned long long attr_clr;
	unsigned long long propagation;
	unsigned long long userns_fd;
};

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

static int map_id(const char *file, const char *setgroups, unsigned int id)
{
	char map[64];

	if (setgroups && write_str(setgroups, "deny") < 0 && errno != ENOENT)
		return -1;
	snprintf(map, sizeof(map), "0 %u 1\n", id);
	return write_str(file, map);
}

/*
 * A tmpfs file owned by kuid 1 must appear as uid 0 through a mount whose
 * idmap is a userns mapping 0 -> kuid 1.  The child holds that userns; the
 * parent (root in the initial userns) writes the maps and uses its ns fd.
 */
static int idmap_test(void)
{
	char byte, path[64];
	struct mount_attr attr = { .attr_set = MOUNT_ATTR_IDMAP };
	struct stat st;
	int pipefd[2], ufd, fd, ret;
	pid_t pid;

	if (pipe(pipefd) < 0)
		return perror("idmap pipe"), 1;

	pid = fork();
	if (pid < 0)
		return perror("idmap fork"), 1;
	if (pid == 0) {
		close(pipefd[0]);
		if (unshare(CLONE_NEWUSER) < 0) {
			perror("idmap child unshare");
			_exit(1);
		}
		byte = 'x';
		if (write(pipefd[1], &byte, 1) != 1)
			_exit(1);
		for (;;)
			pause();
	}
	close(pipefd[1]);
	if (read(pipefd[0], &byte, 1) != 1) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap child died\n");
		kill(pid, SIGKILL);
		waitpid(pid, NULL, 0);
		return 1;
	}
	close(pipefd[0]);

	snprintf(path, sizeof(path), "/proc/%d/setgroups", pid);
	if (write_str(path, "deny") < 0 && errno != ENOENT)
		goto fail;
	snprintf(path, sizeof(path), "/proc/%d/uid_map", pid);
	if (write_str(path, "0 1 1\n") < 0)
		goto fail;
	snprintf(path, sizeof(path), "/proc/%d/gid_map", pid);
	if (write_str(path, "0 1 1\n") < 0)
		goto fail;
	snprintf(path, sizeof(path), "/proc/%d/ns/user", pid);
	ufd = open(path, O_RDONLY);
	if (ufd < 0)
		goto fail;
	attr.userns_fd = ufd;

	if (mkdir("/tmp/idmap", 0755) < 0 && errno != EEXIST)
		goto fail;
	if (mount("tmpfs", "/tmp/idmap", "tmpfs", 0, "size=1M") < 0)
		goto fail;
	fd = open("/tmp/idmap/f", O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		goto fail_umount;
	close(fd);
	if (chown("/tmp/idmap/f", 1, 1) < 0)
		goto fail_umount;
	if (stat("/tmp/idmap/f", &st) < 0 || st.st_uid != 1) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap pre uid=%u\n",
			(unsigned)st.st_uid);
		goto fail_umount;
	}

	ret = syscall(SYS_mount_setattr, AT_FDCWD, "/tmp/idmap", 0,
		      &attr, sizeof(attr));
	if (ret < 0) {
		perror("mount_setattr idmap");
		goto fail_umount;
	}
	if (stat("/tmp/idmap/f", &st) < 0) {
		perror("idmap stat");
		goto fail_umount;
	}
	if (st.st_uid != 0) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap uid=%u\n",
			(unsigned)st.st_uid);
		goto fail_umount;
	}
	umount("/tmp/idmap");
	close(ufd);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	printf("USERNS_IDMAP_OK\n");
	return 0;

fail_umount:
	umount("/tmp/idmap");
fail:
	fprintf(stderr, "USERNS_TEST_FAIL idmap_setup: %s\n", strerror(errno));
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return 1;
}

/*
 * OverlayFS mounted from the (emulated) user namespace on a tmpfs whose
 * s_user_ns is that same namespace.  userxattr keeps the upper xattrs in
 * user.* so the tmpfs xattr provider can carry them.
 */
static int overlay_test(void)
{
	char opts[256], buf[32] = {0};
	int fd, ret;

	if (mkdir("/tmp/ovl", 0755) < 0 && errno != EEXIST)
		return perror("ovl mkdir"), 1;
	if (mount("tmpfs", "/tmp/ovl", "tmpfs", 0, "size=8M") < 0)
		return perror("ovl tmpfs"), 1;
	if (mkdir("/tmp/ovl/lower", 0755) < 0 ||
	    mkdir("/tmp/ovl/upper", 0755) < 0 ||
	    mkdir("/tmp/ovl/work", 0755) < 0 ||
	    mkdir("/tmp/ovl/merged", 0755) < 0)
		return perror("ovl dirs"), 1;

	fd = open("/tmp/ovl/lower/base", O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		return perror("ovl lower file"), 1;
	if (write(fd, "base", 4) != 4) {
		close(fd);
		return perror("ovl lower write"), 1;
	}
	close(fd);

	snprintf(opts, sizeof(opts),
		 "lowerdir=/tmp/ovl/lower,upperdir=/tmp/ovl/upper,"
		 "workdir=/tmp/ovl/work,userxattr");
	ret = mount("overlay", "/tmp/ovl/merged", "overlay", 0, opts);
	if (ret < 0)
		return perror("ovl mount"), 1;

	fd = open("/tmp/ovl/merged/new", O_CREAT | O_RDWR, 0644);
	if (fd < 0)
		return perror("ovl create"), 1;
	if (write(fd, "live", 4) != 4) {
		close(fd);
		return perror("ovl write"), 1;
	}
	close(fd);
	fd = open("/tmp/ovl/merged/new", O_RDONLY);
	if (fd < 0)
		return perror("ovl reopen"), 1;
	if (read(fd, buf, sizeof(buf)) != 4 || memcmp(buf, "live", 4)) {
		close(fd);
		fprintf(stderr, "USERNS_TEST_FAIL overlay content\n");
		return 1;
	}
	close(fd);
	printf("USERNS_OVERLAY_OK\n");
	return 0;
}

int main(void)
{
	unsigned int uid = getuid();
	unsigned int gid = getgid();

	if (idmap_test())
		return 1;

	if (unshare(CLONE_NEWUSER) < 0) {
		perror("unshare user");
		return 1;
	}
	if (map_id("/proc/self/uid_map", "/proc/self/setgroups", uid) < 0) {
		perror("uid_map");
		return 1;
	}
	if (map_id("/proc/self/gid_map", NULL, gid) < 0) {
		perror("gid_map");
		return 1;
	}
	if (getuid() != 0 || getgid() != 0) {
		fprintf(stderr, "USERNS_TEST_FAIL unmapped uid=%u gid=%u\n",
			getuid(), getgid());
		return 1;
	}

	if (unshare(CLONE_NEWNS) < 0) {
		perror("unshare mount");
		return 1;
	}
	if (overlay_test())
		return 1;

	if (unshare(CLONE_NEWUSER) < 0) {
		perror("nested unshare user");
		return 1;
	}
	if (map_id("/proc/self/uid_map", "/proc/self/setgroups", 0) < 0) {
		perror("nested uid_map");
		return 1;
	}
	if (map_id("/proc/self/gid_map", NULL, 0) < 0) {
		perror("nested gid_map");
		return 1;
	}
	if (getuid() != 0 || getgid() != 0) {
		fprintf(stderr, "USERNS_TEST_FAIL nested uid=%u gid=%u\n",
			getuid(), getgid());
		return 1;
	}

	printf("USERNS_TEST_OK uid=%u gid=%u\n", getuid(), getgid());
	return 0;
}
