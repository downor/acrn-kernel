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
 */

#ifndef __LINUX_PUBLIC_AHDB_H__
#define __LINUX_PUBLIC_AHDB_H__

#define MAX_SIZE_PRIV_DATA 192

typedef struct {
	int id;
	/* 12B long Random number */
	int rng_key[3];
} ahdb_buf_id_t;

struct ahdb_e_hdr {
	/* hid of new buf */
	ahdb_buf_id_t hid;
	/* size of private data */
	int size;
};

struct ahdb_e_data {
	struct ahdb_e_hdr hdr;
	/* ptr to private data */
	void __user *data;
};

#define IOCTL_SET_EVENT_READER \
_IOC(_IOC_NONE, 'G', 2, 0)

#define IOCTL_IMPORT \
_IOC(_IOC_NONE, 'G', 3, sizeof(struct ioctl_ahdb_import))
struct ioctl_ahdb_import {
	/* IN parameters */
	/* ahdb buf id to be imported */
	ahdb_buf_id_t hid;
	/* flags */
	int flags;
	/* OUT parameters */
	/* exported dma buf fd */
	int fd;
};

#define IOCTL_VHOST_ADD_FE \
_IOC(_IOC_NONE, 'G', '4', sizeof(struct ioctl_ahdb_vhost_addfe))
struct ioctl_ahdb_vhost_addfe {
	/* IN parameters */
	/* id of UOS */
	int vmid;
};

#define IOCTL_VHOST_DEL_FE \
_IOC(_IOC_NONE, 'G', '5', sizeof(struct ioctl_ahdb_vhost_delfe))
struct ioctl_ahdb_vhost_delfe {
	/* IN parameters */
	/* id of UOS */
	int vmid;
};

/* FRONT-END always exports */
#define IOCTL_EXPORT \
_IOC(_IOC_NONE, 'G', 6, sizeof(struct ioctl_ahdb_export))
struct ioctl_ahdb_export {
	/* IN parameters */
	/* DMA buf fd to be exported */
	int fd;
	/* exported dma buf id */
	ahdb_buf_id_t hid;
	int sz_priv;
	char *priv;
};

#define IOCTL_UNEXPORT \
_IOC(_IOC_NONE, 'G', 7, sizeof(struct ioctl_ahdb_unexport))
struct ioctl_ahdb_unexport {
	/* IN parameters */
	/* id of buf to be unexported */
	ahdb_buf_id_t hid;
	/* delay in ms before unexported */
	int delay_ms;
	/* OUT parameters */
	/* Status of request */
	int stat;
};

#define IOCTL_QUERY \
_IOC(_IOC_NONE, 'G', 8, sizeof(struct ioctl_ahdb_query))
struct ioctl_ahdb_query {
	/* in parameters */
	/* id of buf to be queried */
	ahdb_buf_id_t hid;
	/* item to be queried */
	int item;
	/* OUT parameters */
	/* Value of queried item */
	unsigned long info;
};

/* DMABUF query */
enum ahdb_query {
	AHDB_QUERY_SIZE = 0x10,
	AHDB_QUERY_BUSY,
	AHDB_QUERY_UNEXPORTED,
	AHDB_QUERY_DELAYED_UNEXPORTED,
	AHDB_QUERY_PRIV_INFO_SIZE,
	AHDB_QUERY_PRIV_INFO,
};

#endif //__LINUX_PUBLIC_AHDB_H__
