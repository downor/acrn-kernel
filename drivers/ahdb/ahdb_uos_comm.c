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
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/dma-buf.h>
#include <linux/spinlock.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>

#include "ahdb_drv.h"

struct release_work {
	struct work_struct work;
	struct ahdb_buf *exp;
};

static int pend_msg_add(int req_id)
{
	struct wait_for_resp *new = kcalloc(1, sizeof(*new), GFP_KERNEL);

	if (!new)
		return -ENOMEM;

	new->req_id = req_id;
	new->status = 0;
	hash_add(g_ahdb_info->pending_reqs, &new->node, new->req_id);
	return 0;
}

static void pend_msg_responded(int req_id, int resp)
{
	struct wait_for_resp *found;
	int bkt;

	hash_for_each(g_ahdb_info->pending_reqs, bkt, found, node)
		if (found->req_id == req_id)
			found->status = resp;
}

static int pend_msg_status(int req_id)
{
	struct wait_for_resp *found;
	int bkt;

	hash_for_each(g_ahdb_info->pending_reqs, bkt, found, node)
		if (found->req_id == req_id)
			return found->status;

	return -1;
}

static int pend_msg_del(int req_id)
{
	struct wait_for_resp *found;
	int bkt;

	hash_for_each(g_ahdb_info->pending_reqs, bkt, found, node)
		if (found->req_id == req_id) {
			hash_del(&found->node);
			kfree(found);
			return 0;
		}

	return -1;
}

int send_msg(int vmid, enum ahdb_cmd cmd, int *op)
{
	struct ahdb_msg_long *msg;
	struct scatterlist sg;
	static int msg_num;
	unsigned long flags;
	int itr;
	int i, tx_size;

	/* message always goes to the host */
	if (vmid != 0)
		return -EINVAL;

	switch (cmd) {
	case AHDB_CMD_EMPTY:
	case AHDB_CMD_NEED_VMID:
	case AHDB_CMD_NOTIFY_UNEXPORT:
		msg = kcalloc(1, sizeof(struct ahdb_msg_short),
			      GFP_KERNEL);
		if (!msg)
			return -ENOMEM;

		if (op)
			for (i = 0; i < 4; i++)
				msg->op[i] = op[i];

		tx_size = sizeof(struct ahdb_msg_short);
		break;

	case AHDB_CMD_EXPORT:
		msg = kcalloc(1, sizeof(struct ahdb_msg_long),
			      GFP_KERNEL);
		if (!msg)
			return -ENOMEM;

		memcpy(&msg->op[0], &op[0], 9 * sizeof(int) + op[9]);
		tx_size = sizeof(struct ahdb_msg_long);
		break;

	default:
		/* no command found */
		return -EINVAL;
	}
	msg->cmd = cmd;
	msg->req_id = msg_num++;
	msg->stat = 0;

	pend_msg_add(msg->req_id);

	sg_init_one(&sg, msg, tx_size);

	spin_lock_irqsave(&g_ahdb_info->vq_lock, flags);
	virtqueue_add_inbuf(g_ahdb_info->vq, &sg, 1, msg, GFP_KERNEL);
	virtqueue_kick(g_ahdb_info->vq);
	spin_unlock_irqrestore(&g_ahdb_info->vq_lock, flags);

	/* ~1 sec timeout */
	itr = 10000;
	while (itr--) {
		if (pend_msg_status(msg->req_id) > 0)
			break;

		usleep_range(90, 110);
	}
	pend_msg_del(msg->req_id);

	if (itr < 0) {
		dev_err(g_ahdb_info->dev,
			"request-time out : req-id:%d, cmd:%d\n",
			msg->req_id, msg->cmd);
		return -EBUSY;
	}

	return 0;
}

static void ack(int vmid, struct ahdb_msg_short *msg, int stat)
{
	struct scatterlist sg;

	if (vmid) {
		dev_err(g_ahdb_info->dev, "fe can only send msg to SOS\n");
		return;
	}

	msg->stat = stat;

	sg_init_one(&sg, msg, sizeof(*msg));
	virtqueue_add_inbuf(g_ahdb_info->vq, &sg, 1, msg,
			    GFP_KERNEL);
	virtqueue_kick(g_ahdb_info->vq);
}

static void rel_work_sched(struct work_struct *work)
{
	struct release_work *proc = container_of(work,
				struct release_work, work);

	remove_buf(proc->exp, NULL);
}

static int parse_and_ack(struct ahdb_msg_short *msg)
{
	struct ahdb_buf *exp;
	ahdb_buf_id_t hid;
	struct release_work *rel_work;
	int stat = AHDB_REQ_PROCESSED;
	int ret = 0;

	/* empty message not allowed and ignored */
	if (!msg->cmd) {
		dev_err(g_ahdb_info->dev, "empty cmd\n");
		return -EINVAL;
	}

	hid.id = msg->op[0];
	hid.rng_key[0] = msg->op[1];
	hid.rng_key[1] = msg->op[2];
	hid.rng_key[2] = msg->op[3];

	exp = ahdb_findbuf(hid);
	if (!exp) {
		dev_err(g_ahdb_info->dev, "fail to find buffer\n");
		stat = AHDB_REQ_ERROR;
		goto send_ack;
	}

	switch (msg->cmd) {
	case AHDB_CMD_IMPORT_NOTIFY:
		exp->imported = true;
		break;
	case AHDB_CMD_DMABUF_REL:
		exp->imported = false;

		if (!exp->valid && !exp->unexp_sched) {
			/* If not and buffer is invalid and no other importers
			 * are still using the buffer, close it.
			 */
			rel_work = kcalloc(1, sizeof(*rel_work), GFP_ATOMIC);
			if (!rel_work) {
				stat = AHDB_REQ_ERROR;
				goto send_ack;
			}

			rel_work->exp = exp;

			INIT_WORK(&(rel_work->work), rel_work_sched);
			queue_work(g_ahdb_info->wq, &(rel_work->work));
		}
		break;
	}

send_ack:
	ack(0, msg, stat);
	return ret;
}

/*
 *  Handle requests coming from other VMs
 */
void rx_isr(struct virtqueue *vq)
{
	struct ahdb_msg_short *msg;
	unsigned long irq_flags;
	int sz;

	spin_lock_irqsave(&g_ahdb_info->vq_lock, irq_flags);

	/* Make sure all pending requests will be processed */
	for (;;) {
		msg = virtqueue_get_buf(vq, &sz);
		if (!msg)
			break;

		/* valid size */
		if (sz == sizeof(struct ahdb_msg_short)) {
			/* if it's an acknowledgement */
			if (msg->stat) {
				if (msg->cmd == AHDB_CMD_NEED_VMID) {
					g_ahdb_info->vmid = msg->op[0];
					dev_info(g_ahdb_info->dev,
						 "vmid = %d\n",
						 g_ahdb_info->vmid);
				}
				pend_msg_responded(msg->req_id,
						   msg->stat);
			} else if (parse_and_ack(msg)) {
				dev_err(g_ahdb_info->dev,
					"msg parse error\n");
			}
		} else {
			dev_err(g_ahdb_info->dev,
				"received malformed message\n");
		}
	}

	spin_unlock_irqrestore(&g_ahdb_info->vq_lock, irq_flags);
}
