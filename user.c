// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Juhyung Park, Jooyoung Song
 */

#define _GNU_SOURCE

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/uio.h>

#define unlikely(x)     __builtin_expect(!!(x), 0)

enum req_opf {
	/* read sectors from the device */
	REQ_OP_READ = 0,
	/* write sectors to the device */
	REQ_OP_WRITE = 1,
	/* flush the volatile write cache */
	REQ_OP_FLUSH = 2,
	/* discard sectors */
	REQ_OP_DISCARD = 3,
	/* get zone information */
	REQ_OP_ZONE_REPORT = 4,
	/* securely erase sectors */
	REQ_OP_SECURE_ERASE = 5,
	/* seset a zone write pointer */
	REQ_OP_ZONE_RESET = 6,
	/* write the same sector many times */
	REQ_OP_WRITE_SAME = 7,
	/* write the zero filled sector many times */
	REQ_OP_WRITE_ZEROES = 9,

	/* SCSI passthrough using struct scsi_request */
	REQ_OP_SCSI_IN = 32,
	REQ_OP_SCSI_OUT = 33,
	/* Driver private requests */
	REQ_OP_DRV_IN = 34,
	REQ_OP_DRV_OUT = 35,

	REQ_OP_LAST,
};

struct cheedon_req_user {
	// Aligned to 32B
	int id;
	int op;
	char *buf;
	unsigned int pos;	// sector_t
	unsigned int len;
	unsigned int pad[2];
};

// #define STRIPE_K 128
#define STRIPE_SIZE (STRIPE_K * 1024)

// Warning, output is static so this function is not reentrant
static const char *humanSize(uint64_t bytes)
{
	static char output[200];

	char *suffix[] = { "B", "KiB", "MiB", "GiB", "TiB" };
	char length = sizeof(suffix) / sizeof(suffix[0]);

	int i = 0;
	double dblBytes = bytes;
	if (bytes > 1024) {
		for (i = 0; (bytes / 1024) > 0 && i < length - 1;
		     i++, bytes /= 1024)
			dblBytes = bytes / 1024.0;
	}

	sprintf(output, "%.02lf %s", dblBytes, suffix[i]);

	return output;
}

static off_t fdlength(int fd)
{
	struct stat st;
	off_t cur, ret;

	if (!fstat(fd, &st) && S_ISREG(st.st_mode))
		return st.st_size;

	cur = lseek(fd, 0, SEEK_CUR);
	ret = lseek(fd, 0, SEEK_END);
	lseek(fd, cur, SEEK_SET);

	return ret;
}

static inline uint64_t ts_to_ns(struct timespec *ts)
{
	return ts->tv_sec * (uint64_t) 1000000000L + ts->tv_nsec;
}

#define POS (req.pos * 4096UL)

static void *ptr_align(void const *ptr, size_t alignment)
{
	char const *p0 = ptr;
	char const *p1 = p0 + alignment - 1;
	return (void *)(p1 - (size_t)p1 % alignment);
}

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif
// #define NUM_DEVICE 2

static char __tmpbuf[2 * 1024 * 1024 + PAGE_SIZE];
static char *tmpbuf = __tmpbuf;
int main()
{
	int ret;
	int chrfd, copyfd[NUM_DEVICE];
	ssize_t r;
	struct cheedon_req_user req;
	unsigned int i, j, loop;

	off_t stripe_in, pos[NUM_DEVICE], moving_pos[NUM_DEVICE];
	const char *dev_name[4];

	chrfd = open("/dev/cheedon_chr", O_RDWR);
	if (chrfd < 0) {
		perror("Failed to open /dev/cheedon_chr");
		return 1;
	}

	/* Device name */
/*
	dev_name[0] = "/dev/disk/by-id/nvme-SAMSUNG_MZVLB512HBJQ-00007_S4GFNA0NB07880";
	dev_name[1] = "/dev/disk/by-id/nvme-SAMSUNG_MZVLB512HBJQ-00007_S4GFNA0NB07884";
	dev_name[2] = "/dev/disk/by-id/nvme-SAMSUNG_MZVLB512HBJQ-00007_S4GFNA0NB07890";
	dev_name[3] = "/dev/disk/by-id/nvme-SAMSUNG_MZVLB512HBJQ-00007_S4GFNA0NB07893";
*/
	dev_name[0] = "/dev/disk/by-id/usb-USB_SanDisk_3.2Gen1_01016c8835988c82d7b49463d974a5d8cd2c1908791fbaa1d281b5a9aa70bf325ac300000000000000000000b830ba7200090500a355810798a834e7-0:0-part1";
	dev_name[1] = "/dev/disk/by-id/usb-USB_SanDisk_3.2Gen1_0101b7011c1ff67ff116502406f9fd7f585c8f8436244c3e73578a401638d20aa2d60000000000000000000089a9b4cdff900600a355810798a82d35-0:0-part1";
	dev_name[2] = "/dev/disk/by-id/usb-USB_SanDisk_3.2Gen1_0101c4c6d6887a60550d965e91a4061563e4291adde386efd6298b2d72cc1eca755f000000000000000000009278dd88ff020700a355810798a82d34-0:0-part1";
	dev_name[3] = "/dev/disk/by-id/usb-USB_SanDisk_3.2Gen1_0101e16bc75110b5def39e69e655a6b5096b6a3b5db7578ac2f925a6e02f0de86344000000000000000000003bcc9552000c0700a355810798a82d26-0:0-part1";

	for (i = 0; i < NUM_DEVICE; i++) {
#ifdef DIRECT
		copyfd[i] = open(dev_name[i], O_RDWR | O_DIRECT);
#else
		copyfd[i] = open(dev_name[i], O_RDWR);
#endif
		if (copyfd[i] < 0) {
			perror("Failed to open file");
			exit(1);
		}
	}

	tmpbuf = ptr_align(tmpbuf, PAGE_SIZE);

	while (1) {
		r = read(chrfd, &req, sizeof(struct cheedon_req_user));
		if (r < 0)
			break;

		if (req.op != REQ_OP_READ && req.op != REQ_OP_WRITE) {
			write(chrfd, &req, sizeof(struct cheedon_req_user));
			continue;
		}

/*
		printf("req[%d]\n"
			"  pos=%d\n"
			"  len=%d\n",
				req.id, req.pos, req.len);
*/

		stripe_in = POS / STRIPE_SIZE;

		for (i = 0; i < NUM_DEVICE; i++) {
			pos[i] = ((stripe_in / NUM_DEVICE + ((stripe_in % NUM_DEVICE) > i ? 1 : 0)) * STRIPE_SIZE)
				+ (POS - stripe_in * STRIPE_SIZE) * ((stripe_in % NUM_DEVICE) == i ? 1 : 0);
			moving_pos[i] = pos[i];
		}

		req.buf = tmpbuf;

		if (req.op == REQ_OP_WRITE)
			write(chrfd, &req, sizeof(struct cheedon_req_user));

		loop = req.len / 4096;
		for (i = 0; i < loop; i++) {
			j = ((POS + i * 4096) / STRIPE_SIZE) % NUM_DEVICE;

			if (req.op == REQ_OP_READ)
				pread(copyfd[j],
				      tmpbuf + (i * 4096), 4096,
				      moving_pos[j]);
			else
				pwrite(copyfd[j],
				       tmpbuf + (i * 4096), 4096,
				       moving_pos[j]);
			moving_pos[j] += 4096;
		}

		if (req.op == REQ_OP_READ) {
			write(chrfd, &req, sizeof(struct cheedon_req_user));
/*		} else {
			for (i = 0; i < NUM_DEVICE; i++) {
				printf("fsync(%d)\n", i);
				fsync(copyfd[i]);
			}
*/		}
	}

	return 0;
}
