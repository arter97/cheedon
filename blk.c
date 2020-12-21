// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2020 Park Ju Hyung
 */

#define pr_fmt(fmt) "cheedon: " fmt

/*
 * XXX
 ** Concurrent I/O may require rwsem lock
 */

// #define DEBUG

#include <linux/module.h>
#include <linux/blkdev.h>
#include <linux/genhd.h>
#include <linux/backing-dev.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/blk-mq.h>

#include "cheedon.h"

// cheedon is intentionally designed to expose 1 disk only

/* Globals */
static int cheedon_major;
static struct gendisk *cheedon_disk;
static u64 cheedon_disksize;
static struct page *swap_header_page;
static struct blk_mq_tag_set tag_set;

struct class *cheedon_chr_class;

static int cheedon_open(struct block_device *dev, fmode_t mode)
{
	pr_info("%s\n", __func__);
	return 0;
}

static void cheedon_release(struct gendisk *gdisk, fmode_t mode)
{
	pr_info("%s\n", __func__);
}

static int cheedon_ioctl(struct block_device *bdev, fmode_t mode, unsigned cmd,
		   unsigned long arg)
{
	pr_info("ioctl cmd 0x%08x\n", cmd);

	return -ENOTTY;
}

/* Serve requests */
static int do_request(struct request *rq)
{
	int ret, id;
	struct cheedon_req *req;

	id = cheedon_push(rq);
	if (unlikely(id < 0)) {
		if (id == SKIP)
			return 0;
		return id;
	}

	req = reqs + id;

	wait_for_completion(&req->acked);

	ret = req->ret;
	cheedon_pop(id);

	return ret;
}

/* queue callback function */
static blk_status_t queue_rq(struct blk_mq_hw_ctx *hctx,
			     const struct blk_mq_queue_data *bd)
{
	int ret;
	struct request *rq = bd->rq;

	/* Start request serving procedure */
	blk_mq_start_request(rq);

	ret = do_request(rq);

	/* Stop request serving procedure */
	blk_mq_end_request(rq, ret < 0 ? BLK_STS_IOERR : BLK_STS_OK);

	return ret;
}

static const struct blk_mq_ops mq_ops = {
	.queue_rq = queue_rq,
};

static const struct block_device_operations cheedon_fops = {
	.owner = THIS_MODULE,
	.open = cheedon_open,
	.release = cheedon_release,
	.ioctl = cheedon_ioctl
};

static ssize_t disksize_show(struct device *dev,
			     struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "%llu\n", cheedon_disksize);
}

static ssize_t disksize_store(struct device *dev,
			      struct device_attribute *attr, const char *buf,
			      size_t len)
{
	int ret;
	u64 disksize;

	ret = kstrtoull(buf, 10, &disksize);
	if (ret)
		return ret;

	if (disksize == 0) {
		set_capacity(cheedon_disk, 0);
		return len;
	}

	cheedon_disksize = PAGE_ALIGN(disksize);
	if (!cheedon_disksize) {
		pr_err("disksize is invalid (disksize = %llu)\n", cheedon_disksize);

		cheedon_disksize = 0;

		return -EINVAL;
	}

	set_capacity(cheedon_disk, cheedon_disksize >> SECTOR_SHIFT);

	return len;
}

static DEVICE_ATTR(disksize, S_IRUGO | S_IWUSR, disksize_show, disksize_store);

static struct attribute *cheedon_disk_attrs[] = {
	&dev_attr_disksize.attr,
	NULL,
};

static const struct attribute_group cheedon_disk_attr_group = {
	.attrs = cheedon_disk_attrs,
};

static int create_device(void)
{
	int ret;

	/* gendisk structure */
	cheedon_disk = alloc_disk(1);
	if (!cheedon_disk) {
		pr_err("%s %d: Error allocating disk structure for device\n",
		       __func__, __LINE__);
		ret = -ENOMEM;
		goto out;
	}

	cheedon_disk->queue = blk_mq_init_sq_queue(&tag_set, &mq_ops, 128, BLK_MQ_F_SHOULD_MERGE);
	if (!cheedon_disk->queue) {
		pr_err("%s %d: Error allocating disk queue for device\n",
		       __func__, __LINE__);
		ret = -ENOMEM;
		goto out_put_disk;
	}

	// blk_queue_make_request(cheedon_disk->queue, cheedon_make_request);

	cheedon_disk->major = cheedon_major;
	cheedon_disk->first_minor = 0;
	cheedon_disk->fops = &cheedon_fops;
	cheedon_disk->private_data = NULL;
	snprintf(cheedon_disk->disk_name, 16, "cheedon%d", 0);

	/* Actual capacity set using sysfs (/sys/block/cheedon<id>/disksize) */
	set_capacity(cheedon_disk, 0);

	/*
	 * To ensure that we always get PAGE_SIZE aligned
	 * and n*PAGE_SIZED sized I/O requests.
	 */
	blk_queue_physical_block_size(cheedon_disk->queue, PAGE_SIZE);
	blk_queue_logical_block_size(cheedon_disk->queue,
				     CHEEDON_LOGICAL_BLOCK_SIZE);
	blk_queue_io_min(cheedon_disk->queue, PAGE_SIZE);
	blk_queue_max_hw_sectors(cheedon_disk->queue, 4096); // 512 * 4096 = 2MiB

	// Set discard capability
	cheedon_disk->queue->limits.discard_granularity = PAGE_SIZE;
	blk_queue_flag_set(QUEUE_FLAG_DISCARD, cheedon_disk->queue);
	blk_queue_max_discard_sectors(cheedon_disk->queue, 4096); // 512 * 4096 = 2MiB
	blk_queue_max_write_zeroes_sectors(cheedon_disk->queue, 4096); // 512 * 4096 = 2MiB

	add_disk(cheedon_disk);

	cheedon_disksize = 0;

	ret = sysfs_create_group(&disk_to_dev(cheedon_disk)->kobj,
				 &cheedon_disk_attr_group);
	if (ret < 0) {
		pr_err("%s %d: Error creating sysfs group\n",
		       __func__, __LINE__);
		goto out_free_queue;
	}

	/* cheedon devices sort of resembles non-rotational disks */
	blk_queue_flag_set(QUEUE_FLAG_NONROT, cheedon_disk->queue);
	blk_queue_flag_clear(QUEUE_FLAG_ADD_RANDOM, cheedon_disk->queue);

out:
	return ret;

out_free_queue:
	blk_cleanup_queue(cheedon_disk->queue);

out_put_disk:
	put_disk(cheedon_disk);

	return ret;
}

static void destroy_device(void)
{
	if (!cheedon_disk)
		return;

	pr_info("Removing device cheedon0\n");

	sysfs_remove_group(&disk_to_dev(cheedon_disk)->kobj,
			   &cheedon_disk_attr_group);

	if (cheedon_disk->queue)
		blk_cleanup_queue(cheedon_disk->queue);

	del_gendisk(cheedon_disk);
	put_disk(cheedon_disk);

	cheedon_disk = NULL;
}

static int __init cheedon_init(void)
{
	int ret, i;

	cheedon_major = register_blkdev(0, "cheedon");
	if (cheedon_major <= 0) {
		pr_err("%s %d: Unable to get major number\n",
		       __func__, __LINE__);
		ret = -EBUSY;
		goto out;
	}

	ret = create_device();
	if (ret) {
		pr_err("%s %d: Unable to create cheedon_device\n",
		       __func__, __LINE__);
		goto free_devices;
	}

	cheedon_chr_class = class_create(THIS_MODULE, "cheedon_chr");
	if (IS_ERR(cheedon_chr_class)) {
		ret = PTR_ERR(cheedon_chr_class);
		pr_warn("Failed to register class cheedon_chr\n");
		goto destroy_devices;
	}

	ret = cheedon_chr_init_module();
	if (ret)
		goto destroy_chr;

	reqs = kzalloc(sizeof(struct cheedon_req) * CHEEDON_QUEUE_SIZE, GFP_KERNEL);
	if (reqs == NULL) {
		pr_err("%s %d: Unable to allocate memory for cheedon_req\n", __func__, __LINE__);
		ret = -ENOMEM;
		goto nomem;
	}
	cheedon_queue_init();
	for (i = 0; i < CHEEDON_QUEUE_SIZE; i++)
		init_completion(&reqs[i].acked);

	return 0;

nomem:
	cheedon_chr_cleanup_module();
destroy_chr:
	class_destroy(cheedon_chr_class);
destroy_devices:
	destroy_device();
free_devices:
	unregister_blkdev(cheedon_major, "cheedon");
out:
	return ret;
}

static void __exit cheedon_exit(void)
{
	cheedon_queue_exit();

	kfree(reqs);

	cheedon_chr_cleanup_module();

	class_destroy(cheedon_chr_class);

	destroy_device();

	unregister_blkdev(cheedon_major, "cheedon");

	if (swap_header_page)
		__free_page(swap_header_page);
}

module_init(cheedon_init);
module_exit(cheedon_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Park Ju Hyung <qkrwngud825@gmail.com>");
