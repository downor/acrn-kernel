/* SPDX-License-Identifier: (MIT OR GPL-2.0) */

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
 */

#ifndef __ACRN_DRV_H__
#define __ACRN_DRV_H__

#include <linux/ahdb.h>
#include <linux/hashtable.h>
#include "../vhost/vhost.h"

/*
 * ACRN uses physical addresses for memory sharing,
 * so size of one page ref will be 64-bits
 */
#define REFS_PER_PAGE (PAGE_SIZE/sizeof(long))

struct shmem_info {
	long ref;

	/* page array */
	struct page **pages;
	long **l2refs;
	long *l3refs;
	int nents;
#ifdef CONFIG_AHDB_BE
	int vmid;
#endif
};

struct ahdb_buf {
	ahdb_buf_id_t hid;

	/* cross-VM ref number for the buffer */
	long ref;

	struct dma_buf_attachment *attach;
	struct dma_buf *dma_buf;
	struct sg_table *sgt;
	struct shmem_info *shmem;

	/* offset and size info of DMA_BUF */
	int frst_ofst;
	int last_len;
	int nents;

	/* set to 0 when unexported. Importer doesn't
	 * do a new mapping of buffer if valid == 0
	 */
	bool valid;

	/* set if the buffer is imported via import_ioctl */
	bool imported;

	/* size of private */
	size_t sz_priv;
	/* private data associated with the exported buffer */
	void *priv;

#ifndef CONFIG_AHDB_BE
	struct delayed_work unexport;
	bool unexp_sched;
#endif
	/* list for file pointers associated with all user space
	 * application that have exported this same buffer to
	 * another VM. This needs to be tracked to know whether
	 * the buffer can be completely freed.
	 */
	struct file *filp;
	struct hlist_node node;
};

/* long message is only used for exporting */
struct ahdb_msg_long {
	unsigned int req_id;
	unsigned int stat;
	unsigned int cmd;
	unsigned int op[64];
};

struct ahdb_msg_short {
	unsigned int req_id;
	unsigned int stat;
	unsigned int cmd;
	unsigned int op[5];
};

#ifdef CONFIG_AHDB_BE
struct txmsg {
	struct ahdb_msg_short msg;
	void __user *msg_ptr;
	int head;
};

struct ahdb_vdev {
	struct vhost_dev dev;
	struct vhost_virtqueue vq;
	struct vhost_work tx_work;

	int vmid;

	/* synchronization between transmissions */
	struct mutex tx_mutex;
	/* synchronization on tx and rx*/
	struct mutex vq_mutex;

	/* container of next msg */
	struct txmsg next;
	struct hlist_node node;
};

struct ahdb_vdev *vdev_find(int client_id);

#else
struct list_used_id {
	int id;
	struct list_head list;
};
#endif

/* driver information */
struct ahdb_info {
	struct device *dev;

#ifdef CONFIG_AHDB_BE
	/* max 16 ahdb virtio clients */
	DECLARE_HASHTABLE(vdev_list, 4);
#else
	struct virtio_device *vdev;
	struct virtqueue *vq;
	spinlock_t vq_lock;
	struct list_used_id *used_ids;
	int vmid;
#endif
	/* workqueue dedicated to AHDB */
	struct workqueue_struct *wq;

	/* list of HyperDMABUF info
	 * max 128 entries
	 */
	DECLARE_HASHTABLE(buf_list, 7);
	DECLARE_HASHTABLE(pending_reqs, 5);

	struct mutex g_mutex;
};

/* IOCTL definitions
 */
typedef int (*ahdb_ioctl_t)(struct file *filp, void *data);

struct ahdb_ioctl_desc {
	unsigned int cmd;
	int flags;
	ahdb_ioctl_t func;
	const char *name;
};

#define AHDB_IOCTL_DEF(ioctl, _func, _flags)	\
	[_IOC_NR(ioctl)] = {			\
			.cmd = ioctl,		\
			.func = _func,		\
			.flags = _flags,	\
			.name = #ioctl		\
}

#define AHDB_VMID(hid) (((hid.id) >> 24) & 0xFF)

#ifndef CONFIG_AHDB_BE

/* ACRN HYPER_DMABUF IDs */
#define AHDB_BUF_ID_CREATE(domid, cnt) ((((domid) & 0xFF) << 24) | \
				       ((cnt) & 0xFFFFFF))

/* currently maximum number of buffers shared
 * at any given moment is limited to 1000
 */
#define AHDB_MAX_ID 1000

/* adding freed hid to the reusable list */
void add_used_id(int id);

/* freeing the reusasble list */
void free_used_ids_all(void);

/* getting a hid available to use. */
ahdb_buf_id_t get_hid(void);

#endif

/* Messages between Host and Client */

/* List of commands from Guest OS:
 *
 * ------------------------------------------------------------------
 * A. NEED_VMID
 *
 *  making query for vmid of itself
 *
 * req:
 *
 * cmd: AHDB_NEED_VMID
 *
 * ack:
 *
 * cmd: same as req
 * op[0] : vmid of UOS
 *
 * ------------------------------------------------------------------
 * B. EXPORT
 *
 *  export dmabuf to SOS
 *
 * req:
 *
 * cmd: AHDB_CMD_EXPORT
 * op0~op3 : HDMABUF ID
 * op4 : number of pages to be shared
 * op5 : offset of data in the first page
 * op6 : length of data in the last page
 * op7 : upper 32 bit of top-level ref of shared buf
 * op8 : lower 32 bit of top-level ref of shared buf
 * op9 : size of private data
 * op10 ~ op64: User private date associated with the buffer
 *	        (e.g. graphic buffer's meta info)
 *
 * ------------------------------------------------------------------
 * C. NOTIFY_UNEXPORT
 *
 *  notifying SOS that the shared buffer is not available anymore
 *
 * req:
 *
 * cmd: ANDB_CMD_NOTIFY_UNEXPORT
 * op0~op3 : HDMABUF ID
 *
 * ------------------------------------------------------------------
 *
 * List commands from Service OS
 *
 * ------------------------------------------------------------------
 * A. IMPORT
 *
 *  notifying UOS that the shared buffer is imported
 *
 * req:
 *
 * cmd: AHDB_CMD_IMPORT - notifying UOS
 * op0~op3 : HDMABUF ID
 * ------------------------------------------------------------------
 * B. DMABUF_REL
 *
 *  notifying UOS that the shared buffer is released by an importer
 *  (There could still be other importers using the same buffer.)
 *
 * req:
 *
 * cmd: AHDB_CMD_DMABUF_REL
 * op0~op3 : HDMABUF ID
 *
 * ------------------------------------------------------------------
 */

enum ahdb_cmd {
	AHDB_CMD_EMPTY = 0,
	AHDB_CMD_NEED_VMID,
	AHDB_CMD_EXPORT = 0x10,
	AHDB_CMD_NOTIFY_UNEXPORT,
	AHDB_CMD_IMPORT_NOTIFY,
	AHDB_CMD_DMABUF_REL
};

enum ahdb_ack {
	AHDB_REQ_NEW = 0x0,
	AHDB_REQ_PROCESSED,
	AHDB_REQ_NEEDS_FOLLOW_UP,
	AHDB_REQ_ERROR,
};

int ahdb_addbuf(struct ahdb_buf *hdmabuf);

ahdb_buf_id_t ahdb_find_hid(struct dma_buf *dmabuf);

struct ahdb_buf *ahdb_findbuf(ahdb_buf_id_t hid);

int ahdb_delbuf(ahdb_buf_id_t hid);

void ahdb_foreachbuf(void (*func)(struct ahdb_buf *, void *),
		     void *param);

void remove_buf(struct ahdb_buf *exp, void *dummy);

int ahdb_clear_buf(struct ahdb_buf *exp);

struct wait_for_resp {
	int req_id;
	int status;
	struct hlist_node node;
};

#ifdef CONFIG_AHDB_BE
void tx_work(struct vhost_work *work);
#endif

int send_msg(int vmid, enum ahdb_cmd cmd, int *op);

/* externs */

extern struct ahdb_info *g_ahdb_info;

#ifdef CONFIG_AHDB_BE
extern void rx_work(struct vhost_work *work);
extern int ahdb_exp_fd(struct ahdb_buf *imp, int flags);
#else
extern void rx_isr(struct virtqueue *vq);
#endif

/* place holder */
extern int ahdb_unmap(struct shmem_info *shmem);

#endif /* __ACRN_DRV_H__*/
