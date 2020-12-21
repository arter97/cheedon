// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Park Ju Hyung
 */

#define pr_fmt(fmt) "cheedon_chr: " fmt

#include <linux/delay.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/cdev.h>
#include <linux/completion.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/backing-dev.h>
#include <linux/blk-mq.h>
#include <linux/sched/signal.h>

#include "cheedon.h"

#define CHEEDON_CHR_MAJOR 510
#define CHEEDON_CHR_MINOR 11

static struct cdev cheedon_chr_cdev;
static DECLARE_WAIT_QUEUE_HEAD(cheedon_chr_wait);

static int do_request(struct cheedon_req *req)
{
	unsigned long b_len = 0;
	struct bio_vec bvec;
	struct req_iterator iter;
	loff_t off = 0;
	void *b_buf;
	struct request *rq;

	rq = req->rq;

	pr_debug("%s++\n", __func__);

	/* Iterate over all requests segments */
	rq_for_each_segment(bvec, rq, iter) {
		b_len = bvec.bv_len;

		/* Get pointer to the data */
		b_buf = page_address(bvec.bv_page) + bvec.bv_offset;

		pr_debug("off: %lld, len: %ld, dest_buf: %px, user_buf: %px\n", off, b_len, b_buf, req->user.buf);

		switch (req->user.op) {
		case REQ_OP_WRITE:
			// Write
			if (unlikely(copy_to_user(req->user.buf + off, b_buf, 1 << CHEEDON_LOGICAL_BLOCK_SHIFT))) {
				WARN_ON(1);
				pr_err("%s: copy_to_user() failed\n", __func__);
				return -EFAULT;
			}
			break;
		case REQ_OP_READ:
			// Read
			if (unlikely(copy_from_user(b_buf, req->user.buf + off, 1 << CHEEDON_LOGICAL_BLOCK_SHIFT))) {
				WARN_ON(1);
				pr_err("%s: copy_from_user() failed\n", __func__);
				return -EFAULT;
			}
			break;
		}

		/* Increment counters */
		off += b_len;
	}

	pr_debug("%s--\n", __func__);

	return 0;
}

static int cheedon_chr_open(struct inode *inode, struct file *filp)
{
	return 0;
}

static int cheedon_chr_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t cheedon_chr_read(struct file *filp, char *buf, size_t count,
			    loff_t * f_pos)
{
	struct cheedon_req *req;

	if (unlikely(count != sizeof(struct cheedon_req_user))) {
		pr_err("%s: size mismatch: %ld vs %ld\n",
			__func__, count, sizeof(struct cheedon_req_user));
		WARN_ON(1);
		return -EINVAL;
	}

	req = cheedon_peek();
	if (unlikely(req == NULL)) {
		pr_err("%s: failed to peek queue\n", __func__);
		return -ERESTARTSYS;
	}

	if (unlikely(copy_to_user(buf, &req->user, count))) {
		pr_err("%s: copy_to_user() failed\n", __func__);
		return -EFAULT;
	}

	return count;
}

static ssize_t cheedon_chr_write(struct file *file, const char __user *buf,
			    size_t count, loff_t *ppos)
{
	struct cheedon_req *req;
	struct cheedon_req_user ureq;

	if (unlikely(count != sizeof(struct cheedon_req_user))) {
		pr_err("%s: size mismatch: %ld vs %ld\n",
			__func__, count, sizeof(struct cheedon_req_user));
		return -EINVAL;
	}

	if (unlikely(copy_from_user(&ureq, buf, sizeof(ureq)))) {
		pr_err("%s: failed to fill req\n", __func__);
		return -EFAULT;
	}

	pr_debug("write: req[%d]\n"
		"  buf=%px\n"
		"  pos=%u\n"
		"  len=%u\n",
			ureq.id, ureq.buf, ureq.pos, ureq.len);

	req = reqs + ureq.id;
	req->user.buf = ureq.buf;

	// Process bio
	if (likely(req->is_rw))
		req->ret = do_request(req);
	else
		req->ret = 0;

	complete(&req->acked);

	return (ssize_t)count;
}

static const struct file_operations cheedon_chr_fops = {
	.read = cheedon_chr_read,
	.write = cheedon_chr_write,
	.open = cheedon_chr_open,
	.release = cheedon_chr_release,
};

void cheedon_chr_cleanup_module(void)
{
	unregister_chrdev_region(MKDEV(CHEEDON_CHR_MAJOR, CHEEDON_CHR_MINOR), 1);
	cdev_del(&cheedon_chr_cdev);
	device_destroy(cheedon_chr_class, MKDEV(CHEEDON_CHR_MAJOR, CHEEDON_CHR_MINOR));
}

int cheedon_chr_init_module(void)
{
	struct device *cheedon_chr_device;
	int result;


	/*
	 * Register your major, and accept a dynamic number. This is the
	 * first thing to do, in order to avoid releasing other module's
	 * fops in cheedon_chr_cleanup_module()
	 */

	cdev_init(&cheedon_chr_cdev, &cheedon_chr_fops);
	cheedon_chr_cdev.owner = THIS_MODULE;
	result = cdev_add(&cheedon_chr_cdev, MKDEV(CHEEDON_CHR_MAJOR, CHEEDON_CHR_MINOR), 1);
	if (result) {
		pr_warn("Failed to add cdev for /dev/cheedon_chr\n");
		goto error1;
	}

	result =
	    register_chrdev_region(MKDEV(CHEEDON_CHR_MAJOR, CHEEDON_CHR_MINOR), 1,
				   "/dev/cheedon_chr");
	if (result < 0) {
		pr_warn("can't get major/minor %d/%d\n", CHEEDON_CHR_MAJOR,
			CHEEDON_CHR_MINOR);
		goto error2;
	}

	cheedon_chr_device =
	    device_create(cheedon_chr_class, NULL,
			  MKDEV(CHEEDON_CHR_MAJOR, CHEEDON_CHR_MINOR), NULL, "cheedon_chr");

	if (IS_ERR(cheedon_chr_device)) {
		pr_warn("Failed to create cheedon_chr device\n");
		goto error3;
	}

	return 0;		/* succeed */

error3:
	unregister_chrdev_region(MKDEV(CHEEDON_CHR_MAJOR, CHEEDON_CHR_MINOR), 1);
error2:
	cdev_del(&cheedon_chr_cdev);
error1:

	return result;
}
