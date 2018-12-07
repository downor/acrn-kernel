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

#include <linux/list.h>
#include <linux/slab.h>
#include <linux/random.h>

#include "ahdb_drv.h"

/* trying to retrieve reusable HyperDMABUF ID for new HyperDMABUF */
static int try_reusable(void)
{
	struct list_used_id *head = g_ahdb_info->used_ids;
	int id = -1;

	/* check there is reusable id */
	if (!list_empty(&head->list)) {
		head = list_first_entry(&head->list, struct list_used_id,
					list);
		list_del(&head->list);
		id = head->id;
		kfree(head);
	}

	return id;
}

/* adding HyperDMABUF ID to the hash for reusable IDs */
void add_used_id(int id)
{
	struct list_used_id *head = g_ahdb_info->used_ids;
	struct list_used_id *new;

	new = kmalloc(sizeof(*new), GFP_ATOMIC);

	if (!new)
		return;

	new->id = id;

	list_add(&new->list, &head->list);
}

/* remove all buf ids in the hash for reusable IDs */
void free_used_ids_all(void)
{
	struct list_used_id *head = g_ahdb_info->used_ids;
	struct list_used_id *temp;

	if (head) {
		/* freeing mem space all reusable ids in the stack */
		while (!list_empty(&head->list)) {
			temp = list_first_entry(&head->list,
						struct list_used_id,
						list);
			list_del(&temp->list);
			kfree(temp);
		}

		/* freeing head */
		kfree(head);
	}
}

/* getting a new or reusable HyperDMABUF ID */
ahdb_buf_id_t get_hid(void)
{
	static int count;
	ahdb_buf_id_t hid = {-1, {0, 0, 0} };
	struct list_used_id *head;

	/* first get_id */
	if (count == 0) {
		head = kmalloc(sizeof(*head), GFP_KERNEL);

		if (!head)
			return hid;

		/* list head should have an invalid count */
		head->id = -1;
		INIT_LIST_HEAD(&head->list);
		g_ahdb_info->used_ids = head;
	}

	hid.id = try_reusable();

	/* creating a new H-ID only if nothing in the reusable id queue
	 * and count is less than maximum allowed
	 */
	if (hid.id == -1) {
		if (count < AHDB_MAX_ID) {
			hid.id = AHDB_BUF_ID_CREATE(g_ahdb_info->vmid,
						    count++);
		} else {
			dev_err(g_ahdb_info->dev,
				"no more buf id available\n");
			return hid;
		}
	}

	/* random data embedded in the id for security */
	get_random_bytes(&hid.rng_key[0], 12);

	return hid;
}
