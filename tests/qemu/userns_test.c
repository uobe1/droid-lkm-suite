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

/* Avoid a clash with <linux/mount.h> on newer cross toolchains. */
struct dnsk_mount_attr {
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

static int helper_map_write(pid_t pid, const char *name, const char *value)
{
	char path[64];

	snprintf(path, sizeof(path), "/proc/%d/%s", pid, name);
	return write_str(path, value);
}

/*
 * Fork a helper that enters a userns mapping 0 -> kuid 1.  The parent owns
 * the maps; with mount_tmpfs the helper also gets a private mount namespace
 * and mounts a tmpfs there, so the superblock's s_user_ns is the helper's
 * namespace.  The initial userns can later setns into it.
 */
static pid_t spawn_helper(int mount_tmpfs)
{
	int ready[2], go[2];
	char byte;
	pid_t pid;

	if (pipe(ready) < 0 || pipe(go) < 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		close(ready[0]);
		close(go[1]);
		/* Map kuid 1 to uid 0 inside; kuid 0 stays unmapped here. */
		if (setresgid(1, 1, 1) < 0 || setresuid(1, 1, 1) < 0)
			_exit(1);
		if (unshare(CLONE_NEWUSER) < 0)
			_exit(1);
		if (write(ready[1], "r", 1) != 1)
			_exit(1);
		if (read(go[0], &byte, 1) != 1)
			_exit(1);
		if (mount_tmpfs) {
			int fd;

			if (unshare(CLONE_NEWNS) < 0)
				_exit(2);
			mkdir("/tmp/idmapA", 0755);
			if (mount("tmpfs", "/tmp/idmapA", "tmpfs", 0,
				  "size=1M") < 0)
				_exit(3);
			fd = open("/tmp/idmapA/f", O_CREAT | O_WRONLY, 0644);
			if (fd < 0)
				_exit(4);
			close(fd);
			if (chown("/tmp/idmapA/f", 0, 0) < 0)
				_exit(5);
			if (write(ready[1], "d", 1) != 1)
				_exit(6);
		}
		for (;;)
			pause();
	}
	close(ready[1]);
	close(go[0]);
	if (read(ready[0], &byte, 1) != 1)
		goto fail;
	if (helper_map_write(pid, "setgroups", "deny") < 0 &&
	    errno != ENOENT)
		goto fail;
	if (helper_map_write(pid, "uid_map", "0 1 1\n") < 0)
		goto fail;
	if (helper_map_write(pid, "gid_map", "0 1 1\n") < 0)
		goto fail;
	if (write(go[1], "g", 1) != 1)
		goto fail;
	if (mount_tmpfs && read(ready[0], &byte, 1) != 1)
		goto fail;
	close(ready[0]);
	close(go[1]);
	return pid;

fail:
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	close(ready[0]);
	close(go[1]);
	return -1;
}

/*
 * The tmpfs file is owned by kuid 1 (helper A mounts it under userns A).
 * The mount is idmapped with a different userns B mapping 0 -> kuid 1, so
 * the file must appear as uid 0 to the initial userns.  B and A are both
 * children of init, so only init may use B for the idmap.
 */
static int idmap_test(void)
{
	char path[64];
	const char *step = "start";
	struct dnsk_mount_attr attr = { .attr_set = MOUNT_ATTR_IDMAP };
	struct stat st;
	int saved_mnt = -1, a_mnt = -1, ufd = -1;
	int ret, saved, in_a = 0;
	pid_t pa = -1, pb = -1;

	step = "save_ns";
	saved_mnt = open("/proc/self/ns/mnt", O_RDONLY);
	if (saved_mnt < 0)
		goto fail;
	step = "spawn_sb";
	pa = spawn_helper(1);
	if (pa < 0)
		goto fail;
	step = "spawn_idmap";
	pb = spawn_helper(0);
	if (pb < 0)
		goto fail;
	step = "open_sb_ns";
	snprintf(path, sizeof(path), "/proc/%d/ns/mnt", pa);
	a_mnt = open(path, O_RDONLY);
	if (a_mnt < 0)
		goto fail;
	step = "setns_sb";
	if (setns(a_mnt, CLONE_NEWNS) < 0)
		goto fail;
	in_a = 1;

	step = "pre_stat";
	if (stat("/tmp/idmapA/f", &st) < 0)
		goto fail;
	if (st.st_uid != 1) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap pre uid=%u\n",
			(unsigned)st.st_uid);
		goto fail;
	}
	step = "open_idmap_ns";
	snprintf(path, sizeof(path), "/proc/%d/ns/user", pb);
	ufd = open(path, O_RDONLY);
	if (ufd < 0)
		goto fail;
	attr.userns_fd = ufd;

	step = "mount_setattr";
	ret = syscall(SYS_mount_setattr, AT_FDCWD, "/tmp/idmapA", 0,
		      &attr, sizeof(attr));
	if (ret < 0)
		goto fail;
	step = "post_stat";
	if (stat("/tmp/idmapA/f", &st) < 0)
		goto fail;
	if (st.st_uid != 0) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap uid=%u\n",
			(unsigned)st.st_uid);
		goto fail;
	}
	step = "done";
	umount2("/tmp/idmapA", MNT_DETACH);
	if (setns(saved_mnt, CLONE_NEWNS) < 0)
		goto fail;
	in_a = 0;
	close(saved_mnt);
	close(a_mnt);
	close(ufd);
	kill(pa, SIGKILL);
	kill(pb, SIGKILL);
	waitpid(pa, NULL, 0);
	waitpid(pb, NULL, 0);
	printf("USERNS_IDMAP_OK\n");
	return 0;

fail:
	saved = errno;
	if (in_a) {
		umount2("/tmp/idmapA", MNT_DETACH);
		setns(saved_mnt, CLONE_NEWNS);
	}
	if (pa > 0) {
		kill(pa, SIGKILL);
		waitpid(pa, NULL, 0);
	}
	if (pb > 0) {
		kill(pb, SIGKILL);
		waitpid(pb, NULL, 0);
	}
	if (saved_mnt >= 0)
		close(saved_mnt);
	if (a_mnt >= 0)
		close(a_mnt);
	if (ufd >= 0)
		close(ufd);
	fprintf(stderr, "USERNS_TEST_FAIL idmap_%s: %s\n", step,
		strerror(saved));
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
