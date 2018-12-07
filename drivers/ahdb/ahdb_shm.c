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
#include <linux/slab.h>
#include <linux/mm.h>

#ifdef CONFIG_AHDB_BE
#include <linux/vhm/acrn_vhm_mm.h>
#endif

#include "ahdb_drv.h"

#ifdef CONFIG_AHDB_BE
/* mapping guest OS's pages underlying HyperDMABUF */
struct shmem_info *ahdb_map(int vmid, long ref, int nents)
{
	struct shmem_info *shmem;
	void *paddr;

	int npgs = REFS_PER_PAGE;
	int last_nents, n_l2refs;
	int i, j = 0, k = 0;

	shmem = kcalloc(1, sizeof(*shmem), GFP_KERNEL);
	if (!shmem)
		return NULL;

	shmem->vmid = vmid;

	last_nents = (nents - 1) % npgs + 1;
	n_l2refs = (nents / npgs) + ((last_nents > 0) ? 1 : 0) -
		   (last_nents == npgs);

	shmem->pages = kcalloc(nents, sizeof(struct page *), GFP_KERNEL);
	if (!shmem->pages)
		goto fail_page_alloc;

	shmem->l2refs = kcalloc(n_l2refs, sizeof(long *), GFP_KERNEL);
	if (!shmem->l2refs)
		goto fail_l2refs;

	shmem->l3refs = (long *)map_guest_phys(vmid, ref, PAGE_SIZE);
	if (!shmem->l3refs)
		goto fail_l3refs;

	for (i = 0; i < n_l2refs; i++) {
		shmem->l2refs[i] = (long *)map_guest_phys(vmid,
							 shmem->l3refs[i],
							 PAGE_SIZE);

		if (!shmem->l2refs[i])
			goto fail_mapping_l2;

		/* last level-2 ref */
		if (i == n_l2refs - 1)
			npgs = last_nents;

		for (j = 0; j < npgs; j++) {
			paddr = map_guest_phys(vmid, shmem->l2refs[i][j],
					       PAGE_SIZE);
			if (!paddr)
				goto fail_mapping_l1;

			shmem->pages[k] = virt_to_page(paddr);
			k++;
		}
		unmap_guest_phys(vmid, shmem->l3refs[i]);
	}

	unmap_guest_phys(vmid, ref);

	return shmem;

fail_mapping_l1:
	for (k = 0; k < j; k++)
		unmap_guest_phys(vmid, shmem->l2refs[i][k]);

fail_mapping_l2:
	for (j = 0; j < i; j++) {
		for (k = 0; k < REFS_PER_PAGE; k++)
			unmap_guest_phys(vmid, shmem->l2refs[i][k]);
	}

	unmap_guest_phys(vmid, shmem->l3refs[i]);
	unmap_guest_phys(vmid, ref);

fail_l3refs:
	kfree(shmem->l2refs);
fail_l2refs:
	kfree(shmem->pages);
fail_page_alloc:
	kfree(shmem);
	return NULL;
}

/* unmapping mapped pages */
int ahdb_unmap(struct shmem_info *shmem)
{
	int last_nents = (shmem->nents - 1) % REFS_PER_PAGE + 1;
	int n_l2refs = (shmem->nents / REFS_PER_PAGE) +
		       ((last_nents > 0) ? 1 : 0) -
		       (last_nents == REFS_PER_PAGE);
	int i, j;

	if (shmem->pages == NULL)
		return 0;

	for (i = 0; i < n_l2refs - 1; i++) {
		for (j = 0; j < REFS_PER_PAGE; j++)
			unmap_guest_phys(shmem->vmid, shmem->l2refs[i][j]);
	}

	for (j = 0; j < last_nents; j++)
		unmap_guest_phys(shmem->vmid, shmem->l2refs[i][j]);

	kfree(shmem->l2refs);
	kfree(shmem->pages);
	kfree(shmem);

	return 0;
}

#else

/* sharing pages for original DMABUF with Service OS */
struct shmem_info *ahdb_share_buf(struct page **pages, int nents)
{
	struct shmem_info *shmem;
	int i;
	int n_l2refs = nents/REFS_PER_PAGE +
		       ((nents % REFS_PER_PAGE) ? 1 : 0);

	shmem = kcalloc(1, sizeof(*shmem), GFP_KERNEL);
	if (!shmem)
		return NULL;

	shmem->pages = pages;
	shmem->nents = nents;
	shmem->l3refs = (long *)__get_free_page(GFP_KERNEL);
	if (!shmem->l3refs) {
		kfree(shmem);
		return NULL;
	}

	shmem->l2refs = (long **)__get_free_pages(GFP_KERNEL,
					get_order(n_l2refs * PAGE_SIZE));

	if (!shmem->l2refs) {
		free_page((long)shmem->l3refs);
		kfree(shmem);
		return NULL;
	}

	/* Share physical address of pages */
	for (i = 0; i < nents; i++)
		shmem->l2refs[i] = (long *)page_to_phys(pages[i]);

	for (i = 0; i < n_l2refs; i++) {
		shmem->l3refs[i] =
			virt_to_phys((void *)shmem->l2refs + i * PAGE_SIZE);
	}

	shmem->ref = (long)virt_to_phys(shmem->l3refs);

	return shmem;
}

/* stop sharing pages */
int ahdb_free_buf(struct shmem_info *shmem)
{
	int n_l2refs = (shmem->nents/REFS_PER_PAGE +
		       ((shmem->nents % REFS_PER_PAGE) ? 1 : 0));

	free_pages((long)shmem->l2refs, get_order(n_l2refs * PAGE_SIZE));
	free_page((long)shmem->l3refs);

	kfree(shmem);
	return 0;
}

#endif
