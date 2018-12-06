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
#include <linux/dma-buf.h>

#include "ahdb_drv.h"

/* create sg_table with given pages and other parameters */
static struct sg_table *new_sgt(struct page **pgs,
				int frst_ofst, int last_len,
				int nents)
{
	struct sg_table *sgt;
	struct scatterlist *sgl;
	int i, ret;

	sgt = kmalloc(sizeof(struct sg_table), GFP_KERNEL);
	if (!sgt)
		return NULL;

	ret = sg_alloc_table(sgt, nents, GFP_KERNEL);
	if (ret) {
		kfree(sgt);
		return NULL;
	}

	sgl = sgt->sgl;
	sg_set_page(sgl, pgs[0], PAGE_SIZE-frst_ofst, frst_ofst);

	for (i = 1; i < nents-1; i++) {
		sgl = sg_next(sgl);
		sg_set_page(sgl, pgs[i], PAGE_SIZE, 0);
	}

	/* more than 1 page */
	if (nents > 1) {
		sgl = sg_next(sgl);
		sg_set_page(sgl, pgs[i], last_len, 0);
	}

	return sgt;
}

static struct sg_table *ahdb_ops_map(struct dma_buf_attachment *attachment,
				     enum dma_data_direction dir)
{
	struct ahdb_buf *imp;

	if (!attachment->dmabuf)
		return NULL;

	if (!attachment->dmabuf->priv)
		return NULL;

	imp = (struct ahdb_buf *)attachment->dmabuf->priv;

	/* if buffer has never been mapped */
	if (!imp->sgt) {
		imp->sgt = new_sgt(imp->shmem->pages, imp->frst_ofst,
				   imp->last_len, imp->nents);
		if (!imp->sgt)
			goto err_free_sg;
	}

	if (!dma_map_sg(attachment->dev, imp->sgt->sgl,
			imp->sgt->nents, dir))
		goto err_free_sg;

	return imp->sgt;

err_free_sg:
	if (imp->sgt) {
		sg_free_table(imp->sgt);
		kfree(imp->sgt);
	}

	return NULL;
}

static void ahdb_ops_unmap(struct dma_buf_attachment *attachment,
			   struct sg_table *sg, enum dma_data_direction dir)
{
	dma_unmap_sg(attachment->dev, sg->sgl, sg->nents, dir);
}

static void ahdb_ops_release(struct dma_buf *dma_buf)
{
	struct ahdb_buf *imp;
	int ret;

	if (!dma_buf->priv)
		return;

	imp = (struct ahdb_buf *)dma_buf->priv;

	imp->dma_buf = NULL;

	ahdb_unmap(imp->shmem);
	imp->shmem = NULL;

	if (imp->sgt) {
		sg_free_table(imp->sgt);
		kfree(imp->sgt);
		imp->sgt = NULL;
	}

	/* send request and wait for a response */
	ret = send_msg(AHDB_VMID(imp->hid), AHDB_CMD_DMABUF_REL,
		       (int *)&imp->hid);
	if (ret < 0)
		dev_err(g_ahdb_info->dev,
			"fail to send release notification\n");

	/*
	 * Check if buffer is still valid and if not remove it
	 * from imported list. That has to be done after sending
	 * sync request
	 */
	if (!imp->valid) {
		ahdb_delbuf(imp->hid);
		kfree(imp->priv);
		kfree(imp);
	}
}

static const struct dma_buf_ops ahdb_dmabuf_ops = {
	.map_dma_buf = ahdb_ops_map,
	.unmap_dma_buf = ahdb_ops_unmap,
	.release = ahdb_ops_release,
};

/* exporting dmabuf as fd */
int ahdb_exp_fd(struct ahdb_buf *imp, int flags)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);

	exp_info.ops = &ahdb_dmabuf_ops;

	/* multiple of PAGE_SIZE, not considering offset */
	exp_info.size = imp->nents * PAGE_SIZE;
	exp_info.flags = /* not sure about flag */ 0;
	exp_info.priv = imp;

	if (!imp->dma_buf) {
		imp->dma_buf = dma_buf_export(&exp_info);
		if (IS_ERR(imp->dma_buf))
			return -1;
	}

	return dma_buf_fd(imp->dma_buf, flags);
}
