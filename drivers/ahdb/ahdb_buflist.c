// SPDX-License-Identifier: (MIT OR GPL-2.0)

/*
 * Copyright © 2019 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 * Authors:
 *    Dongwon Kim <dongwon.kim@intel.com>
 *    Mateusz Polrola <mateusz.polrola@gmail.com>
 *
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/cdev.h>

#include "ahdb_drv.h"

/* adding exported/imported HyperDMABUF info to hash */
int ahdb_addbuf(struct ahdb_buf *new)
{
	hash_add(g_ahdb_info->buf_list, &new->node, new->hid.id);
	return 0;
}

/* comparing two HyperDMABUF IDs */
static bool id_comp(ahdb_buf_id_t a, ahdb_buf_id_t b)
{
	int i;

	if (a.id != b.id)
		return 1;

	/* compare keys */
	for (i = 0; i < 3; i++) {
		if (a.rng_key[i] != b.rng_key[i])
			return 1;
	}

	return 0;
}

/* search for pre-exported sgt and return id of it if it exist */
ahdb_buf_id_t ahdb_find_hid(struct dma_buf *dmabuf)
{
	struct ahdb_buf *found;
	ahdb_buf_id_t hid = {-1, {0, 0, 0} };
	int bkt;

	hash_for_each(g_ahdb_info->buf_list, bkt, found, node)
		if (found->dma_buf == dmabuf)
			return found->hid;

	return hid;
}

/* find HyperDMABUF info for given HyperDMABUF ID */
struct ahdb_buf *ahdb_findbuf(ahdb_buf_id_t hid)
{
	struct ahdb_buf *found;
	int bkt;

	hash_for_each(g_ahdb_info->buf_list, bkt, found, node)
		if (!id_comp(found->hid, hid))
			return found;

	return NULL;
}

/* remove HyperDMABUF info */
int ahdb_delbuf(ahdb_buf_id_t hid)
{
	struct ahdb_buf *found;
	int bkt;

	hash_for_each(g_ahdb_info->buf_list, bkt, found, node)
		if (!id_comp(found->hid, hid)) {
			hash_del(&found->node);
			return 0;
		}

	return -ENOENT;
}

/* executing *func for every HyperDMABUF info */
void ahdb_foreachbuf(void (*func)(struct ahdb_buf *, void *),
		     void *param)
{
	struct ahdb_buf *found;
	struct hlist_node *tmp;
	int bkt;

	hash_for_each_safe(g_ahdb_info->buf_list, bkt, tmp, found, node)
		func(found, param);
}

static ssize_t shared_list_show(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	struct ahdb_buf *buf_info;
	int bkt;
	ssize_t count = 0;
	size_t total = 0;

	hash_for_each(g_ahdb_info->buf_list, bkt, buf_info, node) {
		ahdb_buf_id_t hid = buf_info->hid;

		total += buf_info->nents;
		count += scnprintf(buf + count, PAGE_SIZE - count,
			"SRC-VM:%d | id:%d | #pg:%d | valid:%d | imped?:%d\n",
			AHDB_VMID(hid), hid.id, buf_info->nents,
			buf_info->valid, buf_info->imported);
	}

	count += scnprintf(buf + count, PAGE_SIZE - count,
			   "total num of pages: %lu (%ld Bytes)\n",
			   total, total * PAGE_SIZE);

	return count;
}

static DEVICE_ATTR(shared_list, 0444, shared_list_show, NULL);

/* adding sysfs */
int ahdb_init_sysfs(struct device *dev)
{
	if (device_create_file(dev, &dev_attr_shared_list) < 0)
		return -1;

	return 0;
}

/* removing sysfs */
int ahdb_remove_sysfs(struct device *dev)
{
	device_remove_file(dev, &dev_attr_shared_list);
	return 0;
}
