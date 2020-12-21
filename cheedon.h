// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Park Ju Hyung
 */

#ifndef __CHEEDON_H
#define __CHEEDON_H

#define SECTOR_SHIFT		9
#define SECTOR_SIZE		(1 << SECTOR_SHIFT)
#define SECTORS_PER_PAGE_SHIFT	(PAGE_SHIFT - SECTOR_SHIFT)
#define SECTORS_PER_PAGE	(1 << SECTORS_PER_PAGE_SHIFT)
#define CHEEDON_LOGICAL_BLOCK_SHIFT 12
#define CHEEDON_LOGICAL_BLOCK_SIZE	(1 << CHEEDON_LOGICAL_BLOCK_SHIFT)
#define CHEEDON_SECTOR_PER_LOGICAL_BLOCK	(1 << \
	(CHEEDON_LOGICAL_BLOCK_SHIFT - SECTOR_SHIFT))

#define CHEEDON_QUEUE_SIZE 4096

#define SKIP INT_MIN

// #define DEBUG
#define DEBUG_SLEEP 1

#ifdef DEBUG
  #define msleep_dbg msleep
#else
  #define msleep_dbg(...) ((void)0)
#endif

struct cheedon_req_user {
	// Aligned to 32B
	int id;
	int op;
	char *buf;
	unsigned int pos; // sector_t but divided by 4096
	unsigned int len;
	unsigned int pad[2];
};

#ifdef __KERNEL__

#include <linux/list.h>

struct cheedon_queue_item {
	int id;
	struct list_head tag_list;
};

struct cheedon_req {
	int ret;
	bool is_rw;
	struct request *rq;
	struct cheedon_req_user user;
	struct completion acked;
	struct cheedon_queue_item *item;
} __attribute__((aligned(8), packed));

// blk.c
void cheedon_io(struct cheedon_req_user *user); // Called by koo
extern struct class *cheedon_chr_class;
// extern struct mutex cheedon_mutex;
void cheedon_chr_cleanup_module(void);
int cheedon_chr_init_module(void);

// queue.c
extern struct cheedon_req *reqs;
int cheedon_push(struct request *rq);
struct cheedon_req *cheedon_peek(void);
void cheedon_pop(int id);
void cheedon_queue_init(void);
void cheedon_queue_exit(void);

#endif

#endif
