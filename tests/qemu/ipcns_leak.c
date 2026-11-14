/*
 * ipcns_leak - put one SysV shm segment or POSIX mqueue in the current IPC
 * namespace and exit without removing it.
 *
 * The namespace dies with its last process, so a correctly isolated IPC
 * namespace destroys the object.  If an emulation shares the init namespace
 * (or only fakes the syscall return), the object survives and the parent can
 * still see it.  That difference is invisible to a probe that only checks that
 * shmget()/mq_open() returned success.
 *
 *   ipcns_leak shm       create a shm segment in the caller's ipcns, leak it
 *   ipcns_leak mq        create /dnsk_probe_ipcns in the caller's ipcns, leak it
 *   ipcns_leak check-mq  try to create that name in the caller's ipcns:
 *                        0     = name is gone (isolated)
 *                        EEXIST= leaked from the child ipcns
 *
 * Every mode prints a KEY=value line; the caller runs the create modes inside
 * `unshare -i`.
 */
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#define MQ_NAME "/dnsk_probe_ipcns"

static int leak_shm(void)
{
	int id = shmget(IPC_PRIVATE, 4096, IPC_CREAT | 0600);

	if (id < 0) {
		printf("IPCNS_SHM_CREATE_RC=%d\n", errno);
		return 0;
	}
	/* Deliberately not IPC_RMID'd: the ipc namespace teardown owns it. */
	printf("IPCNS_SHM_CREATE_RC=0\n");
	return 0;
}

static int leak_mq(void)
{
	mqd_t mq = mq_open(MQ_NAME, O_CREAT | O_EXCL | O_RDWR, 0600, NULL);

	if (mq == (mqd_t)-1) {
		printf("IPCNS_MQ_CREATE_RC=%d\n", errno);
		return 0;
	}
	printf("IPCNS_MQ_CREATE_RC=0\n");
	mq_close(mq);
	/* Deliberately not mq_unlink'd: the ipc namespace teardown owns it. */
	return 0;
}

static int check_mq(void)
{
	mqd_t mq = mq_open(MQ_NAME, O_CREAT | O_EXCL | O_RDWR, 0600, NULL);

	if (mq == (mqd_t)-1) {
		printf("IPCNS_MQ_ISOLATED_RC=%d\n", errno);
		return 0;
	}
	printf("IPCNS_MQ_ISOLATED_RC=0\n");
	mq_close(mq);
	mq_unlink(MQ_NAME);
	return 0;
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";

	if (strcmp(mode, "shm") == 0)
		return leak_shm();
	if (strcmp(mode, "mq") == 0)
		return leak_mq();
	if (strcmp(mode, "check-mq") == 0)
		return check_mq();
	fprintf(stderr, "usage: %s shm|mq|check-mq\n", argv[0]);
	return 2;
}