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

#include <linux/init.h>
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/workqueue.h>
#include <linux/slab.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/poll.h>
#include <linux/dma-buf.h>
#include <linux/vhost.h>

#include "ahdb_drv.h"

/* global driver information */
struct ahdb_info *g_ahdb_info;

/* adding new virtual dev info to the hash */
static int vdev_add(struct ahdb_vdev *new)
{
	hash_add(g_ahdb_info->vdev_list, &new->node,
		 new->vmid);
	return 0;
}

/* find virtual dev info */
struct ahdb_vdev *vdev_find(int vmid)
{
	struct ahdb_vdev *found;
	int bkt;

	hash_for_each(g_ahdb_info->vdev_list, bkt, found, node)
		if (found->vmid == vmid)
			return found;

	return NULL;
}

/* remove all virtual dev info */
static void vdev_del_all(void)
{
	struct ahdb_vdev *found;
	int bkt;

	hash_for_each(g_ahdb_info->vdev_list, bkt, found, node)
		hash_del(&found->node);
}

/* ahdb driver open */
static int ahdb_open(struct inode *inode, struct file *filp)
{
	struct ahdb_vdev *vdev;
	int ret = 0;

	if (!g_ahdb_info) {
		pr_err("misc ahdb_be: can't open AHDB device\n");
		return -EINVAL;
	}

	mutex_lock(&g_ahdb_info->g_mutex);

	/* if device is opened with 'O_RDWR',
	 * open() sets up a new vdev structure
	 * for a vhost client connection
	 */
	if (filp->f_flags & O_RDWR) {
		struct vhost_virtqueue **vqs =
				kcalloc(1, sizeof(*vqs), GFP_KERNEL);
		if (!vqs)
			return -ENOMEM;

		vdev = kvzalloc(sizeof(*vdev), GFP_KERNEL |
				__GFP_RETRY_MAYFAIL);
		if (!vdev) {
			kfree(vqs);
			return -ENOMEM;
		}

		vqs[0] = &vdev->vq;
		vdev->vq.handle_kick = rx_work;

		mutex_init(&vdev->vq_mutex);
		mutex_init(&vdev->tx_mutex);
		vhost_dev_init(&vdev->dev, vqs, 1);
		mutex_lock(&vdev->dev.mutex);

		vhost_work_init(&vdev->tx_work, tx_work);
		vdev_add(vdev);

		mutex_unlock(&vdev->dev.mutex);

		filp->private_data = vdev;
	/* Opening dev with 'read-only' for HyperDMABUF sharing */
	} else if ((filp->f_flags & O_ACCMODE) == O_RDONLY) {
		filp->private_data = g_ahdb_info;
	} else {
		ret = -1;
	}

	mutex_unlock(&g_ahdb_info->g_mutex);

	return ret;
}

/* ahdb driver release */
static int ahdb_release(struct inode *inode, struct file *filp)
{
	struct ahdb_vdev *vdev =
			(struct ahdb_vdev *)filp->private_data;

	/* if opened with O_RDWR access permission, vdev is what
	 * will be closed
	 */
	if (filp->f_flags & O_RDWR) {
		vdev = (struct ahdb_vdev *)filp->private_data;
		vhost_poll_stop(&vdev->vq.poll);
		vhost_poll_flush(&vdev->vq.poll);
		vhost_dev_cleanup(&vdev->dev);
		kfree(vdev->dev.vqs);
		kfree(vdev);
	}

	filp->private_data = NULL;
	return 0;
}

/* ahdb driver dev polling for event */
static unsigned int ahdb_ep(struct file *filp,
			    struct poll_table_struct *wait)
{
	struct ahdb_info *drv_info =
		(struct ahdb_info *) filp->private_data;

	if (drv_info->eq->pid != task_tgid_nr(current)) {
		dev_err(drv_info->dev,
			"current user process is not allowed to poll dev\n");
		return -EINVAL;
	}

	poll_wait(filp, &drv_info->eq->e_wait, wait);

	if (!list_empty(&drv_info->eq->e_list))
		return POLLIN | POLLRDNORM;

	return 0;
}

/* ahdb driver reading event information for new imported buffer */
static ssize_t ahdb_e_read(struct file *filp, char __user *buf,
			   size_t cnt, loff_t *ofst)
{
	struct ahdb_info *drv_info =
		(struct ahdb_info *) filp->private_data;
	int ret;

	if (drv_info->eq->pid != task_tgid_nr(current)) {
		dev_err(drv_info->dev,
			"current user process is not allowed to read events\n");
		return -EPERM;
	}

	/* only root can read events */
	if (!capable(CAP_DAC_OVERRIDE)) {
		dev_err(drv_info->dev, "only root can read events\n");
		return -EPERM;
	}

	/* make sure user buffer can be written */
	if (!access_ok(VERIFY_WRITE, buf, cnt)) {
		dev_err(drv_info->dev, "user buffer can't be written.\n");
		return -EINVAL;
	}

	ret = mutex_lock_interruptible(&drv_info->eq->e_readlock);
	if (ret)
		return ret;

	for (;;) {
		struct ahdb_event *e = NULL;

		spin_lock_irq(&drv_info->eq->e_lock);
		if (!list_empty(&drv_info->eq->e_list)) {
			e = list_first_entry(&drv_info->eq->e_list,
					     struct ahdb_event, link);
			list_del(&e->link);
		}
		spin_unlock_irq(&drv_info->eq->e_lock);

		if (!e) {
			if (ret)
				break;

			if (filp->f_flags & O_NONBLOCK) {
				ret = -EAGAIN;
				break;
			}

			mutex_unlock(&drv_info->eq->e_readlock);
			ret = wait_event_interruptible(drv_info->eq->e_wait,
					!list_empty(&drv_info->eq->e_list));

			if (ret == 0)
				ret = mutex_lock_interruptible(
						&drv_info->eq->e_readlock);

			if (ret)
				return ret;
		} else {
			unsigned int len = (sizeof(e->e_data.hdr) +
					    e->e_data.hdr.size);

			if (len > cnt - ret) {
put_back_event:
				spin_lock_irq(&drv_info->eq->e_lock);
				list_add(&e->link, &drv_info->eq->e_list);
				spin_unlock_irq(&drv_info->eq->e_lock);
				break;
			}

			if (copy_to_user(buf + ret, &e->e_data.hdr,
					 sizeof(e->e_data.hdr))) {
				if (ret == 0)
					ret = -EFAULT;

				goto put_back_event;
			}

			ret += sizeof(e->e_data.hdr);

			if (copy_to_user(buf + ret, e->e_data.data,
					 e->e_data.hdr.size)) {
				/* error while copying void *data */

				struct ahdb_e_hdr dummy_hdr = {0};

				ret -= sizeof(e->e_data.hdr);

				/* nullifying hdr of the event in user buffer */
				if (copy_to_user(buf + ret, &dummy_hdr,
						 sizeof(dummy_hdr)))
					dev_err(drv_info->dev,
					   "fail to nullify invalid hdr\n");

				ret = -EFAULT;

				goto put_back_event;
			}

			ret += e->e_data.hdr.size;
			drv_info->eq->pending--;
			kfree(e);
		}
	}

	mutex_unlock(&drv_info->eq->e_readlock);

	return ret;
}

/* vhost interface owner reset */
static long vhost_reset_owner(struct ahdb_vdev *vdev)
{
	long err;
	struct vhost_umem *umem;

	mutex_lock(&vdev->dev.mutex);
	err = vhost_dev_check_owner(&vdev->dev);
	if (err)
		goto done;

	umem = vhost_dev_reset_owner_prepare();
	if (!umem) {
		err = -ENOMEM;
		goto done;
	}

	vhost_poll_stop(&vdev->vq.poll);
	vhost_poll_flush(&vdev->vq.poll);

	vhost_dev_reset_owner(&vdev->dev, umem);
done:
	mutex_unlock(&vdev->dev.mutex);
	return err;
}

/* wrapper ioctl for vhost interface control */
static int vhost_ioctl(struct file *filp, unsigned int cmd,
		       unsigned long param)
{
	struct ahdb_vdev *vdev =
		(struct ahdb_vdev *) filp->private_data;
	int vmid;
	int ret;

	switch (cmd) {
	case VHOST_SET_VMID:
		if (copy_from_user(&vdev->vmid, (void __user *)param,
				   sizeof(vmid)))
			ret = -EINVAL;
		else
			ret = 0;
		break;
	case VHOST_GET_FEATURES:
		/* TODO: future implementation */
		return 0;
	case VHOST_SET_FEATURES:
		/* TODO: future implementation */
		return 0;
	case VHOST_RESET_OWNER:
		return vhost_reset_owner(vdev);

	default:
		mutex_lock(&g_ahdb_info->g_mutex);
		ret = vhost_dev_ioctl(&vdev->dev, cmd, (void __user *)param);
		if (ret == -ENOIOCTLCMD) {
			ret = vhost_vring_ioctl(&vdev->dev, cmd,
						(void __user *)param);
		} else {
			vhost_poll_flush(&vdev->vq.poll);
		}
		mutex_unlock(&g_ahdb_info->g_mutex);
	}

	return ret;
}

/*
 * ioctl - set pid of user process that will manage import events
 */
static int set_e_reader_ioctl(struct file *filp, void *data)
{
	struct ahdb_info *drv_info =
		(struct ahdb_info *) filp->private_data;

	mutex_lock(&drv_info->eq->e_readlock);

	/* only process with this pid will be able to read from
	 * event queue
	 */
	drv_info->eq->pid = task_tgid_nr(current);
	dev_info(drv_info->dev,
		 "new event reader: user process with id = %d\n",
		 drv_info->eq->pid);

	mutex_unlock(&drv_info->eq->e_readlock);

	return 0;
}

/*
 * ioctl - importing HyperDMABUF from guest OS
 *
 * user parameters:
 *
 *	ahdb_buf_id_t hid - HyperDMABUF ID of imported buffer
 *	int flags - flags
 *	int fd - file handle of	the imported buffer
 *
 */
static int import_ioctl(struct file *filp, void *data)
{
	struct ahdb_info *drv_info =
		(struct ahdb_info *) filp->private_data;

	struct ioctl_ahdb_import *attr =
			(struct ioctl_ahdb_import *)data;

	struct ahdb_buf *imp;
	int ret = 0;
	ahdb_buf_id_t hid = attr->hid;
	int vmid = AHDB_VMID(hid);

	/* look for dmabuf for the id */
	imp = ahdb_findbuf(hid);

	/* can't find hdmabuf from the table */
	if (!imp || !imp->valid) {
		dev_err(drv_info->dev,
			"no valid hdmabuf found: hid.id = %d\n", hid.id);
		return -ENOENT;
	}

	mutex_lock(&drv_info->g_mutex);

	if (!imp->shmem) {
		imp->shmem = ahdb_map(vmid, imp->ref, imp->nents);
		if (!imp->shmem) {
			dev_err(drv_info->dev,
				"failed to map remote pages\n");
			goto fail_map;
		}
	}

	attr->fd = ahdb_exp_fd(imp, attr->flags);
	if (attr->fd < 0) {
		dev_err(drv_info->dev, "failed to get file descriptor\n");
		goto fail_export;
	}

	/* for the first import */
	if (!imp->imported) {
		/* send notification for import */
		ret = send_msg(vmid, AHDB_CMD_IMPORT_NOTIFY, (int *)&hid);
		if (!ret)
			imp->imported = true;
	}
	goto success;

fail_export:
	/* no other importers */
	if (!imp->imported) {
		ahdb_unmap(imp->shmem);
		imp->shmem = NULL;

		if (imp->sgt) {
			sg_free_table(imp->sgt);
			kfree(imp->sgt);
			imp->sgt = NULL;
		}
	}

fail_map:
	/* Check if buffer is still valid and if not remove it
	 * from imported list. That has to be done after sending
	 * sync request
	 */
	if (!imp->valid && !imp->imported) {
		ahdb_delbuf(imp->hid);
		kfree(imp->priv);
		kfree(imp);
	}

	ret =  attr->fd;

success:
	mutex_unlock(&drv_info->g_mutex);
	return ret;
}

/*
 * ioctl - querying various information of HyperDMABUF
 *
 * user parameters:
 *
 *	ahdb_buf_id_t hid - HyperDMABUF ID of imported buffer
 *	int item - querying topic
 *	unsigned long info - returned querying result
 *
 */
static int query_ioctl(struct file *filp, void *data)
{
	struct ioctl_ahdb_query *attr =
			(struct ioctl_ahdb_query *)data;
	struct ahdb_buf *imp;
	int ret = 0;
	ahdb_buf_id_t hid = attr->hid;

	/* query for imported dmabuf */
	imp = ahdb_findbuf(hid);
	if (!imp)
		return -EINVAL;

	switch (attr->item) {
	/* size of dmabuf in byte */
	case AHDB_QUERY_SIZE:
		if (imp->dma_buf) {
			/* if local dma_buf is created (if it's
			 * ever mapped), retrieve it directly
			 * from struct dma_buf *
			 */
			attr->info = imp->dma_buf->size;
		} else {
			/* calcuate it from given nents, frst_ofst
			 * and last_len
			 */
			attr->info = ((imp->nents)*PAGE_SIZE -
				     (imp->frst_ofst) - PAGE_SIZE +
				     (imp->last_len));
		}
		break;

	/* whether the buffer is used or not */
	case AHDB_QUERY_BUSY:
		/* checks if it's used by importer */
		attr->info = imp->imported;
		break;

	/* whether the buffer is unexported */
	case AHDB_QUERY_UNEXPORTED:
		attr->info = !imp->valid;
		break;

	/* size of private info attached to buffer */
	case AHDB_QUERY_PRIV_INFO_SIZE:
		attr->info = imp->sz_priv;
		break;

	/* copy private info attached to buffer */
	case AHDB_QUERY_PRIV_INFO:
		if (imp->sz_priv > 0) {
			int n;

			n = copy_to_user((void __user *)attr->info,
					imp->priv,
					imp->sz_priv);
			if (n != 0)
				return -EINVAL;
		}
		break;

	default:
		return -EINVAL;
	}

	return ret;
}

static const struct ahdb_ioctl_desc ahdb_ioctls[] = {
	AHDB_IOCTL_DEF(IOCTL_SET_EVENT_READER, set_e_reader_ioctl, 0),
	AHDB_IOCTL_DEF(IOCTL_IMPORT, import_ioctl, 0),
	AHDB_IOCTL_DEF(IOCTL_QUERY, query_ioctl, 0),
};

/* entry point of AHDB ioctl */
long ahdb_ioctl(struct file *filp, unsigned int cmd,
		unsigned long param)
{
	struct ahdb_info *drv_info =
		(struct ahdb_info *) filp->private_data;

	const struct ahdb_ioctl_desc *ioctl;
	unsigned int nr;
	int ret;
	ahdb_ioctl_t func;
	char *kdata;

	/* check if cmd is vhost's */
	if (_IOC_TYPE(cmd) == VHOST_VIRTIO) {
		ret = vhost_ioctl(filp, cmd, param);
		return ret;
	}

	/* AHDB IOCTLs */
	nr = _IOC_NR(cmd);

	if (nr >= ARRAY_SIZE(ahdb_ioctls)) {
		dev_err(drv_info->dev, "invalid ioctl\n");
		return -EINVAL;
	}

	ioctl = &ahdb_ioctls[nr];

	func = ioctl->func;

	if (unlikely(!func)) {
		dev_err(drv_info->dev, "no function\n");
		return -EINVAL;
	}

	kdata = kmalloc(_IOC_SIZE(cmd), GFP_KERNEL);
	if (!kdata)
		return -ENOMEM;

	if (copy_from_user(kdata, (void __user *)param,
			   _IOC_SIZE(cmd)) != 0) {
		dev_err(drv_info->dev,
			"failed to copy args from userspace\n");
		ret = -EFAULT;
		goto ioctl_error;
	}

	ret = func(filp, kdata);

	if (copy_to_user((void __user *)param, kdata,
			 _IOC_SIZE(cmd)) != 0) {
		dev_err(drv_info->dev,
			"failed to copy args back to userspace\n");
		ret = -EFAULT;
		goto ioctl_error;
	}

ioctl_error:
	kfree(kdata);
	return ret;
}

static const struct file_operations ahdb_fops = {
	.owner = THIS_MODULE,
	.open = ahdb_open,
	.release = ahdb_release,
	.read = ahdb_e_read,
	.poll = ahdb_ep,
	.unlocked_ioctl = ahdb_ioctl,
};

static struct miscdevice ahdb_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ahdb_be",
	.fops = &ahdb_fops,
};

static int __init init(void)
{
	int ret = 0;

	g_ahdb_info = NULL;

	pr_info("ahdb: initialization started\n");

	ret = misc_register(&ahdb_miscdev);
	if (ret) {
		pr_err("ahdb: driver can't be registered\n");
		return ret;
	}

	/* TODO: Check if there is a different way to initialize dma mask */
	dma_coerce_mask_and_coherent(ahdb_miscdev.this_device,
				     DMA_BIT_MASK(64));

	g_ahdb_info = kcalloc(1, sizeof(*g_ahdb_info), GFP_KERNEL);
	if (!g_ahdb_info) {
		misc_deregister(&ahdb_miscdev);
		return -ENOMEM;
	}

	g_ahdb_info->dev = ahdb_miscdev.this_device;

	mutex_init(&g_ahdb_info->g_mutex);

	ret = ahdb_init_sysfs(g_ahdb_info->dev);
	if (ret < 0)
		dev_err(g_ahdb_info->dev, "failed to initialize sysfs\n");

	g_ahdb_info->eq = kcalloc(1, sizeof(*g_ahdb_info->eq), GFP_KERNEL);
	if (!g_ahdb_info->eq) {
		misc_deregister(&ahdb_miscdev);
		kfree(g_ahdb_info);
		return -ENOMEM;
	}

	mutex_init(&g_ahdb_info->eq->e_readlock);
	spin_lock_init(&g_ahdb_info->eq->e_lock);

	/* Initialize event queue */
	INIT_LIST_HEAD(&g_ahdb_info->eq->e_list);
	init_waitqueue_head(&g_ahdb_info->eq->e_wait);

	/* resetting number of pending events */
	g_ahdb_info->eq->pending = 0;

	g_ahdb_info->wq = create_workqueue("ahdb_wq");
	hash_init(g_ahdb_info->vdev_list);
	hash_init(g_ahdb_info->buf_list);
	hash_init(g_ahdb_info->pending_reqs);

	mutex_unlock(&g_ahdb_info->g_mutex);

	dev_info(g_ahdb_info->dev, "finishing up %s\n", __func__);

	return 0;
}

static void __exit fini(void)
{
	struct ahdb_event *e, *et;
	unsigned long irqflags;

	dev_info(g_ahdb_info->dev, "unregister_device() is called\n");

	mutex_lock(&g_ahdb_info->g_mutex);
	misc_deregister(&ahdb_miscdev);
	ahdb_remove_sysfs(g_ahdb_info->dev);

	/* destroy workqueue */
	if (g_ahdb_info->wq)
		destroy_workqueue(g_ahdb_info->wq);

	/* clean up event queue */
	spin_lock_irqsave(&g_ahdb_info->eq->e_lock, irqflags);

	list_for_each_entry_safe(e, et, &g_ahdb_info->eq->e_list,
				 link) {
		list_del(&e->link);
		kfree(e);
		g_ahdb_info->eq->pending--;
	}

	if (g_ahdb_info->eq->pending)
		dev_err(g_ahdb_info->dev, "possible leak on the e_list\n");

	spin_unlock_irqrestore(&g_ahdb_info->eq->e_lock, irqflags);


	/* remove all connection with UOSes */
	vdev_del_all();

	mutex_unlock(&g_ahdb_info->g_mutex);

	kfree(g_ahdb_info);
	g_ahdb_info = NULL;
}

module_init(init);
module_exit(fini);

MODULE_DESCRIPTION("ACRN HyperDMABUF SOS Drv");
MODULE_LICENSE("GPL and additional rights");
