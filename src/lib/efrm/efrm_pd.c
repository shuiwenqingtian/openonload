/*
** Copyright 2005-2013  Solarflare Communications Inc.
**                      7505 Irvine Center Drive, Irvine, CA 92618, USA
** Copyright 2002-2005  Level 5 Networks Inc.
**
** This program is free software; you can redistribute it and/or modify it
** under the terms of version 2 of the GNU General Public License as
** published by the Free Software Foundation.
**
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
*/

/****************************************************************************
 * Driver for Solarflare network controllers -
 *          resource management for Xen backend, OpenOnload, etc
 *
 * This file provides public API for protection domain resource.
 *
 * Copyright 2011-2011: Solarflare Communications Inc,
 *                      9501 Jeronimo Road, Suite 250,
 *                      Irvine, CA 92618, USA
 *
 * Developed and maintained by Solarflare Communications:
 *                      <linux-xen-drivers@solarflare.com>
 *                      <onload-dev@solarflare.com>
 *
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 ****************************************************************************
 */

#include <ci/efrm/nic_table.h>
#include <ci/driver/efab/hardware.h>
#include <ci/efrm/vi_resource_manager.h>
#include <ci/efrm/private.h>
#include <ci/efrm/pd.h>
#include <ci/efrm/buffer_table.h>
#include <ci/efrm/vf_resource.h>
#include <ci/efrm/vf_resource_private.h>
#include "efrm_internal.h"
#include "efrm_pd.h"


/* NB. Code below requires that N_OWNER_IDS be an exact multiple of
 * N_OWNER_IDS_PER_WORD.
 */
#define N_OWNER_IDS           	 1024
#define N_OWNER_IDS_PER_WORD  	 (sizeof(unsigned long) * 8)
#define N_OWNER_ID_WORDS      	 (N_OWNER_IDS / N_OWNER_IDS_PER_WORD)
#define OWNER_ID_WORD_ALLOCATED  ((unsigned long) -1)

#define OWNER_ID_PHYS_MODE       0
#define OWNER_ID_ALLOC_FAIL      -1


struct efrm_pd {
	struct efrm_resource rs;
	/* [owner_id] is a token used by the NIC to authenticate access to
	 * the buffer table by a VI.  All VIs and memory regions in a
	 * protection domain use the same owner_id.
	 *
	 * If the [owner_id] is negative then the protection domain uses
	 * physical addresses.
	 */
	int owner_id;
#ifdef CONFIG_SFC_RESOURCE_VF
	/* If the protection domain is a VF, then [vf] will be non-null. */
	struct efrm_vf *vf;
#endif
};


struct efrm_pd_manager {
	struct efrm_resource_manager rm;
	/* TODO: ensure this doesn't wrap */
	unsigned next_instance;
	/* TODO: this can and should be per-nic */
	unsigned long used_owner_ids[N_OWNER_ID_WORDS];
};


static struct efrm_pd_manager *pd_manager;


#define efrm_pd(rs1)  container_of((rs1), struct efrm_pd, rs)


static int efrm_pd_owner_id_alloc(void)
{
	/* Must hold pd_manager lock. */
	int i;
	for (i = 0; i < N_OWNER_ID_WORDS; ++i)
		if (pd_manager->used_owner_ids[i] != OWNER_ID_WORD_ALLOCATED) {
			i *= N_OWNER_IDS_PER_WORD;
			while (test_bit(i, pd_manager->used_owner_ids))
				++i;
			__set_bit(i, pd_manager->used_owner_ids);
			return i;
		}
	return OWNER_ID_ALLOC_FAIL;
}


static void efrm_pd_owner_id_free(int owner_id)
{
	/* Must hold pd_manager lock. */
	EFRM_ASSERT(test_bit(owner_id, pd_manager->used_owner_ids));
	__clear_bit(owner_id, pd_manager->used_owner_ids);
}

/***********************************************************************/

int efrm_pd_alloc(struct efrm_pd **pd_out, struct efrm_client *client_opt,
		  struct efrm_vf *vf_opt, int phys_addr_mode)
{
	struct efrm_pd *pd;
	int rc, instance;

	EFRM_ASSERT((client_opt != NULL) || (vf_opt != NULL));

	if ((pd = kmalloc(sizeof(*pd), GFP_KERNEL)) == NULL) {
		rc = -ENOMEM;
		goto fail1;
	}

	spin_lock_bh(&pd_manager->rm.rm_lock);
	instance = pd_manager->next_instance++;
	if (phys_addr_mode)
		pd->owner_id = OWNER_ID_PHYS_MODE;
	else
		pd->owner_id = efrm_pd_owner_id_alloc();
	spin_unlock_bh(&pd_manager->rm.rm_lock);
	if (pd->owner_id == OWNER_ID_ALLOC_FAIL) {
		rc = -EBUSY;
		goto fail2;
	}
#ifdef CONFIG_SFC_RESOURCE_VF
	pd->vf = vf_opt;
	if (pd->vf != NULL) {
		struct efrm_resource *vfrs = efrm_vf_to_resource(pd->vf);
		efrm_resource_ref(vfrs);
		client_opt = vfrs->rs_client;
	}
#endif
	efrm_resource_init(&pd->rs, EFRM_RESOURCE_PD, instance);
	efrm_client_add_resource(client_opt, &pd->rs);
	*pd_out = pd;
	return 0;


fail2:
	kfree(pd);
fail1:
	return rc;
}
EXPORT_SYMBOL(efrm_pd_alloc);


void efrm_pd_release(struct efrm_pd *pd)
{
	if (__efrm_resource_release(&pd->rs))
		efrm_pd_free(pd);
}
EXPORT_SYMBOL(efrm_pd_release);


void efrm_pd_free(struct efrm_pd *pd)
{
	spin_lock_bh(&pd_manager->rm.rm_lock);
	if (pd->owner_id != OWNER_ID_PHYS_MODE)
		efrm_pd_owner_id_free(pd->owner_id);
	spin_unlock_bh(&pd_manager->rm.rm_lock);
#ifdef CONFIG_SFC_RESOURCE_VF
	if (pd->vf != NULL)
		efrm_vf_resource_release(pd->vf);
#endif
	efrm_client_put(pd->rs.rs_client);
	kfree(pd);
}


struct efrm_resource * efrm_pd_to_resource(struct efrm_pd *pd)
{
	return &pd->rs;
}
EXPORT_SYMBOL(efrm_pd_to_resource);


struct efrm_pd * efrm_pd_from_resource(struct efrm_resource *rs)
{
	return efrm_pd(rs);
}
EXPORT_SYMBOL(efrm_pd_from_resource);


int efrm_pd_owner_id(struct efrm_pd *pd)
{
	return pd->owner_id;
}
EXPORT_SYMBOL(efrm_pd_owner_id);


struct efrm_vf *efrm_pd_get_vf(struct efrm_pd *pd)
{
#ifdef CONFIG_SFC_RESOURCE_VF
	return pd->vf;
#else
	return NULL;
#endif
}


struct pci_dev *efrm_pd_get_pci_dev(struct efrm_pd *pd)
{
#ifdef CONFIG_SFC_RESOURCE_VF
	if (pd->vf)
		return pd->vf->pci_dev;
	else
#endif
		return pd->rs.rs_client->nic->pci_dev;
}


int efrm_pd_share_dma_mapping(struct efrm_pd *pd, struct efrm_pd *pd1)
{
#ifndef CONFIG_SFC_RESOURCE_VF_IOMMU
	return false;
#else
	efhw_iommu_domain *dom, *dom1;

	if (pd->owner_id != OWNER_ID_PHYS_MODE ||
	    pd1->owner_id != OWNER_ID_PHYS_MODE)
		return false;

	if (pd->vf == NULL || pd1->vf == NULL)
		return false;

	dom = efrm_vf_get_iommu_domain(pd->vf);
	dom1 = efrm_vf_get_iommu_domain(pd1->vf);
	if (dom == NULL || dom1 == NULL)
		return false;
	if (dom != dom1)
		return false;
	return true;
#endif
}
EXPORT_SYMBOL(efrm_pd_share_dma_mapping);


/**********************************************************************/


static void efrm_pd_dma_unmap_pci(struct pci_dev *pci_dev,
				  int n_pages, int gfp_order,
				  dma_addr_t *pci_addrs, int pci_addrs_stride)
{
	while (--n_pages >= 0) {
		dma_unmap_page(&pci_dev->dev, *pci_addrs,
			       PAGE_SIZE << gfp_order,
			       DMA_BIDIRECTIONAL);
		pci_addrs = (void *)((char *)pci_addrs + pci_addrs_stride);
	}
}


static int efrm_pd_dma_map_pci(struct pci_dev *pci_dev,
			       int n_pages, int gfp_order,
			       struct page **pages, int pages_stride,
			       dma_addr_t *pci_addrs, int pci_addrs_stride)
{
	int i;
	for (i = 0; i < n_pages; ++i) {
		*pci_addrs = dma_map_page(&pci_dev->dev, *pages, 0,
					  PAGE_SIZE << gfp_order,
					  DMA_BIDIRECTIONAL);
		if (dma_mapping_error(&pci_dev->dev, *pci_addrs)) {
			EFRM_ERR("%s: ERROR: dma_map_page failed",
				 __FUNCTION__);
			goto fail;
		}
		pages = (void *)((char *)pages + pages_stride);
		pci_addrs = (void *)((char *)pci_addrs + pci_addrs_stride);
	}
	return 0;

fail:
	pci_addrs = (void *)((char *)pci_addrs - i * pci_addrs_stride);
	pages = (void *)((char *)pages - i * pages_stride);
	efrm_pd_dma_unmap_pci(pci_dev, i, gfp_order,
			      pci_addrs, pci_addrs_stride);
	return -ENOMEM;
}


#ifdef CONFIG_SFC_RESOURCE_VF
#ifdef CONFIG_SFC_RESOURCE_VF_IOMMU
static void efrm_pd_dma_unmap_iommu(efhw_iommu_domain *iommu_domain,
				    int n_pages, int gfp_order,
				    dma_addr_t *pci_addrs,
				    int pci_addrs_stride)
{
	while (--n_pages >= 0) {
		mutex_lock(&efrm_iommu_mutex);
		iommu_unmap(iommu_domain, *pci_addrs, gfp_order);
		mutex_unlock(&efrm_iommu_mutex);
		pci_addrs = (void *)((char *)pci_addrs + pci_addrs_stride);
	}
}


static int efrm_pd_dma_map_iommu(efhw_iommu_domain *iommu_domain,
				 unsigned long iovaddr,
				 int n_pages, int gfp_order,
				 struct page **pages, int pages_stride,
				 dma_addr_t *pci_addrs, int pci_addrs_stride)
{
	/* TODO: Is there any benefit to mapping larger chunks? */

	int i, rc;

	for (i = 0; i < n_pages; ++i) {
#ifdef HPAGE_SHIFT
		/* Yes, this is just crazy, but iommu_map does not
		 * correctly map 2M (huge|compound) pages on SLES11
		 * linux-3.0 Intel IOMMU. */
		if (gfp_order == HPAGE_SHIFT - PAGE_SHIFT) {
			unsigned long shift = 1 << (HPAGE_SHIFT - 1);
			rc = iommu_map(iommu_domain, iovaddr,
				       page_to_phys(*pages), gfp_order - 1,
				       IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
			if (rc >= 0)
				rc = iommu_map(iommu_domain, iovaddr + shift,
					       page_to_phys(*pages) + shift,
					       gfp_order - 1,
					       IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
		}
		else
#endif
		rc = iommu_map(iommu_domain, iovaddr,
			       page_to_phys(*pages), gfp_order,
			       IOMMU_READ | IOMMU_WRITE | IOMMU_CACHE);
		if (rc < 0) {
			EFRM_ERR("%s: ERROR: iommu_map failed (%d)",
				 __FUNCTION__, rc);
			goto fail;
		}
		*pci_addrs = iovaddr;
		iovaddr += PAGE_SIZE << gfp_order;
		pages = (void *)((char *)pages + pages_stride);
		pci_addrs = (void *)((char *)pci_addrs + pci_addrs_stride);
	}
	return 0;

fail:
	pci_addrs = (void *)((char *)pci_addrs - i * pci_addrs_stride);
	pages = (void *)((char *)pages - i * pages_stride);
	efrm_pd_dma_unmap_iommu(iommu_domain, i, gfp_order,
				pci_addrs, pci_addrs_stride);
	return rc;
}
#endif

static void efrm_pd_dma_unmap_vf(struct efrm_pd *pd,
				 int n_pages, int gfp_order,
				 dma_addr_t *pci_addrs, int pci_addrs_stride)
{
	efhw_iommu_domain *iommu_domain;
	efrm_vf_alloc_ioaddrs(pd->vf, 0, &iommu_domain);

#ifdef CONFIG_SFC_RESOURCE_VF_IOMMU
	if (iommu_domain != NULL)
		efrm_pd_dma_unmap_iommu(iommu_domain, n_pages, gfp_order,
				        pci_addrs, pci_addrs_stride);
	else
#endif
		efrm_pd_dma_unmap_pci(pd->vf->pci_dev, n_pages, gfp_order,
				      pci_addrs, pci_addrs_stride);
}


static int efrm_pd_dma_map_vf(struct efrm_pd *pd, int n_pages, int gfp_order,
			      struct page **pages, int pages_stride,
			      dma_addr_t *pci_addrs, int pci_addrs_stride)
{
	efhw_iommu_domain *iommu_domain;
	unsigned long iovaddr;
	iovaddr = efrm_vf_alloc_ioaddrs(pd->vf, n_pages * (1 << gfp_order),
					&iommu_domain);

#ifdef CONFIG_SFC_RESOURCE_VF_IOMMU
	if (iommu_domain != NULL)
		return efrm_pd_dma_map_iommu(iommu_domain, iovaddr,
					     n_pages, gfp_order,
					     pages, pages_stride,
					     pci_addrs, pci_addrs_stride);
	else
#endif
		return efrm_pd_dma_map_pci(pd->vf->pci_dev,
					   n_pages, gfp_order,
					   pages, pages_stride,
					   pci_addrs, pci_addrs_stride);
}
#endif


static void efrm_pd_dma_unmap_nic(struct efrm_pd *pd,
				  int n_pages, int gfp_order,
				  dma_addr_t *pci_addrs, int pci_addrs_stride)
{
	efrm_pd_dma_unmap_pci(efrm_client_get_nic(pd->rs.rs_client)->pci_dev,
			      n_pages, gfp_order,
			      pci_addrs, pci_addrs_stride);
}


static int efrm_pd_dma_map_nic(struct efrm_pd *pd,
			       int n_pages, int gfp_order,
			       struct page **pages, int pages_stride,
			       dma_addr_t *pci_addrs, int pci_addrs_stride)
{
	return efrm_pd_dma_map_pci(
			efrm_client_get_nic(pd->rs.rs_client)->pci_dev,
			n_pages, gfp_order, pages, pages_stride,
			pci_addrs, pci_addrs_stride);
}


static void efrm_pd_dma_unmap_bt(struct efrm_pd *pd, int n_pages,
				 dma_addr_t *pci_addrs, int pci_addrs_stride,
				 struct efhw_buffer_table_allocation *bt_alloc)
{
	struct efhw_nic *nic = efrm_client_get_nic(pd->rs.rs_client);
	efrm_nic_buffer_table_free(efrm_nic(nic), bt_alloc);
}


static int efrm_pd_dma_map_bt(struct efrm_pd *pd, int n_pages, int gfp_order,
			      struct page **pages, int pages_stride,
			      dma_addr_t *pci_addrs, int pci_addrs_stride,
			      uint64_t *user_addrs, int user_addrs_stride,
			      void (*user_addr_put)(uint64_t, uint64_t *),
			      struct efhw_buffer_table_allocation *bt_alloc)
{
	struct efhw_nic *nic = efrm_client_get_nic(pd->rs.rs_client);
	uint64_t user_addr;
	int i, rc;

	rc = efrm_nic_buffer_table_alloc(efrm_nic(nic),
			fls(n_pages * (1 << gfp_order) - 1),
			bt_alloc);
	if (rc < 0) {
		EFRM_ERR("%s: ERROR: out of buffer table (%d pages)",
			 __FUNCTION__, n_pages);
		return rc;
	}
	user_addr = bt_alloc->base << 12u;
	for (i = 0; i < n_pages; ++i) {
		efrm_buffer_table_set_n(bt_alloc, nic, 1 << gfp_order,
				        i << gfp_order, *pci_addrs, pd->owner_id);
		pci_addrs = (void *)((char *)pci_addrs + pci_addrs_stride);
		pages = (void *)((char *)pages + pages_stride);
		user_addr_put(user_addr, user_addrs);
		user_addrs = (void *)((char *)user_addrs + user_addrs_stride);
		user_addr += 4096 << gfp_order;
	}
	efrm_buffer_table_commit(nic);
	return 0;
}


static void efrm_pd_copy_user_addrs(struct efrm_pd *pd, int n_pages,
			    dma_addr_t *pci_addrs, int pci_addrs_stride,
			    uint64_t *user_addrs, int user_addrs_stride,
			    void (*user_addr_put)(uint64_t, uint64_t *))
{
	int i;
	for (i = 0; i < n_pages; ++i) {
		user_addr_put(*pci_addrs, user_addrs);
		pci_addrs = (void *)((char *)pci_addrs + pci_addrs_stride);
		user_addrs = (void *)((char *)user_addrs + user_addrs_stride);
	}
}


int efrm_pd_dma_remap_bt(struct efrm_pd *pd, int n_pages, int gfp_order,
			 dma_addr_t *pci_addrs, int pci_addrs_stride,
			 struct efhw_buffer_table_allocation *bt_alloc)
{
	struct efhw_nic *nic = efrm_client_get_nic(pd->rs.rs_client);
	int i;

	if (pd->owner_id == OWNER_ID_PHYS_MODE) 
		return 0;

	for (i = 0; i < n_pages; ++i) {
		efrm_buffer_table_set_n(bt_alloc, nic, 1 << gfp_order,
					i << gfp_order, *pci_addrs, pd->owner_id);
		pci_addrs = (void *)((char *)pci_addrs + pci_addrs_stride);
	}
	efrm_buffer_table_commit(nic);

	return 0;
}
EXPORT_SYMBOL(efrm_pd_dma_remap_bt);


int efrm_pd_dma_map(struct efrm_pd *pd, int n_pages, int gfp_order,
		    struct page **pages, int pages_stride,
		    void *p_pci_addrs, int pci_addrs_stride,
		    uint64_t *user_addrs, int user_addrs_stride,
		    void (*user_addr_put)(uint64_t, uint64_t *),
		    struct efhw_buffer_table_allocation *bt_alloc)
{
	dma_addr_t *pci_addrs = p_pci_addrs;
	int rc;

#ifdef CONFIG_SFC_RESOURCE_VF
	if (pd->vf != NULL)
		rc = efrm_pd_dma_map_vf(pd, n_pages, gfp_order,
					pages, pages_stride,
					pci_addrs, pci_addrs_stride);
	else
#endif
		rc = efrm_pd_dma_map_nic(pd, n_pages, gfp_order,
					 pages, pages_stride,
					 pci_addrs, pci_addrs_stride);
	if (rc < 0)
		goto fail1;

	if (pd->owner_id != OWNER_ID_PHYS_MODE) {
		rc = efrm_pd_dma_map_bt(pd, n_pages, gfp_order,
					pages, pages_stride,
					pci_addrs, pci_addrs_stride,
					user_addrs, user_addrs_stride,
					user_addr_put, bt_alloc);
		if (rc < 0)
			goto fail2;
	} else {
		efrm_pd_copy_user_addrs(pd, n_pages,
					pci_addrs, pci_addrs_stride,
					user_addrs, user_addrs_stride,
					user_addr_put);
	}
	return 0;


fail2:
#ifdef CONFIG_SFC_RESOURCE_VF
	if (pd->vf != NULL)
		efrm_pd_dma_unmap_vf(pd, n_pages, gfp_order,
				     pci_addrs, pci_addrs_stride);
	else
#endif
		efrm_pd_dma_unmap_nic(pd, n_pages, gfp_order,
				      pci_addrs, pci_addrs_stride);
fail1:
	return rc;
}
EXPORT_SYMBOL(efrm_pd_dma_map);


void efrm_pd_dma_unmap(struct efrm_pd *pd, int n_pages, int gfp_order,
		       void *p_pci_addrs, int pci_addrs_stride,
		       struct efhw_buffer_table_allocation *bt_alloc)
{
	dma_addr_t *pci_addrs = p_pci_addrs;
	if (pd->owner_id != OWNER_ID_PHYS_MODE)
		efrm_pd_dma_unmap_bt(pd, n_pages, pci_addrs, pci_addrs_stride,
				     bt_alloc);
#ifdef CONFIG_SFC_RESOURCE_VF
	if (pd->vf != NULL)
		efrm_pd_dma_unmap_vf(pd, n_pages, gfp_order,
				     pci_addrs, pci_addrs_stride);
	else
#endif
		efrm_pd_dma_unmap_nic(pd, n_pages, gfp_order,
				      pci_addrs, pci_addrs_stride);
}
EXPORT_SYMBOL(efrm_pd_dma_unmap);

/**********************************************************************/

static void efrm_pd_rm_dtor(struct efrm_resource_manager *rm)
{
}


int
efrm_create_pd_resource_manager(struct efrm_resource_manager **rm_out)
{
	struct efrm_pd_manager *rm;
	int rc;

	rm = kmalloc(sizeof(*rm), GFP_KERNEL);
	if (rm == NULL)
		return -ENOMEM;
	memset(rm, 0, sizeof(*rm));

	/* Owner-ID 0 is special.  We must not allocate it to users. */
	__set_bit(0, rm->used_owner_ids);

	rc = efrm_resource_manager_ctor(&rm->rm, efrm_pd_rm_dtor,
					"PD", EFRM_RESOURCE_PD);
	if (rc < 0)
		goto fail1;

	pd_manager = rm;
	*rm_out = &rm->rm;
	return 0;

fail1:
	kfree(rm);
	return rc;
}
