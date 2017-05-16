/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2014-2016 Freescale Semiconductor. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Freescale Semiconductor nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */
#ifndef __DPAA_ETHDEV_H__
#define __DPAA_ETHDEV_H__

/* System headers */
#include <stdbool.h>
#include <rte_ethdev.h>

#include "dpaa_logs.h"

#include <usdpaa/fsl_usd.h>
#include <usdpaa/fsl_qman.h>
#include <usdpaa/fsl_bman.h>
#include <usdpaa/of.h>
#include <usdpaa/usdpaa_netcfg.h>

#define FSL_CLASS_ID		0
#define FSL_VENDOR_ID		0x1957
#define FSL_DEVICE_ID		0x410	 /* custom */
#define FSL_FMAN_ETH_CLASS	0x020000 /* ethernet */
#define FSL_SUBSYSTEM_VENDOR	0
#define FSL_SUBSYSTEM_DEVICE	0

#define FSL_DPAA_DOMAIN	2
#define FSL_DPAA_BUSID	16
#define FSL_DPAA_FUNC		0

#define MAX_ETHDEV_NAME 32
#define ETHDEV_NAME_PREFIX      "dpaaeth"

#define DPAA_MBUF_HW_ANNOTATION		64
#define DPAA_FD_PTA_SIZE		64

#if (DPAA_MBUF_HW_ANNOTATION + DPAA_FD_PTA_SIZE) > RTE_PKTMBUF_HEADROOM
#error "Annotation requirement is more than RTE_PKTMBUF_HEADROOM"
#endif

/* we will re-use the HEADROOM for annotation in RX */
#define DPAA_HW_BUF_RESERVE	0
#define DPAA_PACKET_LAYOUT_ALIGN	64

/* Alignment to use for cpu-local structs to avoid coherency problems. */
#define MAX_CACHELINE			64
#define CPU_SPIN_BACKOFF_CYCLES		512

#define DPAA_MIN_RX_BUF_SIZE 512
#define DPAA_MAX_RX_PKT_LEN  10240

/* RX queue tail drop threshold
 * currently considering 32 KB packets */
#define CONG_THRESHOLD_RX_Q  (32 * 1024)

/* total number of bpools on SoC */
#define DPAA_MAX_BPOOLS	256

/*max mac filter for memac(8) including primary mac addr*/
#define DPAA_MAX_MAC_FILTER (MEMAC_NUM_OF_PADDRS + 1)

/* Maximum release/acquire from BMAN */
#define DPAA_MBUF_MAX_ACQ_REL  8

/*Maximum number of slots available in TX ring*/
#define MAX_TX_RING_SLOTS	8

/* PCD frame queues */
#define DPAA_PCD_FQID_START		0x400
#define DPAA_PCD_FQID_MULTIPLIER	0x100
#define DPAA_DEFAULT_NUM_PCD_QUEUES	1

#define DPAA_IF_TX_PRIORITY		3
#define DPAA_IF_RX_PRIORITY		4
#define DPAA_IF_DEBUG_PRIORITY		7

#define DPAA_IF_RX_ANNOTATION_STASH	1
#define DPAA_IF_RX_DATA_STASH		1
#define DPAA_IF_RX_CONTEXT_STASH		0

/* Each "debug" FQ is represented by one of these */
#define DPAA_DEBUG_FQ_RX_ERROR   0
#define DPAA_DEBUG_FQ_TX_ERROR   1

#define DPAA_RSS_OFFLOAD_ALL ( \
	ETH_RSS_FRAG_IPV4 | \
	ETH_RSS_NONFRAG_IPV4_TCP | \
	ETH_RSS_NONFRAG_IPV4_UDP | \
	ETH_RSS_NONFRAG_IPV4_SCTP | \
	ETH_RSS_FRAG_IPV6 | \
	ETH_RSS_NONFRAG_IPV6_TCP | \
	ETH_RSS_NONFRAG_IPV6_UDP | \
	ETH_RSS_NONFRAG_IPV6_SCTP)

#define DPAA_TX_CKSUM_OFFLOAD_MASK (             \
		PKT_TX_IP_CKSUM |                \
		PKT_TX_TCP_CKSUM |                 \
		PKT_TX_UDP_CKSUM)

/**************************************************************************//**
	DPAA Frame descriptor macros
*/ /***************************************************************************/
#define DPAA_FD_CMD_FCO			0x80000000  /**< Frame queue Context Override */
#define DPAA_FD_CMD_RPD			0x40000000  /**< Read Prepended Data */
#define DPAA_FD_CMD_UPD			0x20000000  /**< Update Prepended Data */
#define DPAA_FD_CMD_DTC			0x10000000  /**< Do IP/TCP/UDP Checksum */
#define DPAA_FD_CMD_DCL4C		0x10000000  /**< Didn't calculate L4 Checksum */
#define DPAA_FD_CMD_CFQ			0x00ffffff  /**< Confirmation Frame Queue */

struct pool_info_entry {
	struct rte_mempool *mp;
	struct bman_pool *bp;
	uint32_t bpid;
	uint32_t size;
	uint32_t meta_data_size;
	int32_t dpaa_ops_index;
};

/* Each network interface is represented by one of these */
struct dpaa_if {
	int valid;
	char name[MAX_ETHDEV_NAME];
	const struct fm_eth_port_cfg *cfg;
	struct qman_fq *rx_queues;
	struct qman_fq *tx_queues;
	struct qman_fq debug_queues[2];
	uint16_t nb_rx_queues;
	uint16_t nb_tx_queues;
	uint32_t ifid;
	struct fman_if *fif;
	struct pool_info_entry *bp_info;
	struct rte_eth_fc_conf *fc_conf;
};

struct dpaa_portal {
	uint32_t bman_idx; /**< BMAN Portal ID*/
	uint32_t qman_idx; /**< QMAN Portal ID*/
	uint64_t tid;/**< Parent Thread id for this portal */
};

/*! Global per thread portal */
RTE_DECLARE_PER_LCORE(bool, _dpaa_io);

extern struct pool_info_entry dpaa_pool_table[DPAA_MAX_BPOOLS];

#define DPAA_BPID_TO_POOL_INFO(__bpid) &dpaa_pool_table[__bpid]

#define DPAA_MEMPOOL_TO_BPID(__mp) \
	((struct pool_info_entry *)__mp->pool_data)->bpid

#define DPAA_MEMPOOL_TO_POOL_INFO(__mp) \
	(struct pool_info_entry *)__mp->pool_data

/* todo - this is costly, need to write a fast coversion routine */
static inline void *dpaa_mem_ptov(phys_addr_t paddr)
{
	const struct rte_memseg *memseg = rte_eal_get_physmem_layout();
	int i;

	for (i = 0; i < RTE_MAX_MEMSEG && memseg[i].addr != NULL; i++) {
		if (paddr >= memseg[i].phys_addr && paddr <
			memseg[i].phys_addr + memseg[i].len)
			return (uint8_t *)(memseg[i].addr) + (paddr - memseg[i].phys_addr);
	}

	return NULL;
}

int dpaa_portal_init(void *arg);

int dpaa_pre_rte_eal_init(void);

#endif
