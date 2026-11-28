// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

struct job {
	unsigned int id;
	char mode[8];
};

static const char *alloc_path;

static int touch_anon(void)
{
	long page_size = sysconf(_SC_PAGESIZE);
	size_t bytes = 32 * page_size, offset;
	volatile unsigned char *addr;
	int err = 0;

	addr = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (addr == MAP_FAILED)
		return errno;
	for (offset = 0; offset < bytes; offset += page_size) {
		addr[offset] = 0x5a;
		if (addr[offset + 1])
			err = EUCLEAN;
	}
	munmap((void *)addr, bytes);
	return err;
}

static void *allocate(void *arg)
{
	struct job *job = arg;
	char buf[40];
	int fd, len, err;

	if (!strcmp(job->mode, "anon")) {
		err = touch_anon();
	} else {
		fd = open(alloc_path, O_WRONLY);
		if (fd < 0) {
			err = errno;
		} else {
			len = snprintf(buf, sizeof(buf), "%s %u 0", job->mode, job->id);
			err = write(fd, buf, len) == len ? 0 : errno;
			close(fd);
		}
	}

	printf("%u=%d\n", job->id, err);
	free(job);
	return NULL;
}

int main(int argc, char **argv)
{
	struct job job;

	if (argc != 2)
		return 1;
	alloc_path = argv[1];
	setvbuf(stdout, NULL, _IOLBF, 0);
	while (scanf("%7s %u", job.mode, &job.id) == 2) {
		struct job *copy = malloc(sizeof(*copy));
		pthread_t thread;
		int err;

		if (!copy)
			return 1;
		*copy = job;
		err = pthread_create(&thread, NULL, allocate, copy);
		if (err) {
			free(copy);
			return 1;
		}
		pthread_detach(thread);
	}
	pthread_exit(NULL);
}
