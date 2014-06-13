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
 * Driver for Solarflare Solarstorm network controllers and boards
 * Copyright 2006-2010 Solarflare Communications Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation, incorporated herein by reference.
 */

#ifndef EFX_WORKAROUNDS_H
#define EFX_WORKAROUNDS_H

/*
 * Hardware workarounds.
 * Bug numbers are from Solarflare's Bugzilla.
 */

#define EFX_WORKAROUND_ALWAYS(efx) 1
#define EFX_WORKAROUND_FALCON_A(efx) (efx_nic_rev(efx) <= EFX_REV_FALCON_A1)
#define EFX_WORKAROUND_FALCON_B(efx) (efx_nic_rev(efx) == EFX_REV_FALCON_B0)
#define EFX_WORKAROUND_FALCON_AB(efx) (efx_nic_rev(efx) <= EFX_REV_FALCON_B0)
#define EFX_WORKAROUND_SIENA(efx) (efx_nic_rev(efx) == EFX_REV_SIENA_A0)
#define EFX_WORKAROUND_10G(efx) 1

/* XAUI resets if link not detected */
#define EFX_WORKAROUND_5147 EFX_WORKAROUND_ALWAYS
/* Reprog PCIe ACK timer to workaround issue in PCIe IP block */
#define EFX_WORKAROUND_6943 EFX_WORKAROUND_FALCON_B
/* RX PCIe double split performance issue */
#define EFX_WORKAROUND_7575 EFX_WORKAROUND_ALWAYS
/* Bit-bashed I2C reads cause performance drop */
#define EFX_WORKAROUND_7884 EFX_WORKAROUND_10G
/* Selftests need to be retried */
#define EFX_WORKAROUND_8568 EFX_WORKAROUND_ALWAYS
/* Queued ACKs aren't flushed before L1 entry */
#define EFX_WORKAROUND_9096 EFX_WORKAROUND_FALCON_B
/* TX_EV_PKT_ERR can be caused by a dangling TX descriptor
 * or a PCIe error (bug 11028) */
#define EFX_WORKAROUND_10727 EFX_WORKAROUND_ALWAYS
/* Transmit flow control may get disabled */
#define EFX_WORKAROUND_11482 EFX_WORKAROUND_FALCON_AB
/* Truncated IPv4 packets can confuse the TX packet parser */
#define EFX_WORKAROUND_15592 EFX_WORKAROUND_FALCON_AB
/* Legacy ISR read can return zero once */
#define EFX_WORKAROUND_15783 EFX_WORKAROUND_ALWAYS
/* Legacy interrupt storm when interrupt fifo fills */
#define EFX_WORKAROUND_17213 EFX_WORKAROUND_SIENA

/* Spurious parity errors in TSORT buffers */
#define EFX_WORKAROUND_5129 EFX_WORKAROUND_FALCON_A
/* Unaligned read request >512 bytes after aligning may break TSORT */
#define EFX_WORKAROUND_5391 EFX_WORKAROUND_FALCON_A
/* iSCSI parsing errors */
#define EFX_WORKAROUND_5583 EFX_WORKAROUND_FALCON_A
/* RX events go missing */
#define EFX_WORKAROUND_5676 EFX_WORKAROUND_FALCON_A
/* RX_RESET on A1 */
#define EFX_WORKAROUND_6555 EFX_WORKAROUND_FALCON_A
/* Increase filter depth to avoid RX_RESET */
#define EFX_WORKAROUND_7244 EFX_WORKAROUND_FALCON_A
/* Flushes may never complete */
#define EFX_WORKAROUND_7803 EFX_WORKAROUND_FALCON_AB
/* Leak overlength packets rather than free */
#define EFX_WORKAROUND_8071 EFX_WORKAROUND_FALCON_A

/* Invalid BAR accesses may not be completed, leading to NMI */
#define EFX_WORKAROUND_11368 EFX_WORKAROUND_FALCON_B

#endif /* EFX_WORKAROUNDS_H */
