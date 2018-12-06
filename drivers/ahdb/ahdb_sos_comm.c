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
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/vhm/acrn_common.h>
#include <linux/vhm/acrn_vhm_ioreq.h>
#include <linux/vhm/acrn_vhm_mm.h>

#include "ahdb_drv.h"

static int pend_msg_add(int req_id)
{
	struct wait_for_resp *new = kcalloc(1, sizeof(*new), GFP_KERNEL);

	if (!new)
		return -ENOMEM;

	new->req_id = req_id;
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

/* transmitting message */
int send_msg(int vmid, enum ahdb_cmd cmd, int *op)
{
	struct ahdb_msg_short msg;
	struct ahdb_vdev *vdev;
	static int msg_num;
	int itr;
	int i;

	vdev = vdev_find(vmid);
	if (!vdev) {
		dev_err(g_ahdb_info->dev,
			"can't find vdev : vmid = %d\n", vmid);
		return -EINVAL;
	}

	switch (cmd) {
	case AHDB_CMD_IMPORT_NOTIFY:
	case AHDB_CMD_DMABUF_REL:
		for (i = 0; i < 4; i++)
			msg.op[i] = op[i];
		break;
	default:
		/* no command found */
		return -EINVAL;
	}

	msg.cmd = cmd;
	msg.req_id = msg_num++;
	msg.stat = 0;

	/* curr shouldn't be updated before tx_work is scheduled */
	mutex_lock(&vdev->vq_mutex);
	mutex_lock(&vdev->tx_mutex);

	if ((vdev->next.msg_ptr == NULL) ||
	    (vdev->next.head == -1)) {
		/* no available container. Possibly waiting for the ack
		 * from UOS that will be used as a container for the
		 * next message
		 */
		mutex_unlock(&vdev->tx_mutex);
		mutex_unlock(&vdev->vq_mutex);
		/* TODO: need reserved container in case
		 * consecutive time-out leads to no-container
		 * err
		 */
		dev_err(g_ahdb_info->dev, "no more container\n");
		return -EBUSY;
	}

	memcpy(&vdev->next.msg, &msg, sizeof(msg));
	pend_msg_add(msg.req_id);

	vhost_work_queue(&vdev->dev, &vdev->tx_work);
	mutex_unlock(&vdev->vq_mutex);

	/* 1 sec timeout */
	itr = 10000;
	while (itr--) {
		if (pend_msg_status(msg.req_id) > 0)
			break;

		usleep_range(90, 110);
	}

	pend_msg_del(msg.req_id);

	if (itr < 0) {
		dev_err(g_ahdb_info->dev,
			"req time-out id - req-id:%d cmd:%d\n",
			msg.req_id, msg.cmd);

		mutex_unlock(&vdev->tx_mutex);
		return -EBUSY;
	}

	mutex_unlock(&vdev->tx_mutex);
	return 0;
}

static int reg_exported(ahdb_buf_id_t hid, int *ops)
{
	struct ahdb_buf *imp;

	/* if nents == 0, it means it is a message only for
	 * priv synchronization. for existing imported_sgt_info
	 * so not creating a new one
	 */
	if (ops[4] == 0) {
		imp = ahdb_findbuf(hid);
		if (!imp)
			return -EINVAL;

		/* if size of new private data is different,
		 * we reallocate it.
		 */
		if (imp->sz_priv != ops[9]) {
			kfree(imp->priv);
			imp->sz_priv = ops[9];
			imp->priv = kcalloc(1, ops[9], GFP_KERNEL);
			if (!imp->priv) {
				/* set it invalid */
				imp->valid = 0;
				return -ENOMEM;
			}
		}

		goto skip_shmem;
	}

	imp = kcalloc(1, sizeof(*imp), GFP_KERNEL);
	if (!imp)
		return -ENOMEM;

	imp->sz_priv = ops[9];
	if (imp->sz_priv) {
		imp->priv = kcalloc(1, ops[9], GFP_KERNEL);
		if (!imp->priv) {
			kfree(imp);
			return -ENOMEM;
		}
	}

	memcpy(&imp->hid, &hid, sizeof(hid));

	dev_info(g_ahdb_info->dev,
		 "registering buffer with id = %d, SRC-VM(%d)\n",
		 imp->hid.id, AHDB_VMID(hid));

	imp->nents = ops[4];
	imp->frst_ofst = ops[5];
	imp->last_len = ops[6];
	imp->ref = *(long *)&ops[7];
	imp->valid = 1;

	ahdb_addbuf(imp);

skip_shmem:
	/* transferring private data */
	memcpy(imp->priv, &ops[10], ops[9]);

	return 0;
}

static void send_to_txq(struct ahdb_vdev *vdev, struct txmsg *msg_info)
{
	int ret;

	ret = __copy_to_user(msg_info->msg_ptr, &msg_info->msg,
			     sizeof(struct ahdb_msg_short));

	if (!ret) {
		vhost_add_used_and_signal(&vdev->dev, &vdev->vq,
					  msg_info->head,
					  sizeof(struct ahdb_msg_short));
		msg_info->msg_ptr = NULL;
		msg_info->head = -1;
	} else {
		dev_err(g_ahdb_info->dev,
			"fail to copy tx msg to the container\n");
	}
}

void tx_work(struct vhost_work *work)
{
	struct ahdb_vdev *vdev = container_of(work,
					      struct ahdb_vdev,
					      tx_work);

	mutex_lock(&vdev->vq_mutex);
	send_to_txq(vdev, &vdev->next);
	mutex_unlock(&vdev->vq_mutex);
}

static void ack(int vmid, struct txmsg *msg_info, int stat)
{
	struct ahdb_vdev *vdev;

	vdev = vdev_find(vmid);
	if (!vdev) {
		dev_err(g_ahdb_info->dev,
			"no guest os with vmid = %d\n", vmid);
	} else {
		msg_info->msg.stat = stat;
		send_to_txq(vdev, msg_info);
	}
}

/* parsing incoming message */
static int parse_and_ack(struct ahdb_msg_long *msg, void __user *out, int head)
{
	struct ahdb_buf *imp;
	struct txmsg *msg_info;
	ahdb_buf_id_t hid;
	int stat = AHDB_REQ_PROCESSED;
	int ret = 0;

	memcpy(&hid, msg->op, sizeof(hid));

	msg_info = kcalloc(1, sizeof(*msg_info), GFP_KERNEL);
	if (!msg_info)
		return -ENOMEM;

	switch (msg->cmd) {
	case AHDB_CMD_NOTIFY_UNEXPORT:
		imp = ahdb_findbuf(hid);
		if (imp) {
			if (imp->imported) {
				imp->valid = 0;
			} else {
				dev_info(g_ahdb_info->dev,
					 "Now freeing buffer with id = %d\n",
					 imp->hid.id);

				if (imp->shmem)
					ahdb_unmap(imp->shmem);

				if (imp->sgt) {
					sg_free_table(imp->sgt);
					kfree(imp->sgt);
				}

				ahdb_delbuf(hid);
				kfree(imp->priv);
				kfree(imp);
			}
		} else {
			dev_err(g_ahdb_info->dev,
				"can't find hdmabuf : %d\n", hid.id);
			stat = AHDB_REQ_ERROR;
			ret = -EINVAL;
		}

		memcpy(&msg_info->msg, msg, sizeof(struct ahdb_msg_short));
		break;

	case AHDB_CMD_EXPORT:
		ret = reg_exported(hid, msg->op);
		if (ret) {
			stat = AHDB_REQ_ERROR;
			ret = -EINVAL;
		}

		memcpy(&msg_info->msg, msg, sizeof(struct ahdb_msg_short));
		break;

	default:
		stat = AHDB_REQ_ERROR;
		ret = -EINVAL;
		break;
	}

	msg_info->msg_ptr = out;
	msg_info->head = head;

	ack(AHDB_VMID(hid), msg_info, stat);

	return ret;
}

/* ISR for virqueue kick (for incoming message from FE(UOS)) */
void rx_work(struct vhost_work *work)
{
	struct vhost_virtqueue *vq = container_of(work,
						  struct vhost_virtqueue,
						  poll.work);
	struct ahdb_vdev *vdev = container_of(vq->dev,
					      struct ahdb_vdev,
					      dev);
	struct ahdb_msg_long msg;
	int head, in, out, in_size;
	int ret;

	mutex_lock(&vdev->vq_mutex);
	vhost_disable_notify(&vdev->dev, vq);

	/* Make sure we will process all pending requests */
	for (;;) {
		head = vhost_get_vq_desc(vq, vq->iov, ARRAY_SIZE(vq->iov),
					 &out, &in, NULL, NULL);

		if (unlikely(head < 0))
			break;

		/* Nothing new? Wait for eventfd to tell us they refilled */
		if (head == vq->num) {
			if (unlikely(vhost_enable_notify(&vdev->dev, vq))) {
				vhost_disable_notify(&vdev->dev, vq);
				continue;
			}
			break;
		}

		/* out should be 0 */
		if (out)
			break;

		in_size = iov_length(&vq->iov[0], in);

		if ((in_size == sizeof(struct ahdb_msg_long)) ||
		    (in_size == sizeof(struct ahdb_msg_short))) {
			if (__copy_from_user(&msg, vq->iov[0].iov_base,
					    in_size)) {
				dev_err(g_ahdb_info->dev,
					"fatal err: can't get a message\n");
				continue;
			}

			/* if it's ack */
			if (!msg.cmd || msg.stat) {
				/* update next msg container */
				vdev->next.msg_ptr = vq->iov[0].iov_base;
				vdev->next.head = head;

				/* it is an acknowledge for one of
				 * tx msgs from BE
				 */
				if (msg.cmd)
					pend_msg_responded(msg.req_id,
							   msg.stat);
			} else {
				if (msg.cmd == AHDB_CMD_NEED_VMID) {
					struct txmsg ack;

					ack.msg.req_id = msg.req_id;
					ack.msg.cmd = msg.cmd;
					ack.msg.op[0] = vdev->vmid;
					ack.msg.stat = AHDB_REQ_PROCESSED;
					ack.msg_ptr = vq->iov[0].iov_base;
					ack.head = head;

					send_to_txq(vdev, &ack);
				} else {
					ret = parse_and_ack(&msg,
							    vq->iov[0].iov_base,
							    head);
					if (ret) {
						dev_err(g_ahdb_info->dev,
							"msg parse error: %d",
							ret);
						dev_err(g_ahdb_info->dev,
							" cmd: %d\n", msg.cmd);
					}
				}
			}
		} else {
			dev_err(g_ahdb_info->dev, "rx msg with wrong size\n");

			/* just throw back the message to the client to
			 * empty used buffer
			 */
			vhost_add_used_and_signal(&vdev->dev, vq, head,
						  in_size);
		}
	}

	vhost_enable_notify(&vdev->dev, vq);
	mutex_unlock(&vdev->vq_mutex);
}
