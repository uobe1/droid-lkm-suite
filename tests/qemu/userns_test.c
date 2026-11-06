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
#ifndef SYS_open_tree
#define SYS_open_tree 428
#endif
#ifndef SYS_move_mount
#define SYS_move_mount 429
#endif
#ifndef MOUNT_ATTR_IDMAP
#define MOUNT_ATTR_IDMAP 0x00100000
#endif
#ifndef OPEN_TREE_CLONE
#define OPEN_TREE_CLONE 1
#endif
#ifndef OPEN_TREE_CLOEXEC
#define OPEN_TREE_CLOEXEC 0x80000
#endif
#ifndef AT_EMPTY_PATH
#define AT_EMPTY_PATH 0x1000
#endif
#ifndef MOVE_MOUNT_F_EMPTY_PATH
#define MOVE_MOUNT_F_EMPTY_PATH 0x00000004
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
 * Fork a child that enters a userns mapping 0 -> outer_id.  The caller is
 * already in the parent userns and owns the maps; the child stays alive to
 * keep the namespace available as a mount idmap.
 */
static pid_t spawn_userns_helper(unsigned int outer_id)
{
	int ready[2], go[2];
	char map[64], byte;
	pid_t pid;

	if (pipe(ready) < 0 || pipe(go) < 0)
		return -1;
	pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		close(ready[0]);
		close(go[1]);
		if (unshare(CLONE_NEWUSER) < 0)
			_exit(1);
		if (write(ready[1], "r", 1) != 1)
			_exit(2);
		if (read(go[0], &byte, 1) != 1)
			_exit(3);
		for (;;)
			pause();
	}
	close(ready[1]);
	close(go[0]);
	if (read(ready[0], &byte, 1) != 1) {
		fprintf(stderr, "idmap helper died before ready, errno=%d\n",
			errno);
		goto fail;
	}
	if (helper_map_write(pid, "setgroups", "deny") < 0 &&
	    errno != ENOENT)
		goto fail;
	snprintf(map, sizeof(map), "0 %u 1\n", outer_id);
	if (helper_map_write(pid, "uid_map", map) < 0)
		goto fail;
	if (helper_map_write(pid, "gid_map", map) < 0)
		goto fail;
	if (write(go[1], "g", 1) != 1)
		goto fail;
	close(ready[0]);
	close(go[1]);
	return pid;

fail:
	fprintf(stderr, "idmap helper setup failed, errno=%d\n", errno);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	close(ready[0]);
	close(go[1]);
	return -1;
}

/*
 * Runs inside userns A (mapped 0 -> kuid 0 and 1 -> kuid 1) with a private
 * mount namespace.  The tmpfs superblock is owned by A; the file f is owned
 * by kuid 0.  A child userns B maps 0 -> kuid 1 and is used as the mount
 * idmap, so f must change from uid 0 to uid 1 in A.
 */
static int child_idmap_test(void)
{
	char path[64];
	const char *step = "start";
	struct dnsk_mount_attr attr = { .attr_set = MOUNT_ATTR_IDMAP };
	struct stat st;
	int fd, tfd = -1, ufd = -1, ret, saved;
	pid_t pb = -1;

	step = "mkdir";
	if (mkdir("/tmp/idmap", 0755) < 0 && errno != EEXIST)
		goto fail;
	step = "mount";
	if (mount("tmpfs", "/tmp/idmap", "tmpfs", 0, "size=1M") < 0)
		goto fail_umount;
	step = "create";
	fd = open("/tmp/idmap/f", O_CREAT | O_WRONLY, 0644);
	if (fd < 0)
		goto fail_umount;
	close(fd);
	step = "pre_stat";
	if (stat("/tmp/idmap/f", &st) < 0)
		goto fail_umount;
	if (st.st_uid != 0) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap pre uid=%u\n",
			(unsigned)st.st_uid);
		goto fail_umount;
	}
	step = "spawn_idmap";
	pb = spawn_userns_helper(1);
	if (pb < 0)
		goto fail_umount;
	step = "open_idmap_ns";
	snprintf(path, sizeof(path), "/proc/%d/ns/user", pb);
	ufd = open(path, O_RDONLY);
	if (ufd < 0)
		goto fail_umount;
	attr.userns_fd = ufd;

	/*
	 * mount_setattr only accepts idmaps for mounts in an anonymous mount
	 * namespace, so clone the tmpfs with open_tree() and attach the
	 * idmapped result with move_mount().
	 */
	step = "open_tree";
	tfd = syscall(SYS_open_tree, AT_FDCWD, "/tmp/idmap",
		      OPEN_TREE_CLONE | OPEN_TREE_CLOEXEC);
	if (tfd < 0)
		goto fail_umount;
	step = "mount_setattr";
	ret = syscall(SYS_mount_setattr, tfd, "", AT_EMPTY_PATH,
		      &attr, sizeof(attr));
	if (ret < 0)
		goto fail_tree;
	step = "clone_stat";
	if (fstatat(tfd, "f", &st, 0) == 0)
		printf("IDMAP_CLONE_UID=%u\n", (unsigned)st.st_uid);
	else
		printf("IDMAP_CLONE_STAT_ERR=%d\n", errno);
	step = "mkdir_target";
	if (mkdir("/tmp/idmap2", 0755) < 0 && errno != EEXIST)
		goto fail_tree;
	step = "move_mount";
	ret = syscall(SYS_move_mount, tfd, "", AT_FDCWD, "/tmp/idmap2",
		      MOVE_MOUNT_F_EMPTY_PATH);
	if (ret < 0)
		goto fail_tree;
	close(tfd);
	tfd = -1;
	step = "post_stat";
	if (stat("/tmp/idmap2/f", &st) < 0)
		goto fail_umount;
	if (st.st_uid != 1) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap uid=%u\n",
			(unsigned)st.st_uid);
		goto fail_umount;
	}
	step = "done";
	umount2("/tmp/idmap2", MNT_DETACH);
	umount2("/tmp/idmap", MNT_DETACH);
	if (pb > 0) {
		kill(pb, SIGKILL);
		waitpid(pb, NULL, 0);
	}
	if (ufd >= 0)
		close(ufd);
	printf("USERNS_IDMAP_OK\n");
	return 0;

fail_tree:
	close(tfd);
	tfd = -1;
fail_umount:
	saved = errno;
	umount2("/tmp/idmap2", MNT_DETACH);
	umount2("/tmp/idmap", MNT_DETACH);
	errno = saved;
fail:
	saved = errno;
	if (pb > 0) {
		kill(pb, SIGKILL);
		waitpid(pb, NULL, 0);
	}
	if (ufd >= 0)
		close(ufd);
	fprintf(stderr, "USERNS_TEST_FAIL idmap_%s: %s\n", step,
		strerror(saved));
	return 1;
}

static int idmap_test(void)
{
	int ready[2], go[2], status;
	char byte;
	pid_t pid;

	if (pipe(ready) < 0 || pipe(go) < 0)
		return perror("idmap pipe"), 1;
	pid = fork();
	if (pid < 0)
		return perror("idmap fork"), 1;
	if (pid == 0) {
		close(ready[0]);
		close(go[1]);
		if (unshare(CLONE_NEWUSER) < 0) {
			perror("idmap child unshare");
			_exit(1);
		}
		if (write(ready[1], "r", 1) != 1)
			_exit(2);
		if (read(go[0], &byte, 1) != 1)
			_exit(3);
		if (unshare(CLONE_NEWNS) < 0) {
			perror("idmap child unshare mount");
			_exit(4);
		}
		_exit(child_idmap_test());
	}
	close(ready[1]);
	close(go[0]);
	if (read(ready[0], &byte, 1) != 1) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap_spawn_sb_ready\n");
		goto fail;
	}
	if (helper_map_write(pid, "setgroups", "deny") < 0 &&
	    errno != ENOENT)
		goto fail_map;
	if (helper_map_write(pid, "uid_map", "0 0 1\n1 1 1\n") < 0)
		goto fail_map;
	if (helper_map_write(pid, "gid_map", "0 0 1\n1 1 1\n") < 0)
		goto fail_map;
	if (write(go[1], "g", 1) != 1)
		goto fail_map;
	close(ready[0]);
	close(go[1]);
	waitpid(pid, &status, 0);
	if (!WIFEXITED(status) || WEXITSTATUS(status)) {
		fprintf(stderr, "USERNS_TEST_FAIL idmap_child_exit=%d\n",
			WIFEXITED(status) ? WEXITSTATUS(status) : -WTERMSIG(status));
		return 1;
	}
	return 0;

fail_map:
	fprintf(stderr, "USERNS_TEST_FAIL idmap_sb_map: %s\n",
		strerror(errno));
fail:
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	close(ready[0]);
	close(go[1]);
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
