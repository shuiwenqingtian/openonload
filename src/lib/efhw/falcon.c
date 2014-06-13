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
 *           (including support for SFE4001 10GBT NIC)
 *
 * This file contains Falcon hardware support.
 *
 * Copyright 2005-2008: Solarflare Communications Inc,
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

#include <ci/driver/efab/hardware.h>
#include <ci/efhw/debug.h>
#include <ci/efhw/iopage.h>
#include <ci/efhw/falcon.h>
#include <ci/efhw/nic.h>
#include <ci/efhw/eventq.h>
#include <ci/efhw/checks.h>


/*----------------------------------------------------------------------------
 *
 * Workarounds and options
 *
 *---------------------------------------------------------------------------*/

/* Keep a software copy of the filter table and check for duplicates. */
#define FALCON_FULL_FILTER_CACHE 1

/* The maximum number of time to read to see that a buffer table entry is set */
/*
 * This is defined to investigate occasional failed assertion in snapper tests
 * TODO: review and remove or do properly
 */
#define BUG_14512_WA

#define MAX_BUF_TBL_READS 128
#ifdef BUG_14512_WA
#define MAX_MAX_BUF_TBL_READS 16*MAX_BUF_TBL_READS
#endif

/* "Fudge factors" - difference between programmed value and actual depth.
 * Due to pipelined implementation we need to program H/W with a value that
 * is larger than the hop limit we want.
 */
#define RX_FILTER_CTL_SRCH_FUDGE_WILD 3	/* increase the search limit */
#define RX_FILTER_CTL_SRCH_FUDGE_FULL 1	/* increase the search limit */
#define TX_FILTER_CTL_SRCH_FUDGE_WILD 3	/* increase the search limit */
#define TX_FILTER_CTL_SRCH_FUDGE_FULL 1	/* increase the search limit */

/*----------------------------------------------------------------------------
 *
 * Debug Macros
 *
 *---------------------------------------------------------------------------*/

#ifndef __KERNEL__
#define _DEBUG_SYM_
#else
#define _DEBUG_SYM_ static
#endif

 /*----------------------------------------------------------------------------
  *
  * Macros and forward declarations
  *
  *--------------------------------------------------------------------------*/

#define Q0_READ(q0, name) \
	((unsigned)(((q0) >> name##_LBN) & (__FALCON_MASK64(name##_WIDTH))))
#define Q0_MASK(name) \
	((__FALCON_MASK64(name##_WIDTH)) << name##_LBN)
#define Q0_VALUE(name, value) \
	(((uint64_t)(value)) << name##_LBN)

#define Q1_READ(q1, name) \
	((unsigned)(((q1) >> (name##_LBN - 64)) & \
		    (__FALCON_MASK64(name##_WIDTH))))
#define Q1_MASK(name) \
	((__FALCON_MASK64(name##_WIDTH)) << (name##_LBN - 64))
#define Q1_VALUE(name, value) \
	(((uint64_t)(value)) << (name##_LBN - 64))

#define FALCON_REGION_NUM 4	/* number of supported memory regions */

#define FALCON_BUFFER_TBL_HALF_BYTES 4
#define FALCON_BUFFER_TBL_FULL_BYTES 8


/*----------------------------------------------------------------------------
 *
 * Header assertion checks
 *
 *---------------------------------------------------------------------------*/

#define FALCON_ASSERT_VALID()	/* nothing yet */

/* Falcon has a 128bit register model but most registers have useful
   defaults or only implement a small number of bits. Some registers
   can be programmed 32bits UNLOCKED all others should be interlocked
   against other threads within the same protection domain.

   Aim is for software to perform the minimum number of writes and
   also to minimise the read-modify-write activity (which generally
   indicates a lack of clarity in the use model).

   Registers which are programmed in this module are listed below
   together with the method of access. Care must be taken to ensure
   remain adequate if the register spec changes.

   All 128bits programmed
    FALCON_BUFFER_TBL_HALF
    RX_FILTER_TBL
    TX_DESC_PTR_TBL
    RX_DESC_PTR_TBL
    DRV_EV_REG

   All 64bits programmed
    FALCON_BUFFER_TBL_FULL

   32 bits are programmed (UNLOCKED)
    EVQ_RPTR_REG

   Low 64bits programmed remainder are written with a random number
    RX_DC_CFG_REG
    TX_DC_CFG_REG
    SRM_RX_DC_CFG_REG
    SRM_TX_DC_CFG_REG
    BUF_TBL_CFG_REG
    BUF_TBL_UPD_REG
    SRM_UPD_EVQ_REG
    EVQ_PTR_TBL
    TIMER_CMD_REG
    TX_PACE_TBL
    FATAL_INTR_REG
    INT_EN_REG (When enabling interrupts)
    TX_FLUSH_DESCQ_REG
    RX_FLUSH_DESCQ

  Read Modify Write on low 32bits remainder are written with a random number
    INT_EN_REG (When sending a driver interrupt)
    DRIVER_REGX

  Read Modify Write on low 64bits remainder are written with a random number
   SRM_CFG_REG_OFST
   RX_CFG_REG_OFST
   RX_FILTER_CTL_REG

  Read Modify Write on full 128bits
   TXDP_RESERVED_REG  (aka TXDP_UNDOCUMENTED)
   TX_CFG_REG

*/


/*----------------------------------------------------------------------------
 *
 * DMAQ low-level register interface
 *
 *---------------------------------------------------------------------------*/

static unsigned dmaq_sizes[] = {
	512,
	EFHW_1K,
	EFHW_2K,
	EFHW_4K,
};

#define N_DMAQ_SIZES  (sizeof(dmaq_sizes) / sizeof(dmaq_sizes[0]))

static inline ulong falcon_dma_tx_q_offset(struct efhw_nic *nic, unsigned dmaq)
{
	EFHW_ASSERT(dmaq < nic->num_dmaqs);
	return FR_AZ_TX_DESC_PTR_TBL_OFST + dmaq * FALCON_REGISTER128;
}

static inline uint falcon_dma_tx_q_size_index(uint dmaq_size)
{
	uint i;

	/* size must be one of the various options, otherwise we assert */
	for (i = 0; i < N_DMAQ_SIZES; i++) {
		if (dmaq_size == dmaq_sizes[i])
			break;
	}
	EFHW_ASSERT(i < N_DMAQ_SIZES);
	return i;
}

static void
falcon_dmaq_tx_q_init(struct efhw_nic *nic,
		      uint dmaq, uint evq_id, uint own_id,
		      uint tag, uint dmaq_size, uint buf_idx, uint flags)
{
	FALCON_LOCK_DECL;
	uint index, desc_type;
	uint64_t val1, val2, val3;
	ulong offset;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	/* Q attributes */
	int iscsi_hdig_en = ((flags & EFHW_VI_ISCSI_TX_HDIG_EN) != 0);
	int iscsi_ddig_en = ((flags & EFHW_VI_ISCSI_TX_DDIG_EN) != 0);
	int csum_ip_dis = ((flags & EFHW_VI_TX_IP_CSUM_DIS) != 0);
	int csum_tcp_dis = ((flags & EFHW_VI_TX_TCPUDP_CSUM_DIS) != 0);
	int non_ip_drop_dis = ((flags & EFHW_VI_TX_TCPUDP_ONLY) == 0);
	int tx_ip_filter_en = ((flags & EFHW_VI_TX_IP_FILTER_EN) != 0);
	int tx_eth_filter_en = ((flags & EFHW_VI_TX_ETH_FILTER_EN) != 0);
	int q_mask_width = ((flags & EFHW_VI_TX_Q_MASK_WIDTH_0) != 0) |
			   (((flags & EFHW_VI_TX_Q_MASK_WIDTH_1) != 0) << 1);

	/* initialise the TX descriptor queue pointer table */

	/* NB physical vs buffer addressing is determined by the Queue ID. */

	offset = falcon_dma_tx_q_offset(nic, dmaq);
	index = falcon_dma_tx_q_size_index(dmaq_size);

	/* allow VI flag to override this queue's descriptor type */
	desc_type = (flags & EFHW_VI_TX_PHYS_ADDR_EN) ? 0 : 1;

	/* bug9403: It is dangerous to allow buffer-addressed queues to
	 * have owner_id=0. */
	EFHW_ASSERT((own_id > 0) || desc_type == 0);

	/* dword 1 */
	__DWCHCK(FRF_AZ_TX_DESCQ_FLUSH);
	__DWCHCK(FRF_AZ_TX_DESCQ_TYPE);
	__DWCHCK(FRF_AZ_TX_DESCQ_SIZE);
	__DWCHCK(FRF_AZ_TX_DESCQ_LABEL);
	__DWCHCK(FRF_AZ_TX_DESCQ_OWNER_ID);

	__LWCHK(FRF_AZ_TX_DESCQ_EVQ_ID);

	__RANGECHCK(1, FRF_AZ_TX_DESCQ_FLUSH_WIDTH);
	__RANGECHCK(desc_type, FRF_AZ_TX_DESCQ_TYPE_WIDTH);
	__RANGECHCK(index, FRF_AZ_TX_DESCQ_SIZE_WIDTH);
	__RANGECHCK(tag, FRF_AZ_TX_DESCQ_LABEL_WIDTH);
	__RANGECHCK(own_id, FRF_AZ_TX_DESCQ_OWNER_ID_WIDTH);
	__RANGECHCK(evq_id, FRF_AZ_TX_DESCQ_EVQ_ID_WIDTH);

	val1 = ((desc_type << FRF_AZ_TX_DESCQ_TYPE_LBN) |
		(index << FRF_AZ_TX_DESCQ_SIZE_LBN) |
		(tag << FRF_AZ_TX_DESCQ_LABEL_LBN) |
		(own_id << FRF_AZ_TX_DESCQ_OWNER_ID_LBN) |
		(__LOW(evq_id, FRF_AZ_TX_DESCQ_EVQ_ID)));

	/* dword 2 */
	__DW2CHCK(FRF_AZ_TX_DESCQ_BUF_BASE_ID);
	__RANGECHCK(buf_idx, FRF_AZ_TX_DESCQ_BUF_BASE_ID_WIDTH);

	val2 = ((__HIGH(evq_id, FRF_AZ_TX_DESCQ_EVQ_ID)) |
		(buf_idx << __DW2(FRF_AZ_TX_DESCQ_BUF_BASE_ID_LBN)));

	/* dword 3 */
	__DW3CHCK(FRF_AZ_TX_ISCSI_HDIG_EN);
	__DW3CHCK(FRF_AZ_TX_ISCSI_DDIG_EN);
	__DW3CHCK(FRF_BZ_TX_IP_CHKSM_DIS);
	__DW3CHCK(FRF_BZ_TX_NON_IP_DROP_DIS);
	__DW3CHCK(FRF_BZ_TX_TCP_CHKSM_DIS);
	__RANGECHCK(iscsi_hdig_en, FRF_AZ_TX_ISCSI_HDIG_EN_WIDTH);
	__RANGECHCK(iscsi_ddig_en, FRF_AZ_TX_ISCSI_DDIG_EN_WIDTH);

	val3 = ((iscsi_hdig_en << __DW3(FRF_AZ_TX_ISCSI_HDIG_EN_LBN)) |
		(iscsi_ddig_en << __DW3(FRF_AZ_TX_ISCSI_DDIG_EN_LBN)) |
		(1 << __DW3(FRF_AZ_TX_DESCQ_EN_LBN)));	/* queue enable bit */

	/* Cummulative features - check nothing is invalid */
	switch (nic->devtype.variant) {
	case 'A':
		if (csum_ip_dis || csum_tcp_dis || !non_ip_drop_dis)
			EFHW_WARN
				("%s: bad settings for A1 csum_ip_dis=%d "
				 "csum_tcp_dis=%d non_ip_drop_dis=%d",
				 __FUNCTION__, csum_ip_dis,
				 csum_tcp_dis, non_ip_drop_dis);
		/* fall-through */
	case 'B':
		if (tx_ip_filter_en || tx_eth_filter_en || q_mask_width) {
			EFHW_WARN
				("%s: bad settings for B0 tx_ip_filter_en=%d "
				 "tx_eth_filter_en=%d q_mask_width=%d",
				 __FUNCTION__, tx_ip_filter_en,
				 tx_eth_filter_en, q_mask_width);
			tx_ip_filter_en = 0;
			tx_eth_filter_en = 0;
			q_mask_width = 0;
		}
		break;
        case 'C':
                break;
	default:
                EFHW_WARN("%s: unknown NIC variant '\\x%02x'",
                          __FUNCTION__, nic->devtype.variant);
		EFHW_ASSERT(0);
		break;
	}

	val3 |= ((non_ip_drop_dis  << __DW3(FRF_BZ_TX_NON_IP_DROP_DIS_LBN)) |
		 (csum_ip_dis      << __DW3(FRF_BZ_TX_IP_CHKSM_DIS_LBN))    |
		 (csum_tcp_dis     << __DW3(FRF_BZ_TX_TCP_CHKSM_DIS_LBN))   |
		 (tx_ip_filter_en  << __DW3(FRF_CZ_TX_DPT_IP_FILT_EN_LBN))  |
		 (tx_eth_filter_en << __DW3(FRF_CZ_TX_DPT_ETH_FILT_EN_LBN)) |
		 (q_mask_width     << __DW3(FRF_CZ_TX_DPT_Q_MASK_WIDTH_LBN)));

	EFHW_TRACE("%s: txq %x evq %u tag %x id %x buf %x "
		   "%x:%x:%x->%" PRIx64 ":%" PRIx64 ":%" PRIx64,
		   __FUNCTION__,
		   dmaq, evq_id, tag, own_id, buf_idx, dmaq_size,
		   iscsi_hdig_en, iscsi_ddig_en, val1, val2, val3);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, ((val2 << 32) | val1), val3);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return;
}

static inline ulong
falcon_dma_rx_q_offset(struct efhw_nic *nic, unsigned dmaq)
{
	EFHW_ASSERT(dmaq < nic->num_dmaqs);
	return FR_AZ_RX_DESC_PTR_TBL_OFST + dmaq * FALCON_REGISTER128;
}

static void
falcon_dmaq_rx_q_init(struct efhw_nic *nic,
		      uint dmaq, uint evq_id, uint own_id,
		      uint tag, uint dmaq_size, uint buf_idx, uint flags)
{
	FALCON_LOCK_DECL;
	uint i, desc_type = 1;
	uint64_t val1, val2, val3;
	ulong offset;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	/* Q attributes */
	int jumbo = ((flags & EFHW_VI_JUMBO_EN) != 0);
	int iscsi_hdig_en = ((flags & EFHW_VI_ISCSI_RX_HDIG_EN) != 0);
	int iscsi_ddig_en = ((flags & EFHW_VI_ISCSI_RX_DDIG_EN) != 0);
	int hdr_split_en = ((flags & EFHW_VI_RX_HDR_SPLIT) != 0);

	/* initialise the TX descriptor queue pointer table */
	offset = falcon_dma_rx_q_offset(nic, dmaq);

	/* size must be one of the various options, otherwise we assert */
	for (i = 0; i < N_DMAQ_SIZES; i++) {
		if (dmaq_size == dmaq_sizes[i])
			break;
	}
	EFHW_ASSERT(i < N_DMAQ_SIZES);

	/* allow VI flag to override this queue's descriptor type */
	desc_type = (flags & EFHW_VI_RX_PHYS_ADDR_EN) ? 0 : 1;

	/* bug9403: It is dangerous to allow buffer-addressed queues to have
	 * owner_id=0 */
	EFHW_ASSERT((own_id > 0) || desc_type == 0);

	/* dword 1 */
	__DWCHCK(FRF_AZ_RX_DESCQ_EN);
	__DWCHCK(FRF_AZ_RX_DESCQ_JUMBO);
	__DWCHCK(FRF_AZ_RX_DESCQ_TYPE);
	__DWCHCK(FRF_AZ_RX_DESCQ_SIZE);
	__DWCHCK(FRF_AZ_RX_DESCQ_LABEL);
	__DWCHCK(FRF_AZ_RX_DESCQ_OWNER_ID);

	__LWCHK(FRF_AZ_RX_DESCQ_EVQ_ID);

	__RANGECHCK(1, FRF_AZ_RX_DESCQ_EN_WIDTH);
	__RANGECHCK(jumbo, FRF_AZ_RX_DESCQ_JUMBO_WIDTH);
	__RANGECHCK(desc_type, FRF_AZ_RX_DESCQ_TYPE_WIDTH);
	__RANGECHCK(i, FRF_AZ_RX_DESCQ_SIZE_WIDTH);
	__RANGECHCK(tag, FRF_AZ_RX_DESCQ_LABEL_WIDTH);
	__RANGECHCK(own_id, FRF_AZ_RX_DESCQ_OWNER_ID_WIDTH);
	__RANGECHCK(evq_id, FRF_AZ_RX_DESCQ_EVQ_ID_WIDTH);

	val1 = ((1 << FRF_AZ_RX_DESCQ_EN_LBN) |
		(jumbo << FRF_AZ_RX_DESCQ_JUMBO_LBN) |
		(desc_type << FRF_AZ_RX_DESCQ_TYPE_LBN) |
		(i << FRF_AZ_RX_DESCQ_SIZE_LBN) |
		(tag << FRF_AZ_RX_DESCQ_LABEL_LBN) |
		(own_id << FRF_AZ_RX_DESCQ_OWNER_ID_LBN) |
		(__LOW(evq_id, FRF_AZ_RX_DESCQ_EVQ_ID)));

	/* dword 2 */
	__DW2CHCK(FRF_AZ_RX_DESCQ_BUF_BASE_ID);
	__RANGECHCK(buf_idx, FRF_AZ_RX_DESCQ_BUF_BASE_ID_WIDTH);

	val2 = ((__HIGH(evq_id, FRF_AZ_RX_DESCQ_EVQ_ID)) |
		(buf_idx << __DW2(FRF_AZ_RX_DESCQ_BUF_BASE_ID_LBN)));

	/* dword 3 */
	__DW3CHCK(FRF_AZ_RX_ISCSI_HDIG_EN);
	__DW3CHCK(FRF_AZ_RX_ISCSI_DDIG_EN);
	__DW3CHCK(FRF_CZ_RX_HDR_SPLIT);

	val3 = (iscsi_hdig_en << __DW3(FRF_AZ_RX_ISCSI_HDIG_EN_LBN)) |
	    (iscsi_ddig_en << __DW3(FRF_AZ_RX_ISCSI_DDIG_EN_LBN)) |
	    (hdr_split_en << __DW3(FRF_CZ_RX_HDR_SPLIT_LBN));

	EFHW_TRACE("%s: rxq %x evq %u tag %x id %x buf %x %s "
		   "%x:%x:%x:%x -> %" PRIx64 ":%" PRIx64 ":%" PRIx64,
		   __FUNCTION__,
		   dmaq, evq_id, tag, own_id, buf_idx,
		   jumbo ? "jumbo" : "normal", dmaq_size,
		   iscsi_hdig_en, iscsi_ddig_en, hdr_split_en,
		   val1, val2, val3);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, ((val2 << 32) | val1), val3);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return;
}

static void falcon_dmaq_tx_q_disable(struct efhw_nic *nic, uint dmaq)
{
	FALCON_LOCK_DECL;
	uint64_t val1, val2, val3;
	ulong offset;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	/* initialise the TX descriptor queue pointer table */

	offset = falcon_dma_tx_q_offset(nic, dmaq);

	/* dword 1 */
	__DWCHCK(FRF_AZ_TX_DESCQ_TYPE);

	val1 = ((uint64_t) 1 << FRF_AZ_TX_DESCQ_TYPE_LBN);

	/* dword 2 */
	val2 = 0;

	/* dword 3 */
	val3 = (0 << __DW3(FRF_AZ_TX_DESCQ_EN_LBN));	/* queue enable bit */

	EFHW_TRACE("%s: %x->%" PRIx64 ":%" PRIx64 ":%" PRIx64,
		   __FUNCTION__, dmaq, val1, val2, val3);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, ((val2 << 32) | val1), val3);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return;
}

static void falcon_dmaq_rx_q_disable(struct efhw_nic *nic, uint dmaq)
{
	FALCON_LOCK_DECL;
	uint64_t val1, val2, val3;
	ulong offset;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	/* initialise the TX descriptor queue pointer table */
	offset = falcon_dma_rx_q_offset(nic, dmaq);

	/* dword 1 */
	__DWCHCK(FRF_AZ_RX_DESCQ_EN);
	__DWCHCK(FRF_AZ_RX_DESCQ_TYPE);

	val1 = ((0 << FRF_AZ_RX_DESCQ_EN_LBN) | (1 << FRF_AZ_RX_DESCQ_TYPE_LBN));

	/* dword 2 */
	val2 = 0;

	/* dword 3 */
	val3 = 0;

	EFHW_TRACE("falcon_dmaq_rx_q_disable: %x->%"
		   PRIx64 ":%" PRIx64 ":%" PRIx64,
		   dmaq, val1, val2, val3);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, ((val2 << 32) | val1), val3);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return;
}


/*----------------------------------------------------------------------------
 *
 * Buffer Table low-level register interface
 *
 *---------------------------------------------------------------------------*/

/*! Convert a (potentially) 64-bit physical address to 32-bits.  Every use
** of this function is a place where we're not 64-bit clean.
*/
static inline uint32_t dma_addr_to_u32(dma_addr_t addr)
{
	/* Top bits had better be zero! */
	EFHW_ASSERT(addr == (addr & 0xffffffff));
	return (uint32_t) addr;
}

static inline uint32_t
falcon_nic_buffer_table_entry32_mk(dma_addr_t dma_addr, int own_id)
{
	uint32_t dma_addr32 = FALCON_BUFFER_4K_PAGE(dma_addr_to_u32(dma_addr));

	/* don't do this to me */
	EFHW_BUILD_ASSERT(FRF_AZ_BUF_ADR_HBUF_ODD_LBN == 
			  FRF_AZ_BUF_ADR_HBUF_EVEN_LBN + 32);
	EFHW_BUILD_ASSERT(FRF_AZ_BUF_OWNER_ID_HBUF_ODD_LBN ==
			  FRF_AZ_BUF_OWNER_ID_HBUF_EVEN_LBN + 32);

	EFHW_BUILD_ASSERT(FRF_AZ_BUF_OWNER_ID_HBUF_ODD_WIDTH ==
			  FRF_AZ_BUF_OWNER_ID_HBUF_EVEN_WIDTH);
	EFHW_BUILD_ASSERT(FRF_AZ_BUF_ADR_HBUF_ODD_WIDTH == 
			  FRF_AZ_BUF_ADR_HBUF_EVEN_WIDTH);

	__DWCHCK(FRF_AZ_BUF_ADR_HBUF_EVEN);
	__DWCHCK(FRF_AZ_BUF_OWNER_ID_HBUF_EVEN);

	__RANGECHCK(dma_addr32, FRF_AZ_BUF_ADR_HBUF_EVEN_WIDTH);
	__RANGECHCK(own_id, FRF_AZ_BUF_OWNER_ID_HBUF_EVEN_WIDTH);

	return (dma_addr32 << FRF_AZ_BUF_ADR_HBUF_EVEN_LBN) |
		(own_id << FRF_AZ_BUF_OWNER_ID_HBUF_EVEN_LBN);
}

static inline uint64_t
falcon_nic_buffer_table_entry64_mk(dma_addr_t dma_addr,
				   int bufsz,	/* bytes */
				   int region, int own_id)
{
	__DW2CHCK(FRF_AZ_IP_DAT_BUF_SIZE);
	__DW2CHCK(FRF_AZ_BUF_ADR_REGION);
	__LWCHK(FRF_AZ_BUF_ADR_FBUF);
	__DWCHCK(FRF_AZ_BUF_OWNER_ID_FBUF);

	EFHW_ASSERT((bufsz == EFHW_4K) || (bufsz == EFHW_8K));

	dma_addr = (dma_addr >> 12) & __FALCON_MASK64(FRF_AZ_BUF_ADR_FBUF_WIDTH);

	__RANGECHCK(dma_addr, FRF_AZ_BUF_ADR_FBUF_WIDTH);
	__RANGECHCK(1, FRF_AZ_IP_DAT_BUF_SIZE_WIDTH);
	__RANGECHCK(region, FRF_AZ_BUF_ADR_REGION_WIDTH);
	__RANGECHCK(own_id, FRF_AZ_BUF_OWNER_ID_FBUF_WIDTH);

	return ((uint64_t) (bufsz == EFHW_8K) << FRF_AZ_IP_DAT_BUF_SIZE_LBN) |
		((uint64_t) region << FRF_AZ_BUF_ADR_REGION_LBN) |
		((uint64_t) dma_addr << FRF_AZ_BUF_ADR_FBUF_LBN) |
		((uint64_t) own_id << FRF_AZ_BUF_OWNER_ID_FBUF_LBN);
}

static inline void
_falcon_nic_buffer_table_set64(struct efhw_nic *nic,
			       dma_addr_t dma_addr, uint bufsz,
			       uint region, int own_id, int buffer_id)
{
	volatile char __iomem *offset;
	uint64_t entry;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	EFHW_ASSERT(region < FALCON_REGION_NUM);

	EFHW_ASSERT((bufsz == EFHW_4K) || (bufsz == EFHW_8K));

	offset = (efhw_kva + FR_AZ_BUF_FULL_TBL_OFST +
		  (buffer_id * FALCON_BUFFER_TBL_FULL_BYTES));

	entry = falcon_nic_buffer_table_entry64_mk(dma_addr, bufsz, region,
						   own_id);

	EFHW_TRACE("%s[%x]: %lx:bufsz=%x:region=%x:ownid=%x",
		   __FUNCTION__, buffer_id, (unsigned long) dma_addr, bufsz,
		   region, own_id);

	EFHW_TRACE("%s: BUF[%x]:NIC[%x]->%" PRIx64,
		   __FUNCTION__, buffer_id,
		   (unsigned int)(offset - efhw_kva), entry);

	/* Falcon requires that access to this register is serialised */
	falcon_write_q(offset, entry);

	/* Confirm the entry if the event queues haven't been set up. */
	if (!(nic->options & NIC_OPT_EFTEST)) {
		uint64_t new_entry;
		int count = MAX_BUF_TBL_READS;
#ifdef BUG_14512_WA
                int count2 = MAX_BUF_TBL_READS;
                count = MAX_MAX_BUF_TBL_READS;
#endif
		while (1) {
			falcon_read_q(offset, &new_entry);
			if (new_entry == entry)
				return;
			count--;
#ifdef BUG_14512_WA
                        if (count2-- <= 0 ) {
                          EFHW_ERR("%s: WARNING MAX_BUF_TBL_READS exceeded "
                                   "at ID %d (offset 0x%x)",
                                   __FUNCTION__, buffer_id,
                                   (unsigned)(offset - efhw_kva));
                          count2 = MAX_BUF_TBL_READS;
                        }
#endif
			if (count <= 0) {
				EFHW_ERR("%s: poll Timeout waiting at ID %d "
					 "(offset 0x%x) for value %"PRIx64
                                         " (last was %"PRIx64")",
                                         __FUNCTION__, buffer_id,
                                         (unsigned)(offset - efhw_kva),
					 entry, new_entry);
				EFHW_ASSERT(0);
			}
			udelay(1);
		}
	}
}

#define _falcon_nic_buffer_table_set _falcon_nic_buffer_table_set64

static inline void _falcon_nic_buffer_table_commit(struct efhw_nic *nic)
{
	/* MUST be called holding the FALCON_LOCK */
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	uint64_t cmd;


	__DW2CHCK(FRF_AZ_BUF_UPD_CMD);
	__RANGECHCK(1, FRF_AZ_BUF_UPD_CMD_WIDTH);

	cmd = ((uint64_t) 1 << FRF_AZ_BUF_UPD_CMD_LBN);

	/* Falcon requires 128 bit atomic access for this register */
	falcon_write_qq(efhw_kva + FR_AZ_BUF_TBL_UPD_REG_OFST,
			cmd, FALCON_ATOMIC_UPD_REG);
	mmiowb();

	nic->buf_commit_outstanding++;
	EFHW_TRACE("COMMIT REQ out=%d", nic->buf_commit_outstanding);
}

static void falcon_nic_buffer_table_commit(struct efhw_nic *nic)
{
	/* nothing to do */
}

static inline void
_falcon_nic_buffer_table_clear(struct efhw_nic *nic, int buffer_id, int num)
{
	uint64_t cmd;
	uint64_t start_id = buffer_id;
	uint64_t end_id = buffer_id + num - 1;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	volatile char __iomem *offset = (efhw_kva + FR_AZ_BUF_TBL_UPD_REG_OFST);

	EFHW_ASSERT(num >= 1);

	__DWCHCK(FRF_AZ_BUF_CLR_START_ID);
	__DW2CHCK(FRF_AZ_BUF_CLR_END_ID);

	__DW2CHCK(FRF_AZ_BUF_CLR_CMD);
	__RANGECHCK(1, FRF_AZ_BUF_CLR_CMD_WIDTH);

	__RANGECHCK(start_id, FRF_AZ_BUF_CLR_START_ID_WIDTH);
	__RANGECHCK(end_id, FRF_AZ_BUF_CLR_END_ID_WIDTH);

	cmd = (((uint64_t) 1 << FRF_AZ_BUF_CLR_CMD_LBN) |
	       (start_id << FRF_AZ_BUF_CLR_START_ID_LBN) |
	       (end_id << FRF_AZ_BUF_CLR_END_ID_LBN));

	/* Falcon requires 128 bit atomic access for this register */
	falcon_write_qq(offset, cmd, FALCON_ATOMIC_UPD_REG);
	mmiowb();

	nic->buf_commit_outstanding++;
	EFHW_TRACE("COMMIT CLEAR out=%d", nic->buf_commit_outstanding);
}

/*----------------------------------------------------------------------------
 *
 * Events low-level register interface
 *
 *---------------------------------------------------------------------------*/

static unsigned eventq_sizes[] = {
	512,
	EFHW_1K,
	EFHW_2K,
	EFHW_4K,
	EFHW_8K,
	EFHW_16K,
	EFHW_32K
};

#define N_EVENTQ_SIZES  (sizeof(eventq_sizes) / sizeof(eventq_sizes[0]))

static inline void falcon_nic_srm_upd_evq(struct efhw_nic *nic, int evq)
{
	/* set up the eventq which will receive events from the SRAM module.
	 * i.e buffer table updates and clears, TX and RX aperture table
	 * updates */

	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);


	__DWCHCK(FRF_AZ_SRM_UPD_EVQ_ID);
	__RANGECHCK(evq, FRF_AZ_SRM_UPD_EVQ_ID_WIDTH);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + FR_AZ_SRM_UPD_EVQ_REG_OFST,
				((uint64_t) evq << FRF_AZ_SRM_UPD_EVQ_ID_LBN),
				FALCON_ATOMIC_SRPM_UDP_EVQ_REG);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
}

static void
falcon_nic_evq_ptr_tbl(struct efhw_nic *nic,
		       uint evq,	/* evq id */
		       uint enable,	/* 1 to enable, 0 to disable */
		       uint buf_base_id,/* Buffer table base for EVQ */
		       uint evq_size,	/* Number of events */
		       uint enable_dos_p/* 1 to enable RPTR dos protection, 0 to disable*/)
{
	FALCON_LOCK_DECL;
	uint i;
        uint64_t val;
	ulong offset;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	/* size must be one of the various options, otherwise we assert */
	for (i = 0; i < N_EVENTQ_SIZES; i++) {
		if (evq_size <= eventq_sizes[i])
			break;
	}
	EFHW_ASSERT(i < N_EVENTQ_SIZES);

	__DWCHCK(FRF_AZ_EVQ_BUF_BASE_ID);
	__DWCHCK(FRF_AZ_EVQ_SIZE);
	__DWCHCK(FRF_AZ_EVQ_EN);

	__RANGECHCK(i, FRF_AZ_EVQ_SIZE_WIDTH);
	__RANGECHCK(buf_base_id, FRF_AZ_EVQ_BUF_BASE_ID_WIDTH);
	__RANGECHCK(1, FRF_AZ_EVQ_EN_WIDTH);

	if (nic->devtype.variant >= 'C') {
		__DW2CHCK(FRF_CZ_EVQ_DOS_PROTECT_EN);
		__RANGECHCK(1, FRF_CZ_EVQ_DOS_PROTECT_EN_WIDTH);
	} 

	/* if !enable then only evq needs to be correct, although valid
	 * values need to be passed in for other arguments to prevent
	 * assertions */
	
	if (nic->devtype.variant >= 'C')
		val = ((i << FRF_AZ_EVQ_SIZE_LBN) | 
                       (buf_base_id << FRF_AZ_EVQ_BUF_BASE_ID_LBN) |
                       (enable_dos_p ? ((uint64_t)1 << FRF_CZ_EVQ_DOS_PROTECT_EN_LBN) : 0) |
                       (enable ? (1 << FRF_AZ_EVQ_EN_LBN) : 0)); 
	else  
		val = ((i << FRF_AZ_EVQ_SIZE_LBN) | 
                       (buf_base_id << FRF_AZ_EVQ_BUF_BASE_ID_LBN) |
	               (enable ? (1 << FRF_AZ_EVQ_EN_LBN) : 0) );

	EFHW_ASSERT(evq < nic->num_evqs);

	offset = FR_AZ_EVQ_PTR_TBL_OFST;
	offset += evq * FALCON_REGISTER128;

	EFHW_TRACE("%s: evq %u en=%x:buf=%x:size=%x->%x at %lx",
		   __FUNCTION__, evq, enable, buf_base_id, evq_size,
		   (int) val, offset);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, val, FALCON_ATOMIC_PTR_TBL_REG);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);

	/* caller must wait for an update done event before writing any more
	   table entries */

	return;
}

void
falcon_nic_evq_ack(struct efhw_nic *nic,
		   uint evq,	/* evq id */
		   uint rptr	/* new read pointer update */)
{
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	unsigned offset;

	EFHW_ASSERT(evq < nic->num_evqs);

	__DWCHCK(FRF_AZ_EVQ_RPTR);
	__RANGECHCK(rptr, FRF_AZ_EVQ_RPTR_WIDTH);

	/* No nead to check nic->resetting here as this is equivalent
	 * to something a client (with no visibility of
	 * nic->resetting) could do anyway */

	offset = FR_AZ_EVQ_RPTR_REG_OFST + evq * FALCON_REGISTER128;
	writel(rptr << FRF_AZ_EVQ_RPTR_LBN, efhw_kva + offset);
	mmiowb();
}

/*---------------------------------------------------------------------------*/

static inline void
falcon_drv_ev(struct efhw_nic *nic, uint64_t data, uint qid)
{
	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	/* send an event from one driver to the other */
	EFHW_BUILD_ASSERT(FRF_AZ_DRV_EV_DATA_LBN == 0);
	EFHW_BUILD_ASSERT(FRF_AZ_DRV_EV_DATA_WIDTH == 64);
	EFHW_BUILD_ASSERT(FRF_AZ_DRV_EV_QID_LBN == 64);
	EFHW_BUILD_ASSERT(FRF_AZ_DRV_EV_QID_WIDTH == 12);

	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + FR_AZ_DRV_EV_REG_OFST, data, qid);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
}

_DEBUG_SYM_ void
falcon_ab_timer_tbl_set(struct efhw_nic *nic,
			uint evq,	/* timer id */
			uint mode,	/* mode bits */
			uint countdown /* counting value to set */)
{
	FALCON_LOCK_DECL;
	uint val;
	ulong offset;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);

	EFHW_BUILD_ASSERT(FRF_AB_TIMER_VAL_LBN == 0);

	__DWCHCK(FRF_AB_TIMER_MODE);
	__DWCHCK(FRF_AB_TIMER_VAL);

	__RANGECHCK(mode, FRF_AB_TIMER_MODE_WIDTH);
	__RANGECHCK(countdown, FRF_AB_TIMER_VAL_WIDTH);

	val = ((mode << FRF_AB_TIMER_MODE_LBN) | (countdown << FRF_AB_TIMER_VAL_LBN));

	if ((nic->devtype.variant == 'A') && (evq < FALCON_A_EVQ_CHAR)) {
		/* Assert that this is the CHAR bar */
		EFHW_ASSERT(nic->ctr_ap_bar == FALCON_S_CTR_AP_BAR);
		offset = FR_AA_TIMER_COMMAND_REG_KER_OFST;
		offset += evq * EFHW_8K;	/* PAGE mapped register */
	} else {
		offset = FR_AZ_TIMER_TBL_OFST;
		offset += evq * FALCON_REGISTER128;
	}
	EFHW_ASSERT(evq < nic->num_evqs);

	EFHW_TRACE("%s: evq %u mode %x (%s) time %x -> %08x",
		   __FUNCTION__, evq, mode,
		   mode == 0 ? "DISABLE" :
		   mode == 1 ? "IMMED" :
		   mode == 2 ? (evq < 5 ? "HOLDOFF" : "RX_TRIG") :
		   "<BAD>", countdown, val);

	/* Falcon requires 128 bit atomic access for this register when
	 * accessed from the driver. User access to timers is paged mapped
	 */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, val, FALCON_ATOMIC_TIMER_CMD_REG);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return;
}


_DEBUG_SYM_ void
siena_timer_tbl_set(struct efhw_nic *nic,
		int instance,
		int enable,
		int is_interrupting,
		int mode,
		int countdown)
{
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	FALCON_LOCK_DECL;
	unsigned offset;
	uint64_t val;

	EFHW_ASSERT(instance < (int)nic->num_evqs);
	__RANGECHCK(mode, FRF_CZ_TIMER_MODE_WIDTH);
	__RANGECHCK(countdown, FRF_CZ_TIMER_VAL_WIDTH);

	offset = FR_AZ_TIMER_TBL_OFST;
	offset += instance * FALCON_REGISTER128;

	val = (uint64_t) enable << FRF_CZ_TIMER_Q_EN_LBN;
	val |= (uint64_t) 0 << FRF_CZ_INT_ARMD_LBN;
	val |= (uint64_t) !is_interrupting << FRF_CZ_HOST_NOTIFY_MODE_LBN;
	val |= (uint64_t) mode << FRF_CZ_TIMER_MODE_LBN;
	val |= (uint64_t) countdown << FRF_CZ_TIMER_VAL_LBN;

	/* Falcon requires 128 bit atomic access for this register when
	 * accessed from the driver. User access to timers is paged mapped
	 */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, val, 0);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return;
}

/*--------------------------------------------------------------------
 *
 * Rate pacing - Low level interface
 *
 *--------------------------------------------------------------------*/
void falcon_nic_pace(struct efhw_nic *nic, uint dmaq, int pace)
{
	/* The pace delay imposed is (2^pace)*100ns unless the pace
	   value is zero in which case the delay is zero.  If the
	   delay is less than the IPG then it will effectively be
	   ignored because the IPG will be the limiting factor.

	   Pacing only available on the virtual interfaces
	 */
	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	ulong offset;

	if( pace < 0 )
		/* 21 is a special value that puts the queue in the pacing
		 * bin, but does not apply any extra IPG.
		 */
		pace = 21;
	else if( pace > 20 )
		pace = 20;	/* maxm supported value */

	__DWCHCK(FRF_AZ_TX_PACE);
	__RANGECHCK(pace, FRF_AZ_TX_PACE_WIDTH);

	switch (nic->devtype.variant) {
	case 'A':
		EFHW_ASSERT(dmaq >= FR_AA_TX_PACE_TBL_FIRST_QUEUE);
		offset = FR_AA_TX_PACE_TBL_OFST;
		offset += (dmaq - FR_AA_TX_PACE_TBL_FIRST_QUEUE) * 16;
		break;
	case 'B':
	case 'C':
		/* Would be nice to assert this, but as dmaq is unsigned and
		 * FRF_BZ_TX_PACE_TBL_FIRST_QUEUE is 0, it makes no sense
		 * EFHW_ASSERT(dmaq >= FRF_BZ_TX_PACE_TBL_FIRST_QUEUE);
		 */
		offset = FR_BZ_TX_PACE_TBL_OFST;
		offset += (dmaq - FR_BZ_TX_PACE_TBL_FIRST_QUEUE) * 16;
		break;
	default:
		EFHW_ASSERT(0);
		offset = 0;
		break;
	}

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, pace, FALCON_ATOMIC_PACE_REG);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);

	EFHW_TRACE("%s: txq %d offset=%lx pace=2^%x",
		   __FUNCTION__, dmaq, offset, pace);
}


/*--------------------------------------------------------------------
 *
 * RSS control
 *
 *--------------------------------------------------------------------*/

void falcon_nic_wakeup_mask_set(struct efhw_nic *nic, unsigned mask)
{
	uint64_t q0, q1;
	FALCON_LOCK_DECL;

	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_read_qq(EFHW_KVA(nic) + FR_AZ_EVQ_CTL_REG_OFST, 
			       &q0, &q1);
		switch (nic->devtype.variant) {
		case 'B':
			q0 &= ~Q0_MASK(FRF_BB_RX_EVQ_WAKEUP_MASK);
			q0 |= Q0_VALUE(FRF_BB_RX_EVQ_WAKEUP_MASK, mask);
			break;
		default:
			if (nic->devtype.variant >= 'C') {
				q0 &= ~Q0_MASK(FRF_CZ_RX_EVQ_WAKEUP_MASK);
				q0 |= Q0_VALUE(FRF_CZ_RX_EVQ_WAKEUP_MASK, 
					       mask);
			}
			break;
		}
		falcon_write_qq(EFHW_KVA(nic) + FR_AZ_EVQ_CTL_REG_OFST, 
				q0, q1);
	}
	FALCON_LOCK_UNLOCK(nic);
}

/*--------------------------------------------------------------------
 *
 * RXDP - low level interface
 *
 *--------------------------------------------------------------------*/

void
falcon_nic_set_rx_usr_buf_size(struct efhw_nic *nic, int usr_buf_bytes)
{
	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	uint64_t val, val2, usr_buf_size = usr_buf_bytes / 32;
	int rubs_lbn, rubs_width, roec_lbn;

	__DWCHCK(FRF_AA_RX_USR_BUF_SIZE);
	__DWCHCK(FRF_BZ_RX_USR_BUF_SIZE);
	__QWCHCK(FRF_AA_RX_OWNERR_CTL);
	__QWCHCK(FRF_BZ_RX_OWNERR_CTL);

	switch (nic->devtype.variant) {
	default:
		EFHW_ASSERT(0);
		/* Fall-through to avoid compiler warnings. */
	case 'A':
		rubs_lbn = FRF_AA_RX_USR_BUF_SIZE_LBN;
		rubs_width = FRF_AA_RX_USR_BUF_SIZE_WIDTH;
		roec_lbn = FRF_AA_RX_OWNERR_CTL_LBN;
		break;
	case 'B':
	case 'C':
		rubs_lbn = FRF_BZ_RX_USR_BUF_SIZE_LBN;
		rubs_width = FRF_BZ_RX_USR_BUF_SIZE_WIDTH;
		roec_lbn = FRF_AA_RX_OWNERR_CTL_LBN;
		break;
	}

	__RANGECHCK(usr_buf_size, rubs_width);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_read_qq(efhw_kva + FR_AZ_RX_CFG_REG_OFST, &val, &val2);

		val &= ~((__FALCON_MASK64(rubs_width)) << rubs_lbn);
		val |= (usr_buf_size << rubs_lbn);
		
		/* shouldn't be needed for a production driver */
		val |= ((uint64_t) 1 << roec_lbn);
		
		falcon_write_qq(efhw_kva + FR_AZ_RX_CFG_REG_OFST, val, val2);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
}
EXPORT_SYMBOL(falcon_nic_set_rx_usr_buf_size);


/*--------------------------------------------------------------------
 *
 * TXDP - low level interface
 *
 *--------------------------------------------------------------------*/

_DEBUG_SYM_ void falcon_nic_tx_cfg(struct efhw_nic *nic, int unlocked)
{
	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	uint64_t val1, val2;

	__DWCHCK(FRF_AZ_TX_OWNERR_CTL);
	__DWCHCK(FRF_AA_TX_NON_IP_DROP_DIS);

	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_read_qq(efhw_kva + FR_AZ_TX_CFG_REG_OFST, &val1, &val2);

		/* Will flag fatal interrupts on owner id errors. This
		 * should not be on for production code because there
		 * is otherwise a denial of serivce attack possible */
		val1 |= (1 << FRF_AZ_TX_OWNERR_CTL_LBN);

		/* Setup user queue TCP/UDP only packet security */
		if (unlocked)
			val1 |= (1 << FRF_AA_TX_NON_IP_DROP_DIS_LBN);
		else
			val1 &= ~(1 << FRF_AA_TX_NON_IP_DROP_DIS_LBN);
		
		falcon_write_qq(efhw_kva + FR_AZ_TX_CFG_REG_OFST, val1, val2);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
}

/*--------------------------------------------------------------------
 *
 * Random thresholds - Low level interface (Would like these to be op
 * defaults wherever possible)
 *
 *--------------------------------------------------------------------*/

void falcon_nic_pace_cfg(struct efhw_nic *nic, int fb_base, int bin_thresh)
{
	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	unsigned offset = 0;
	uint64_t val;

	__DWCHCK(FRF_AZ_TX_PACE_FB_BASE);
	__DWCHCK(FRF_AZ_TX_PACE_BIN_TH);

	switch (nic->devtype.variant) {
	case 'A':  offset = FR_AA_TX_PACE_REG_OFST;  break;
	case 'B':  offset = FR_BZ_TX_PACE_REG_OFST;  break;
	case 'C':  offset = FR_BZ_TX_PACE_REG_OFST;  break;
	default:   EFHW_ASSERT(0);                break;
	}

	val = (0x15 << FRF_AZ_TX_PACE_SB_NOT_AF_LBN);
	val |= (0xb << FRF_AZ_TX_PACE_SB_AF_LBN);

	val |= ((fb_base & __FALCON_MASK64(FRF_AZ_TX_PACE_FB_BASE_WIDTH)) <<
		 FRF_AZ_TX_PACE_FB_BASE_LBN);
	val |= ((bin_thresh & __FALCON_MASK64(FRF_AZ_TX_PACE_BIN_TH_WIDTH)) <<
		 FRF_AZ_TX_PACE_BIN_TH_LBN);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + offset, val, 0);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
}


/**********************************************************************
 * Implementation of the HAL. ********************************************
 **********************************************************************/

/*----------------------------------------------------------------------------
 *
 * Initialisation and configuration discovery
 *
 *---------------------------------------------------------------------------*/

static void falcon_nic_close_hardware(struct efhw_nic *nic)
{
	/* check we are in possession of some hardware */
	if (!efhw_nic_have_hw(nic))
		return;
}

#ifndef __ci_ul_driver__
static
#endif
int falcon_nic_get_mac_config(struct efhw_nic *nic)
{
	switch (nic->devtype.variant) {
	case 'A':
	case 'B':
        {
         	volatile char __iomem *efhw_kva = nic->bar_ioaddr;
                uint32_t altera;
                altera = readl(efhw_kva + FR_AZ_ALTERA_BUILD_REG_OFST);
		nic->devtype.in_fpga = (altera != 0);
		break;
        }
	default:
		break;
	}

	nic->flags |= NIC_FLAG_10G;
	return 0;
}


void
falcon_nic_tweak_hardware(struct efhw_nic *nic)
{
	/* RXDP tweaks */

	/* ?? bug2396 rx_cfg should be ok so long as the net driver
	 * always pushes buffers big enough for the link MTU */

	/* set the RX buffer cutoff size to be the same as PAGE_SIZE.
	 * Use this value when we think that there will be a lot of
	 * jumbo frames.
	 *
	 * The default value 1600 is useful when packets are small,
	 * but would means that jumbo frame RX queues would need more
	 * descriptors pushing */
	falcon_nic_set_rx_usr_buf_size(nic, FALCON_RX_USR_BUF_SIZE);

	/* TXDP tweaks */
	/* ?? bug2396 looks ok */
	falcon_nic_tx_cfg(nic, /*unlocked(for non-UDP/TCP)= */ 0);
	falcon_nic_pace_cfg(nic, 4, 2);
}


static int
falcon_nic_init_hardware(struct efhw_nic *nic,
			 struct efhw_ev_handler *ev_handlers,
			 const uint8_t *mac_addr, int non_irq_evq)
{
	int rc;

	/* header sanity checks */
	FALCON_ASSERT_VALID();

	rc = falcon_nic_get_mac_config(nic);
	if (rc < 0)
		return rc;

	/* Initialise the top level hardware blocks */
	memcpy(nic->mac_addr, mac_addr, ETH_ALEN);

	EFHW_TRACE("%s:", __FUNCTION__);

	/*****************************************************************
	 * The rest of this function deals with initialization of the NICs
	 * hardware (as opposed to the initialization of the
	 * struct efhw_nic data structure */

	if (non_irq_evq == 0)
		return 0;

	/* char driver grabs SRM events onto the non interrupting
	 * event queue */
	falcon_nic_srm_upd_evq(nic, non_irq_evq);

	falcon_nic_tweak_hardware(nic);

	rc = efhw_keventq_ctor(nic, non_irq_evq,
			       &nic->non_interrupting_evq, NULL);
	if (rc < 0) {
		EFHW_ERR("%s: efhw_keventq_ctor() failed (%d) evq=%d",
			 __FUNCTION__, rc, non_irq_evq);
		return rc;
	}

	return 0;
}

/*--------------------------------------------------------------------
 *
 * Interrupt
 *
 *--------------------------------------------------------------------*/

static void
falcon_nic_set_interrupt_moderation(struct efhw_nic *nic, int evq,
				    uint32_t val)
{
	EFHW_ASSERT(evq >= 0);

	if (nic->devtype.variant < 'C')
		falcon_ab_timer_tbl_set(nic, evq, FFE_AB_TIMER_MODE_INT_HLDOFF, val / 5);
	else
		siena_timer_tbl_set(nic, evq, 1/*enable*/, 1/*interrupting*/,
				    FFE_CZ_TIMER_MODE_INT_HLDOFF, val*10/61);
}

/*--------------------------------------------------------------------
 *
 * Event Management - and SW event posting
 *
 *--------------------------------------------------------------------*/

static void
falcon_nic_event_queue_enable(struct efhw_nic *nic, uint evq, uint evq_size,
			      uint buf_base_id, int interrupting, int enable_dos_p)
{
	EFHW_ASSERT(nic);

	if (nic->devtype.variant < 'C')
		/* Whether or not queue has an interrupt depends on
		 * instance number and h/w variant, so [interrupting] is
		 * ignored.
		 */
		falcon_ab_timer_tbl_set(nic, evq, 0/*disable*/, 0);
	else
		siena_timer_tbl_set(nic, evq, 1/*enable*/, interrupting,
				    FFE_CZ_TIMER_MODE_DIS, 0);

	falcon_nic_evq_ptr_tbl(nic, evq, 1, buf_base_id, evq_size, enable_dos_p);
	EFHW_TRACE("%s: enable evq %u size %u", __FUNCTION__, evq, evq_size);
}

static void
falcon_nic_event_queue_disable(struct efhw_nic *nic, uint evq, int timer_only)
{
	EFHW_ASSERT(nic);

	if (nic->devtype.variant < 'C')
		falcon_ab_timer_tbl_set(nic, evq, 0 /* disable */ , 0);
	else
		siena_timer_tbl_set(nic, evq, timer_only /* enable */,
				    0 /* interrupting */,
				    FFE_CZ_TIMER_MODE_DIS, 0);

	if (!timer_only)
		falcon_nic_evq_ptr_tbl(nic, evq, 0, 0, 0, 0);
	EFHW_TRACE("%s: disenable evq %u", __FUNCTION__, evq);
}

static void
falcon_nic_wakeup_request(struct efhw_nic *nic, int next_i, int evq)
{
	EFHW_ASSERT(evq >= 0);
	falcon_nic_evq_ack(nic, evq, next_i);
	EFHW_TRACE("%s: evq %d next_i %d", __FUNCTION__, evq, next_i);
}

static void falcon_nic_sw_event(struct efhw_nic *nic, int data, int evq)
{
	uint64_t ev_data = data;

	ev_data &= ~FALCON_EVENT_CODE_MASK;
	ev_data |= FALCON_EVENT_CODE_SW;

	falcon_drv_ev(nic, ev_data, evq);
	EFHW_NOTICE("%s: evq[%d]->%x", __FUNCTION__, evq, data);
}


/*--------------------------------------------------------------------
 *
 * Buffer table - helpers
 *
 *--------------------------------------------------------------------*/

#define FALCON_LAZY_COMMIT_HWM (FALCON_BUFFER_UPD_MAX - 16)

/* Note re.:
 *  falcon_nic_buffer_table_lazy_commit(struct efhw_nic *nic)
 *  falcon_nic_buffer_table_update_poll(struct efhw_nic *nic)
 *  falcon_nic_buffer_table_confirm(struct efhw_nic *nic)
 * -- these are no-ops in the user-level driver because it would need to
 * coordinate with the real driver on the number of outstanding commits.
 *
 * An exception is made for eftest apps, which manage the hardware without
 * using the char driver.
 */

static inline void falcon_nic_buffer_table_lazy_commit(struct efhw_nic *nic)
{
#if defined(__ci_ul_driver__)
	if (!(nic->options & NIC_OPT_EFTEST))
		return;
#endif

	/* Do nothing if operating in synchronous mode. */
	if (nic->options & NIC_OPT_EFTEST)
		return;
}

static inline void falcon_nic_buffer_table_update_poll(struct efhw_nic *nic)
{
	FALCON_LOCK_DECL;
	int count = 0, rc = 0;

#if defined(__ci_ul_driver__)
	if (!(nic->options & NIC_OPT_EFTEST))
		return;
#endif

	/* We can be called here early days */
	if (nic->non_interrupting_evq.instance == 0)
		return;

	/* If we need to gather buffer update events then poll the
	   non-interrupting event queue */

	/* For each _buffer_table_commit there will be an update done
	   event. We don't keep track of how many buffers each commit has
	   committed, just make sure that all the expected events have been
	   gathered */
	FALCON_LOCK_LOCK(nic);

	if (!nic->resetting)
		goto out;

	EFHW_TRACE("%s: %d", __FUNCTION__, nic->buf_commit_outstanding);

	while (nic->buf_commit_outstanding > 0) {
		/* we're not expecting to handle any events that require
		 * upcalls into the core driver */
		struct efhw_ev_handler handler;
		memset(&handler, 0, sizeof(handler));
		nic->non_interrupting_evq.ev_handlers = &handler;
		rc = efhw_keventq_poll(nic, &nic->non_interrupting_evq);
		nic->non_interrupting_evq.ev_handlers = NULL;

		if (rc < 0) {
			EFHW_ERR("%s: poll ERROR (%d:%d) ***** ",
				 __FUNCTION__, rc,
				 nic->buf_commit_outstanding);
			goto out;
		}

		FALCON_LOCK_UNLOCK(nic);

		if (count++)
			udelay(1);

		if (count > MAX_BUF_TBL_READS) {
			EFHW_ERR("%s: poll Timeout ***** (%d)", __FUNCTION__,
				 nic->buf_commit_outstanding);
			nic->buf_commit_outstanding = 0;
			return;
		}
		FALCON_LOCK_LOCK(nic);

		if (!nic->resetting)
			goto out;
	}

out:
	FALCON_LOCK_UNLOCK(nic);
	return;
}

void falcon_nic_buffer_table_confirm(struct efhw_nic *nic)
{
	/* confirm buffer table updates - should be used for items where
	   loss of data would be unacceptable. E.g for the buffers that back
	   an event or DMA queue */
	FALCON_LOCK_DECL;

#if defined(__ci_ul_driver__)
	if (!(nic->options & NIC_OPT_EFTEST))
		return;
#endif

	FALCON_LOCK_LOCK(nic);

	if (!nic->resetting)
		_falcon_nic_buffer_table_commit(nic);

	FALCON_LOCK_UNLOCK(nic);

	falcon_nic_buffer_table_update_poll(nic);
}

/*--------------------------------------------------------------------
 *
 * Buffer table - API
 *
 *--------------------------------------------------------------------*/

static void
falcon_nic_buffer_table_clear(struct efhw_nic *nic, int buffer_id, int num)
{
	FALCON_LOCK_DECL;
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting)
		_falcon_nic_buffer_table_clear(nic, buffer_id, num);
	FALCON_LOCK_UNLOCK(nic);
}

void
falcon_nic_buffer_table_set(struct efhw_nic *nic, dma_addr_t dma_addr,
			    uint region,
			    int own_id, int buffer_id)
{
	FALCON_LOCK_DECL;

	EFHW_ASSERT(region < FALCON_REGION_NUM);

	falcon_nic_buffer_table_update_poll(nic);
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		_falcon_nic_buffer_table_set(nic, dma_addr, EFHW_NIC_PAGE_SIZE,
					     region, own_id, buffer_id);
		falcon_nic_buffer_table_lazy_commit(nic);
	}
	FALCON_LOCK_UNLOCK(nic);
}

void
falcon_nic_buffer_table_set_n(struct efhw_nic *nic, int buffer_id,
			      dma_addr_t dma_addr, uint region,
			      int n_pages, int own_id)
{
	/* used to set up a contiguous range of buffers */
	FALCON_LOCK_DECL;

	EFHW_ASSERT(region < FALCON_REGION_NUM);

	while (n_pages--) {
		falcon_nic_buffer_table_update_poll(nic);
		FALCON_LOCK_LOCK(nic);
		if (!nic->resetting) {
			_falcon_nic_buffer_table_set(nic, dma_addr, 
						     EFHW_NIC_PAGE_SIZE,
						     region, own_id, 
						     buffer_id);
			falcon_nic_buffer_table_lazy_commit(nic);
		}
		FALCON_LOCK_UNLOCK(nic);
		buffer_id++;
		dma_addr += EFHW_NIC_PAGE_SIZE;
	}
}

/*--------------------------------------------------------------------
 *
 * DMA Queues - mid level API
 *
 *--------------------------------------------------------------------*/

static inline int
__falcon_really_flush_tx_dma_channel(struct efhw_nic *nic, uint dmaq)
{
	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	uint val;

	__DWCHCK(FRF_AZ_TX_FLUSH_DESCQ_CMD);
	__DWCHCK(FRF_AZ_TX_FLUSH_DESCQ);
	__RANGECHCK(dmaq, FRF_AZ_TX_FLUSH_DESCQ_WIDTH);

	val = ((1 << FRF_AZ_TX_FLUSH_DESCQ_CMD_LBN) | (dmaq << FRF_AZ_TX_FLUSH_DESCQ_LBN));

	EFHW_TRACE("TX DMA flush[%d]", dmaq);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + FR_AZ_TX_FLUSH_DESCQ_REG_OFST,
				val, FALCON_ATOMIC_TX_FLUSH_DESCQ);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return 0;
}

static inline int
__falcon_is_tx_dma_channel_flushed(struct efhw_nic *nic, uint dmaq)
{
	FALCON_LOCK_DECL;
	uint64_t val_low64, val_high64;
	uint64_t enable, flush_pending;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	ulong offset = falcon_dma_tx_q_offset(nic, dmaq);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting)
		falcon_read_qq(efhw_kva + offset, &val_low64, &val_high64);
	else {
		FALCON_LOCK_UNLOCK(nic);
		return 0;
	}
	FALCON_LOCK_UNLOCK(nic);

	/* should see one of three values for these 2 bits
	 *   1, queue enabled no flush pending
	 *	- i.e. first flush request
	 *   2, queue enabled, flush pending
	 *	- i.e. request to reflush before flush finished
	 *   3, queue disabled (no flush pending)
	 *	- flush complete
	 */
	__DWCHCK(FRF_AZ_TX_DESCQ_FLUSH);
	__DW3CHCK(FRF_AZ_TX_DESCQ_EN);
	enable = val_high64 & (1 << __DW3(FRF_AZ_TX_DESCQ_EN_LBN));
	flush_pending = val_low64 & (1 << FRF_AZ_TX_DESCQ_FLUSH_LBN);

	if (enable && !flush_pending)
		return 0;

	EFHW_TRACE("%d, %s: %s, %sflush pending", dmaq, __FUNCTION__,
		   enable ? "enabled" : "disabled",
		   flush_pending ? "" : "NO ");
	/* still in progress */
	if (enable && flush_pending)
		return -EALREADY;

	return -EAGAIN;
}

static int falcon_flush_tx_dma_channel(struct efhw_nic *nic, uint dmaq)
{
	int rc;
	rc = __falcon_is_tx_dma_channel_flushed(nic, dmaq);
	if (rc < 0) {
		EFHW_WARN("%s: failed %d", __FUNCTION__, rc);
		return rc;
	}
	return __falcon_really_flush_tx_dma_channel(nic, dmaq);
}

static int
__falcon_really_flush_rx_dma_channel(struct efhw_nic *nic, uint dmaq)
{
	FALCON_LOCK_DECL;
	volatile char __iomem *efhw_kva = EFHW_KVA(nic);
	uint val;


	__DWCHCK(FRF_AZ_RX_FLUSH_DESCQ_CMD);
	__DWCHCK(FRF_AZ_RX_FLUSH_DESCQ);
	__RANGECHCK(dmaq, FRF_AZ_RX_FLUSH_DESCQ_WIDTH);

	val = ((1 << FRF_AZ_RX_FLUSH_DESCQ_CMD_LBN) | (dmaq << FRF_AZ_RX_FLUSH_DESCQ_LBN));

	EFHW_TRACE("RX DMA flush[%d]", dmaq);

	/* Falcon requires 128 bit atomic access for this register */
	FALCON_LOCK_LOCK(nic);
	if (!nic->resetting) {
		falcon_write_qq(efhw_kva + FR_AZ_RX_FLUSH_DESCQ_REG_OFST, val,
				FALCON_ATOMIC_RX_FLUSH_DESCQ);
		mmiowb();
	}
	FALCON_LOCK_UNLOCK(nic);
	return 0;
}


static int falcon_flush_rx_dma_channel(struct efhw_nic *nic, uint dmaq)
{
	return __falcon_really_flush_rx_dma_channel(nic, dmaq);
}

/*--------------------------------------------------------------------
 *
 * Falcon specific event callbacks
 *
 *--------------------------------------------------------------------*/

int
falcon_handle_char_event(struct efhw_nic *nic, struct efhw_ev_handler *h,
			 efhw_event_t *ev)
{
	EFHW_TRACE("DRIVER EVENT: "FALCON_EVENT_FMT,
		   FALCON_EVENT_PRI_ARG(*ev));

	switch (FALCON_EVENT_DRIVER_SUBCODE(ev)) {

	case TX_DESCQ_FLS_DONE_EV_DECODE:
		EFHW_TRACE("TX[%d] flushed",
			   (int)FALCON_EVENT_TX_FLUSH_Q_ID(ev));
#if !defined(__ci_ul_driver__)
		efhw_handle_txdmaq_flushed(nic, h, ev);
#endif
		break;

	case RX_DESCQ_FLS_DONE_EV_DECODE:
		EFHW_TRACE("RX[%d] flushed",
			   (int)FALCON_EVENT_TX_FLUSH_Q_ID(ev));
#if !defined(__ci_ul_driver__)
		efhw_handle_rxdmaq_flushed(nic, h, ev);
#endif
		break;

	case SRM_UPD_DONE_EV_DECODE:
		nic->buf_commit_outstanding =
		    max(0, nic->buf_commit_outstanding - 1);
		EFHW_TRACE("COMMIT DONE %d", nic->buf_commit_outstanding);
		break;

	case EVQ_INIT_DONE_EV_DECODE:
		EFHW_TRACE("%sEVQ INIT", "");
		break;

	case WAKE_UP_EV_DECODE:
		EFHW_TRACE("%sWAKE UP", "");
		efhw_handle_wakeup_event(nic, h, ev);
		break;

	case TIMER_EV_DECODE:
		EFHW_TRACE("%sTIMER", "");
		efhw_handle_timeout_event(nic, h, ev);
		break;

	case RX_DESCQ_FLSFF_OVFL_EV_DECODE:
		/* This shouldn't happen. */
		EFHW_ERR("%s: RX flush fifo overflowed", __FUNCTION__);
		return -EINVAL;

	default:
		EFHW_TRACE("UNKOWN DRIVER EVENT: " FALCON_EVENT_FMT,
			   FALCON_EVENT_PRI_ARG(*ev));
		break;
	}
	return 0;
}


/*--------------------------------------------------------------------
 *
 * Abstraction Layer Hooks
 *
 *--------------------------------------------------------------------*/

struct efhw_func_ops falcon_char_functional_units = {
	falcon_nic_close_hardware,
	falcon_nic_init_hardware,
	falcon_nic_tweak_hardware,
	falcon_nic_set_interrupt_moderation,
	falcon_nic_event_queue_enable,
	falcon_nic_event_queue_disable,
	falcon_nic_wakeup_request,
	falcon_nic_sw_event,
	falcon_dmaq_tx_q_init,
	falcon_dmaq_rx_q_init,
	falcon_dmaq_tx_q_disable,
	falcon_dmaq_rx_q_disable,
	falcon_flush_tx_dma_channel,
	falcon_flush_rx_dma_channel,
	falcon_nic_buffer_table_set,
	falcon_nic_buffer_table_set_n,
	falcon_nic_buffer_table_clear,
	falcon_nic_buffer_table_commit,
};
