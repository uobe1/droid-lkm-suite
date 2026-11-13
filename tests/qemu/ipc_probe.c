/*
 * ipc_probe - report the error codes that SysV IPC and POSIX mqueue return for
 * well-defined invalid inputs.
 *
 * An emulation that makes the *success* paths work but reports different errnos
 * on the failure paths is not equivalent: callers branch on those codes.  So the
 * probe prints them instead of just saying "works".
 *
 * Output is key=value, one per line, so two kernels can be diffed directly.
 */
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <stdio.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/sem.h>
#include <sys/shm.h>
#include <unistd.h>

static void report(const char *what, int ret, int err)
{
	if (ret >= 0)
		printf("%s=ok\n", what);
	else
		/*
		 * Numeric errno, not a name: strerrorname_np() is a glibc extension and
		 * the differential only needs a comparable value.  The number is the
		 * contract callers branch on anyway.
		 */
		printf("%s=err:%d\n", what, err);
}

int main(void)
{
	int id, ret, err;

	/* A private IPC key that no other process uses. */
	key_t key = (key_t)(0x40000000 | (getpid() & 0xffff));

	/* shmget with an absurd size must fail with EINVAL. */
	errno = 0;
	ret = shmget(key, (size_t)1 << 40, IPC_CREAT | 0600);
	err = errno;
	report("SHMGET_HUGE", ret, err);
	if (ret >= 0)
		shmctl(ret, IPC_RMID, NULL);

	/* shmget with no flags on a missing key: ENOENT. */
	errno = 0;
	ret = shmget(key + 1, 4096, 0600);
	err = errno;
	report("SHMGET_MISSING", ret, err);

	/* semget with an invalid count: EINVAL. */
	errno = 0;
	ret = semget(key, 0, IPC_CREAT | 0600);
	err = errno;
	report("SEMGET_ZERO", ret, err);
	if (ret >= 0)
		semctl(ret, 0, IPC_RMID);

	/* msgget, then a send that exceeds the queue size: EINVAL. */
	errno = 0;
	id = msgget(key, IPC_CREAT | 0600);
	if (id < 0) {
		report("MSGGET", id, errno);
	} else {
		report("MSGGET", id, 0);
		struct {
			long mtype;
			char buf[8192];
		} msg;
		msg.mtype = 1;

		errno = 0;
		ret = msgsnd(id, &msg, sizeof(msg.buf), IPC_NOWAIT);
		err = errno;
		report("MSGSND_TOO_BIG", ret, err);
		msgctl(id, IPC_RMID, NULL);
	}

	/* POSIX mqueue: an invalid name must be EINVAL, a duplicate open EEXIST. */
	errno = 0;
	mqd_t mq = mq_open("/dnsk_probe", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
	err = errno;
	report("MQ_OPEN", (int)mq, err);
	if (mq != (mqd_t)-1) {
		mq_close(mq);
		errno = 0;
		mq = mq_open("/dnsk_probe", O_CREAT | O_EXCL | O_RDWR, 0600, NULL);
		err = errno;
		report("MQ_OPEN_DUP", (int)mq, err);
		mq_unlink("/dnsk_probe");
	}

	errno = 0;
	mq = mq_open("bad/name", O_CREAT | O_RDWR, 0600, NULL);
	err = errno;
	report("MQ_BAD_NAME", (int)mq, err);

	printf("IPC_PROBE_DONE\n");
	return 0;
}
