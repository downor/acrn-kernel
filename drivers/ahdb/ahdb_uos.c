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
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/miscdevice.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/dma-buf.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>
#include <linux/virtio_config.h>

#include "ahdb_drv.h"

/* one global drv_priv */
struct ahdb_info *g_ahdb_info;

/* opening ahdb driver */
static int ahdb_open(struct inode *inode, struct file *filp)
{
	if (!g_ahdb_info) {
		pr_err("AHDB FE driver is not ready\n");
		return -EINVAL;
	}

	filp->private_data = g_ahdb_info;

	return 0;
}

static void force_free(struct ahdb_buf *info, void *param);

/* release ahdb driver */
static int ahdb_release(struct inode *inode, struct file *filp)
{
	/* remove all buffer sharing associated to this driver
	 * operation
	 */
	ahdb_foreachbuf(force_free, filp);
	return 0;
}

/* releasing HyperDMABUF info */
int ahdb_clear_buf(struct ahdb_buf *exp)
{
	/* Start cleanup of buffer in reverse order to exporting */
	ahdb_free_buf(exp->shmem);

	dma_buf_unmap_attachment(exp->attach, exp->sgt,
				 DMA_BIDIRECTIONAL);

	if (exp->dma_buf) {
		dma_buf_detach(exp->dma_buf, exp->attach);
		/* close connection to dma-buf completely */
		dma_buf_put(exp->dma_buf);
		exp->dma_buf = NULL;
	}

	dev_info(g_ahdb_info->dev, "clearing buffer with id = %d\n",
		 exp->hid.id);

	return 0;
}

/* notify SOS for the new HyperDMABUF export */
static int export_notify(struct ahdb_buf *exp, struct page **pages)
{
	int *op;
	int ret;

	op = kcalloc(1, sizeof(int) * 65, GFP_KERNEL);
	if (!op)
		return -ENOMEM;

	/* now create request for importer via ring */
	memcpy(op, &exp->hid, sizeof(exp->hid));

	/* if new pages to be shared */
	if (pages) {
		op[4] = exp->nents;
		op[5] = exp->frst_ofst;
		op[6] = exp->last_len;
		exp->shmem = ahdb_share_buf(pages, exp->nents);
		if (!exp->shmem) {
			kfree(op);
			return -ENOMEM;
		}

		/* op[8] is dummy if running on 32bit system */
		memcpy(&op[7], &exp->shmem->ref, sizeof(long));
	}

	op[9] = exp->sz_priv;

	/* driver/application specific private info */
	memcpy(&op[10], exp->priv, op[9]);

	ret = send_msg(0, AHDB_CMD_EXPORT, op);

	kfree(op);
	return ret;
}

/* In case same buffer was already exported, we skip normal export
 * process and just update private data on SOS and UOS.
 */
static int update_priv(struct ahdb_buf *exp, int sz_priv, void *priv)
{
	int reexp = 1;
	int ret = 0;
	void *temp;

	if (!exp->valid)
		return reexp;

	/*
	 * Check if unexport is already scheduled for that buffer,
	 * if so try to cancel it. If that will fail, buffer needs
	 * to be reexport once again.
	 */
	if (exp->unexp_sched) {
		if (!cancel_delayed_work_sync(&exp->unexport))
			return reexp;

		exp->unexp_sched = 0;
	}

	/* if there's any change in size of private data.
	 * we reallocate space for private data with new size
	 */
	if (sz_priv != exp->sz_priv) {
		/* truncating size */
		if (sz_priv > MAX_SIZE_PRIV_DATA)
			exp->sz_priv = MAX_SIZE_PRIV_DATA;
		else
			exp->sz_priv = sz_priv;

		temp = kcalloc(1, exp->sz_priv, GFP_KERNEL);

		if (!temp)
			return -ENOMEM;

		kfree(exp->priv);
		exp->priv = temp;
	}

	/* update private data in sgt_info with new ones */
	ret = copy_from_user(exp->priv, priv, exp->sz_priv);
	if (ret)
		ret = -EINVAL;
	else {
		/* send an export msg for updating priv in importer */
		ret = export_notify(exp, NULL);
		if (ret < 0)
			ret = -EBUSY;
	}

	return ret;
}

/* return total number of pages referenced by a sgt
 * for pre-calculation of # of pages behind a given sgt
 */
static int num_pgs(struct sg_table *sgt)
{
	struct scatterlist *sgl;
	int len, i;
	/* at least one page */
	int n_pgs = 1;

	sgl = sgt->sgl;

	len = sgl->length - PAGE_SIZE + sgl->offset;

	/* round-up */
	n_pgs += ((len + PAGE_SIZE - 1)/PAGE_SIZE);

	for (i = 1; i < sgt->nents; i++) {
		sgl = sg_next(sgl);

		/* round-up */
		n_pgs += ((sgl->length + PAGE_SIZE - 1) /
			  PAGE_SIZE); /* round-up */
	}

	return n_pgs;
}

/* extract pages referenced by sgt */
static struct page **extr_pgs(struct sg_table *sgt, int nents, int *last_len)
{
	struct scatterlist *sgl;
	struct page **pages;
	struct page **temp_pgs;
	int i, j;
	int len;

	pages =	kmalloc_array(nents, sizeof(struct page *), GFP_KERNEL);
	if (!pages)
		return NULL;

	sgl = sgt->sgl;

	temp_pgs = pages;
	*temp_pgs++ = sg_page(sgl);
	len = sgl->length - PAGE_SIZE + sgl->offset;

	i = 1;
	while (len > 0) {
		*temp_pgs++ = nth_page(sg_page(sgl), i++);
		len -= PAGE_SIZE;
	}

	for (i = 1; i < sgt->nents; i++) {
		sgl = sg_next(sgl);
		*temp_pgs++ = sg_page(sgl);
		len = sgl->length - PAGE_SIZE;
		j = 1;

		while (len > 0) {
			*temp_pgs++ = nth_page(sg_page(sgl), j++);
			len -= PAGE_SIZE;
		}
	}

	*last_len = len + PAGE_SIZE;

	return pages;
}

/* ioctl - exporting new HyperDMABUF
 *
 *	 int dmabuf_fd - File handle of original DMABUF
 *	 ahdb_buf_id_t hid - returned HyperDMABUF ID
 *	 int sz_priv - size of private data from userspace
 *	 char *priv - buffer of user private data
 *
 */
static int export_ioctl(struct file *filp, void *data)
{
	struct ioctl_ahdb_export *attr =
			(struct ioctl_ahdb_export *)data;

	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct ahdb_buf *exp;
	struct page **pages;
	ahdb_buf_id_t hid;
	int ret = 0;

	dmabuf = dma_buf_get(attr->fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);

	/* we check if this specific attachment was already exported
	 * to the same domain and if yes and it's valid sgt_info,
	 * it returns hyper_dmabuf_id of pre-exported sgt_info
	 */
	hid = ahdb_find_hid(dmabuf);
	if (hid.id != -1) {
		exp = ahdb_findbuf(hid);
		ret = update_priv(exp, attr->sz_priv, attr->priv);

		/* return if fastpath_export succeeds or gets some
		 * fatal error
		 */
		if (ret <= 0) {
			dma_buf_put(dmabuf);
			attr->hid = hid;

			if (ret < 0)
				dev_err(g_ahdb_info->dev,
					"fail to update private data\n");

			return ret;
		}
	}

	hid = get_hid();

	/* no more exported dmabuf allowed */
	if (hid.id == -1) {
		ret = -ENOMEM;
		goto fail_no_more_hid;
	}

	attach = dma_buf_attach(dmabuf, g_ahdb_info->dev);
	if (IS_ERR(attach)) {
		ret = PTR_ERR(attach);
		goto fail_attach;
	}

	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		ret = PTR_ERR(sgt);
		goto fail_map_attachment;
	}

	exp = kcalloc(1, sizeof(*exp), GFP_KERNEL);
	if (!exp) {
		ret = -ENOMEM;
		goto fail_sgt_info_creation;
	}

	/* possible truncation */
	if (attr->sz_priv > MAX_SIZE_PRIV_DATA)
		exp->sz_priv = MAX_SIZE_PRIV_DATA;
	else
		exp->sz_priv = attr->sz_priv;

	/* creating buffer for private data of buffer */
	if (exp->sz_priv != 0) {
		exp->priv = kcalloc(1, exp->sz_priv, GFP_KERNEL);
		if (!exp->priv) {
			ret = -ENOMEM;
			goto fail_priv_creation;
		}
	}

	exp->hid = hid;
	exp->attach = attach;
	exp->sgt = sgt;
	exp->dma_buf = dmabuf;
	exp->valid = 1;

	if (exp->sz_priv) {
		/* copy private data to sgt_info */
		ret = copy_from_user(exp->priv, attr->priv, exp->sz_priv);
		if (ret) {
			ret = -EINVAL;
			goto fail_exp;
		}
	}

	exp->frst_ofst = sgt->sgl->offset;
	exp->nents = num_pgs(sgt);

	pages = extr_pgs(sgt, exp->nents, &exp->last_len);
	if (pages == NULL) {
		ret = -ENOMEM;
		goto fail_exp;
	}

	/* TODO: we probably shouldn't do this for better security. */
	/* TODO: event read might need to export this */
	attr->hid = exp->hid;
	ret = export_notify(exp, pages);
	if (ret < 0)
		goto fail_send_request;

	/* now register it to export list */
	ahdb_addbuf(exp);

	exp->filp = filp;

	dev_info(g_ahdb_info->dev, "exporting new buf with id = %d\n",
		 exp->hid.id);

	return ret;

/* Clean-up if error occurs */
fail_send_request:
	ahdb_free_buf(exp->shmem);
	kfree(pages);

fail_exp:
	kfree(exp->priv);

fail_priv_creation:
	kfree(exp);

fail_sgt_info_creation:
	dma_buf_unmap_attachment(attach, sgt,
				 DMA_BIDIRECTIONAL);

fail_map_attachment:
	dma_buf_detach(dmabuf, attach);

fail_attach:
fail_no_more_hid:
	dma_buf_put(dmabuf);

	return ret;
}

void remove_buf(struct ahdb_buf *exp, void *dummy)
{
	ahdb_clear_buf(exp);
	ahdb_delbuf(exp->hid);

	/* register buf id to the list for reuse */
	add_used_id(exp->hid.id);

	if (exp->sz_priv > 0 && !exp->priv)
		kfree(exp->priv);

	kfree(exp);
}

/* unexport dmabuf from the database and send int req to the source domain
 * to unmap it.
 */
static void delayed_unexp(struct work_struct *work)
{
	struct ahdb_buf *exp;

	exp = container_of(work, struct ahdb_buf, unexport.work);

	/* no longer valid */
	exp->valid = 0;
	exp->unexp_sched = 0;

	if (!exp->imported)
		remove_buf(exp, NULL);
}

/* ioctl - scheduling unexport of dmabuf
 *
 *	 ahdb_buf_id_t hid - ID of HyperDMABUF that needs to be unexported
 *	 int delay_ms - user provided delay (in ms) before actual termination
 *	 int stat - returned result
 *
 */
static int unexport_ioctl(struct file *filp, void *data)
{
	struct ioctl_ahdb_unexport *attr =
			(struct ioctl_ahdb_unexport *)data;
	struct ahdb_buf *exp;
	int ret;

	/* find dmabuf in export list */
	exp = ahdb_findbuf(attr->hid);

	/* failed to find corresponding entry in export list */
	if (exp == NULL) {
		attr->stat = -ENOENT;
		return -ENOENT;
	}

	if (exp->unexp_sched)
		return 0;

	/* Now send unexport request to remote domain, marking
	 * that buffer should not be used anymore
	 */
	ret = send_msg(0, AHDB_CMD_NOTIFY_UNEXPORT, (int *)&attr->hid);
	if (ret < 0)
		/* fail to send notification but moving on with unexport
		 * process
		 */
		dev_err(g_ahdb_info->dev,
			"fail to send unexport notification\n");

	INIT_DELAYED_WORK(&exp->unexport, delayed_unexp);
	schedule_delayed_work(&exp->unexport,
			      msecs_to_jiffies(attr->delay_ms));

	exp->unexp_sched = true;

	return 0;
}

static void force_free(struct ahdb_buf *info, void *param)
{
	struct ioctl_ahdb_unexport attr;
	struct file *filp = (struct file *)param;

	if (info->filp == filp) {
		attr.hid = info->hid;
		attr.delay_ms = 0;
		unexport_ioctl(filp, &attr);
	}
}

static const struct ahdb_ioctl_desc ahdb_ioctls[] = {
	AHDB_IOCTL_DEF(IOCTL_EXPORT, export_ioctl, 0),
	AHDB_IOCTL_DEF(IOCTL_UNEXPORT, unexport_ioctl, 0),
};

long ahdb_ioctl(struct file *filp, unsigned int cmd,
		unsigned long param)
{
	const struct ahdb_ioctl_desc *ioctl = NULL;
	unsigned int nr = _IOC_NR(cmd);
	int ret;
	ahdb_ioctl_t func;
	char *kdata;

	if (nr >= ARRAY_SIZE(ahdb_ioctls)) {
		dev_err(g_ahdb_info->dev, "invalid ioctl\n");
		return -EINVAL;
	}

	ioctl = &ahdb_ioctls[nr];

	func = ioctl->func;

	if (unlikely(!func)) {
		dev_err(g_ahdb_info->dev, "no function\n");
		return -EINVAL;
	}

	kdata = kmalloc(_IOC_SIZE(cmd), GFP_KERNEL);
	if (!kdata)
		return -ENOMEM;

	if (copy_from_user(kdata, (void __user *)param,
			   _IOC_SIZE(cmd)) != 0) {
		dev_err(g_ahdb_info->dev,
			"failed to copy from user arguments\n");
		ret = -EFAULT;
		goto ioctl_error;
	}

	ret = func(filp, kdata);

	if (copy_to_user((void __user *)param, kdata,
			 _IOC_SIZE(cmd)) != 0) {
		dev_err(g_ahdb_info->dev,
			"failed to copy to user arguments\n");
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
	.unlocked_ioctl = ahdb_ioctl,
};

static struct miscdevice ahdb_miscdev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ahdb_fe",
	.fops = &ahdb_fops,
};

static int ahdb_vdev_probe(struct virtio_device *vdev)
{
	vq_callback_t *cb[] = {rx_isr};
	static const char * const name[] = {"ahdb_virtqueue"};
	int ret;

	if (!g_ahdb_info)
		return -EINVAL;

	/* Set vmid to -1 to mark that it is not initialized yet */
	g_ahdb_info->vmid = -1;
	g_ahdb_info->vdev = vdev;
	vdev->priv = g_ahdb_info;

	pr_info("ahdb: initialize vq_lock\n");
	/* initialize spinlock for synchronizing virtqueue accesses */
	spin_lock_init(&g_ahdb_info->vq_lock);

	ret = virtio_find_vqs(g_ahdb_info->vdev, 1, &g_ahdb_info->vq, cb,
			      name, NULL);
	if (ret) {
		kfree(g_ahdb_info);
		g_ahdb_info = NULL;
		return ret;
	}

	return 0;
}

static void ahdb_vdev_remove(struct virtio_device *vdev)
{
	if (!g_ahdb_info)
		return;

	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);
}

/*
 * Queues empty requests buffers to backend,
 * which will be used by it to send requests back to frontend.
 */
static void ahdb_vdev_scan(struct virtio_device *vdev)
{
	int ret;

	/* Send AHDB_CMD_NEED_VMID reques to know vmid
	 */
	ret = send_msg(0, AHDB_CMD_NEED_VMID, 0);
	if (ret < 0) {
		dev_err(g_ahdb_info->dev, "fail to receive vmid\n");
		return;
	}

	ret = send_msg(0, AHDB_CMD_EMPTY, 0);
	if (ret < 0)
		dev_err(g_ahdb_info->dev, "fail to send empty buffer\n");
}

#ifdef CONFIG_PM_SLEEP
static int ahdb_vdev_freeze(struct virtio_device *vdev)
{
	ahdb_vdev_remove(vdev);
	return 0;
}

struct vdev_restore_work {
	struct work_struct work;
	struct virtio_device *dev;
};

static void proc_restore_work(struct work_struct *work)
{
	struct vdev_restore_work *rw = (struct vdev_restore_work *)work;

	while (!(VIRTIO_CONFIG_S_DRIVER_OK &
		rw->dev->config->get_status(rw->dev))) {
		usleep_range(100, 120);
	}

	ahdb_vdev_scan(rw->dev);
}

static int ahdb_vdev_restore(struct virtio_device *vdev)
{
	struct vdev_restore_work *w;
	int ret;

	ret = ahdb_vdev_probe(vdev);
	if (!ret) {
		w = kcalloc(1, sizeof(*w), GFP_KERNEL);
		INIT_WORK(&w->work, proc_restore_work);
		w->dev = vdev;
		schedule_work(&w->work);
	}

	return ret;
}
#endif

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_AHDB, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver ahdb_vdev_drv = {
	.driver.name =  KBUILD_MODNAME,
	.driver.owner = THIS_MODULE,
	.id_table =     id_table,
	.probe =        ahdb_vdev_probe,
	.remove =       ahdb_vdev_remove,
	.scan =         ahdb_vdev_scan,
#ifdef CONFIG_PM_SLEEP
	.freeze =       ahdb_vdev_freeze,
	.restore =      ahdb_vdev_restore,
#endif
};

static int __init init(void)
{
	int ret = 0;

	pr_info("ahdb: initialization started\n");

	g_ahdb_info = NULL;

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
	mutex_lock(&g_ahdb_info->g_mutex);

	hash_init(g_ahdb_info->buf_list);
	hash_init(g_ahdb_info->pending_reqs);

	g_ahdb_info->wq = create_workqueue("ahdb_wq");

	ret = register_virtio_driver(&ahdb_vdev_drv);
	if (ret) {
		dev_err(g_ahdb_info->dev, "failed to register vdev\n");
		misc_deregister(&ahdb_miscdev);
		mutex_unlock(&g_ahdb_info->g_mutex);
		kfree(g_ahdb_info);
		return -EFAULT;
	}

	mutex_unlock(&g_ahdb_info->g_mutex);

	dev_info(g_ahdb_info->dev, "finishing up initialization of DRV\n");

	return 0;
}

static void __exit fini(void)
{
	dev_info(g_ahdb_info->dev, "unregister_device() is called\n");
	misc_deregister(&ahdb_miscdev);

	mutex_lock(&g_ahdb_info->g_mutex);

	unregister_virtio_driver(&ahdb_vdev_drv);

	if (g_ahdb_info->wq)
		destroy_workqueue(g_ahdb_info->wq);

	free_used_ids_all();

	/* freeing all exported buffers */
	ahdb_foreachbuf(remove_buf, NULL);

	mutex_unlock(&g_ahdb_info->g_mutex);

	kfree(g_ahdb_info);
	g_ahdb_info = NULL;
}

module_init(init);
module_exit(fini);

MODULE_DEVICE_TABLE(virtio, ahdb_id_table);
MODULE_DESCRIPTION("ACRN HyperDMABUF UOS Drv");
MODULE_LICENSE("GPL and additional rights");
MODULE_AUTHOR("Intel Corporation");
