// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Park Ju Hyung
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/semaphore.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/backing-dev.h>
#include <linux/blk-mq.h>
#include <linux/spinlock.h>

#include "cheedon.h"

//static int front, rear;
//static struct semaphore mutex, slots, items;
static struct semaphore slots, items;
static struct list_head free_tag_list, processing_tag_list; 
static spinlock_t queue_spin;


// Protect with lock
struct cheedon_req *reqs = NULL;

// Lock must be held and freed before and after push()
int cheedon_push(struct request *rq) {
	struct cheedon_req *req;
	int id, op;
	bool is_rw = true;
	unsigned long irqflags;
	struct cheedon_queue_item *item; 

	op = req_op(rq);
	if (unlikely(op > 1)) {
		is_rw = false;
		switch (op = req_op(rq)) {
		case REQ_OP_FLUSH:
			pr_warn("ignoring REQ_OP_FLUSH\n");
			return SKIP;
		case REQ_OP_WRITE_ZEROES:
			// pr_warn("ignoring REQ_OP_WRITE_ZEROES\n");
			// return SKIP;
			pr_debug("REQ_OP_WRITE_ZEROES -> REQ_OP_DISCARD\n");
			op = REQ_OP_DISCARD;
			/* fallthrough */
		case REQ_OP_DISCARD:
			pr_debug("REQ_OP_DISCARD\n");
			break;
		default:
			pr_warn("unsupported operation: %d\n", op);
			return -EOPNOTSUPP;
		}
	}

	while(down_interruptible(&slots) == -EINTR) {
		//pr_info("interrupt - 1\n");
	}
	spin_lock_irqsave(&queue_spin, irqflags);
	//while(down_interruptible(&mutex) == -EINTR) {
	//	pr_info("interrupt - 2\n");
	//}
	//down(&mutex);

	item = list_first_entry(&free_tag_list, struct cheedon_queue_item, tag_list);
	list_move_tail(&item->tag_list, &processing_tag_list);
	id = item->id;

	//id = (rear + 1) % CHEEDON_QUEUE_SIZE; // XXX: Overflow?

	//pr_info("pushing %d(buf_len: %d)\n", id, user->buf_len);

	//rear = id;
	req = reqs + id;		/* Insert the item */

	req->rq = rq;
	req->is_rw = is_rw;

	req->user.op = op;
	req->user.pos = (blk_rq_pos(rq) << SECTOR_SHIFT) >> CHEEDON_LOGICAL_BLOCK_SHIFT;
	req->user.len = blk_rq_bytes(rq);
	req->user.id = id;
	reinit_completion(&req->acked);
	req->item = item;

	spin_unlock_irqrestore(&queue_spin, irqflags);

	//up(&mutex);	/* Unlock the buffer */
	up(&items);	/* Announce available item */

	return id;
}

// Queue is locked until pop
struct cheedon_req *cheedon_peek(void) {
	int id, ret;
	struct cheedon_queue_item *item;
	unsigned long irqflags;

	ret = down_interruptible(&items);	/* Wait for available item */
	if (unlikely(ret < 0))
		return NULL;

	/* Lock the buffer */
	//down(&mutex);
	spin_lock_irqsave(&queue_spin, irqflags);
	//while(down_interruptible(&mutex) == -EINTR) {
	//	pr_info("interrupt - 3\n");
	//}
	item = list_first_entry(&processing_tag_list, struct cheedon_queue_item, tag_list);
	list_del(&item->tag_list);
	id = item->id;

	//id = (front + 1) % CHEEDON_QUEUE_SIZE;	/* Remove the item */
	spin_unlock_irqrestore(&queue_spin, irqflags);

	return reqs + id;
}

void cheedon_pop(int id) {
	unsigned long irqflags;
	struct cheedon_queue_item *item;
	struct cheedon_req *req;

	spin_lock_irqsave(&queue_spin, irqflags);

	req = reqs + id;
	
	item = req->item;
	//list_move_tail(&item->tag_list, &free_tag_list);
	list_add_tail(&item->tag_list, &free_tag_list);

	spin_unlock_irqrestore(&queue_spin, irqflags);

	//up(&mutex);	/* Unlock the buffer */
	up(&slots);	/* Announce available slot */
}



void cheedon_queue_init(void) {
	int i;
	struct cheedon_queue_item *item;
	//front = rear = 0;	/* Empty buffer iff front == rear */
	//sema_init(&mutex, 1);	/* Binary semaphore for locking */
	INIT_LIST_HEAD(&free_tag_list);
	INIT_LIST_HEAD(&processing_tag_list);
	spin_lock_init(&queue_spin);

	for (i = 0; i < CHEEDON_QUEUE_SIZE; i++) {
		item = kzalloc(sizeof(struct cheedon_queue_item), GFP_KERNEL);
		item->id = i;
		INIT_LIST_HEAD(&item->tag_list);
		list_add_tail(&item->tag_list, &free_tag_list);
	}

	sema_init(&slots, CHEEDON_QUEUE_SIZE);	/* Initially, buf has n empty slots */
	sema_init(&items, 0);	/* Initially, buf has zero data items */
}


void cheedon_queue_exit(void) {
	int i;
	struct cheedon_req *req;
	for (i = 0; i < CHEEDON_QUEUE_SIZE; i++) {
		req = reqs + i;
		kfree(req->item);
	}
}
