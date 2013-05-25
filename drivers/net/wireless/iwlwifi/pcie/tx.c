/******************************************************************************
 *
 * Copyright(c) 2003 - 2013 Intel Corporation. All rights reserved.
 *
 * Portions of this file are derived from the ipw3945 project, as well
 * as portions of the ieee80211 subsystem header files.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of version 2 of the GNU General Public License as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110, USA
 *
 * The full GNU General Public License is included in this distribution in the
 * file called LICENSE.
 *
 * Contact Information:
 *  Intel Linux Wireless <ilw@linux.intel.com>
 * Intel Corporation, 5200 N.E. Elam Young Parkway, Hillsboro, OR 97124-6497
 *
 *****************************************************************************/
#include <linux/etherdevice.h>
#include <linux/slab.h>
#include <linux/sched.h>

#include "iwl-debug.h"
#include "iwl-csr.h"
#include "iwl-prph.h"
#include "iwl-io.h"
#include "iwl-op-mode.h"
#include "internal.h"
/* FIXME: need to abstract out TX command (once we know what it looks like) */
#include "dvm/commands.h"

#define IWL_TX_CRC_SIZE 4
#define IWL_TX_DELIMITER_SIZE 4

/*************** DMA-QUEUE-GENERAL-FUNCTIONS  *****
 * DMA services
 *
 * Theory of operation
 *
 * A Tx or Rx queue resides in host DRAM, and is comprised of a circular buffer
 * of buffer descriptors, each of which points to one or more data buffers for
 * the device to read from or fill.  Driver and device exchange status of each
 * queue via "read" and "write" pointers.  Driver keeps minimum of 2 empty
 * entries in each circular buffer, to protect against confusing empty and full
 * queue states.
 *
 * The device reads or writes the data in the queues via the device's several
 * DMA/FIFO channels.  Each queue is mapped to a single DMA channel.
 *
 * For Tx queue, there are low mark and high mark limits. If, after queuing
 * the packet for Tx, free space become < low mark, Tx queue stopped. When
 * reclaiming packets (on 'tx done IRQ), if free space become > high mark,
 * Tx queue resumed.
 *
 ***************************************************/
static int iwl_queue_space(const struct iwl_queue *q)
{
	int s = q->read_ptr - q->write_ptr;

	if (q->read_ptr > q->write_ptr)
		s -= q->n_bd;

	if (s <= 0)
		s += q->n_window;
	/* keep some reserve to not confuse empty and full situations */
	s -= 2;
	if (s < 0)
		s = 0;
	return s;
}

/*
 * iwl_queue_init - Initialize queue's high/low-water and read/write indexes
 */
static int iwl_queue_init(struct iwl_queue *q, int count, int slots_num, u32 id)
{
	q->n_bd = count;
	q->n_window = slots_num;
	q->id = id;

	/* count must be power-of-two size, otherwise iwl_queue_inc_wrap
	 * and iwl_queue_dec_wrap are broken. */
	if (WARN_ON(!is_power_of_2(count)))
		return -EINVAL;

	/* slots_num must be power-of-two size, otherwise
	 * get_cmd_index is broken. */
	if (WARN_ON(!is_power_of_2(slots_num)))
		return -EINVAL;

	q->low_mark = q->n_window / 4;
	if (q->low_mark < 4)
		q->low_mark = 4;

	q->high_mark = q->n_window / 8;
	if (q->high_mark < 2)
		q->high_mark = 2;

	q->write_ptr = 0;
	q->read_ptr = 0;

	return 0;
}

static int iwl_pcie_alloc_dma_ptr(struct iwl_trans *trans,
				  struct iwl_dma_ptr *ptr, size_t size)
{
	if (WARN_ON(ptr->addr))
		return -EINVAL;

	ptr->addr = dma_alloc_coherent(trans->dev, size,
				       &ptr->dma, GFP_KERNEL);
	if (!ptr->addr)
		return -ENOMEM;
	ptr->size = size;
	return 0;
}

static void iwl_pcie_free_dma_ptr(struct iwl_trans *trans,
				  struct iwl_dma_ptr *ptr)
{
	if (unlikely(!ptr->addr))
		return;

	dma_free_coherent(trans->dev, ptr->size, ptr->addr, ptr->dma);
	memset(ptr, 0, sizeof(*ptr));
}

static void iwl_pcie_txq_stuck_timer(unsigned long data)
{
	struct iwl_txq *txq = (void *)data;
	struct iwl_queue *q = &txq->q;
	struct iwl_trans_pcie *trans_pcie = txq->trans_pcie;
	struct iwl_trans *trans = iwl_trans_pcie_get_trans(trans_pcie);
	u32 scd_sram_addr = trans_pcie->scd_base_addr +
				SCD_TX_STTS_QUEUE_OFFSET(txq->q.id);
	u8 buf[16];
	int i;

	spin_lock(&txq->lock);
	/* check if triggered erroneously */
	if (txq->q.read_ptr == txq->q.write_ptr) {
		spin_unlock(&txq->lock);
		return;
	}
	spin_unlock(&txq->lock);

	IWL_ERR(trans, "Queue %d stuck for %u ms.\n", txq->q.id,
		jiffies_to_msecs(trans_pcie->wd_timeout));
	IWL_ERR(trans, "Current SW read_ptr %d write_ptr %d\n",
		txq->q.read_ptr, txq->q.write_ptr);

	iwl_trans_read_mem_bytes(trans, scd_sram_addr, buf, sizeof(buf));

	iwl_print_hex_error(trans, buf, sizeof(buf));

	for (i = 0; i < FH_TCSR_CHNL_NUM; i++)
		IWL_ERR(trans, "FH TRBs(%d) = 0x%08x\n", i,
			iwl_read_direct32(trans, FH_TX_TRB_REG(i)));

	for (i = 0; i < trans->cfg->base_params->num_of_queues; i++) {
		u32 status = iwl_read_prph(trans, SCD_QUEUE_STATUS_BITS(i));
		u8 fifo = (status >> SCD_QUEUE_STTS_REG_POS_TXF) & 0x7;
		bool active = !!(status & BIT(SCD_QUEUE_STTS_REG_POS_ACTIVE));
		u32 tbl_dw =
			iwl_trans_read_mem32(trans,
					     trans_pcie->scd_base_addr +
					     SCD_TRANS_TBL_OFFSET_QUEUE(i));

		if (i & 0x1)
			tbl_dw = (tbl_dw & 0xFFFF0000) >> 16;
		else
			tbl_dw = tbl_dw & 0x0000FFFF;

		IWL_ERR(trans,
			"Q %d is %sactive and mapped to fifo %d ra_tid 0x%04x [%d,%d]\n",
			i, active ? "" : "in", fifo, tbl_dw,
			iwl_read_prph(trans,
				      SCD_QUEUE_RDPTR(i)) & (txq->q.n_bd - 1),
			iwl_read_prph(trans, SCD_QUEUE_WRPTR(i)));
	}

	for (i = q->read_ptr; i != q->write_ptr;
	     i = iwl_queue_inc_wrap(i, q->n_bd)) {
		struct iwl_tx_cmd *tx_cmd =
			(struct iwl_tx_cmd *)txq->entries[i].cmd->payload;
		IWL_ERR(trans, "scratch %d = 0x%08x\n", i,
			get_unaligned_le32(&tx_cmd->scratch));
	}

	iwl_op_mode_nic_error(trans->op_mode);
}

/*
 * iwl_pcie_txq_update_byte_cnt_tbl - Set up entry in Tx byte-count array
 */
static void iwl_pcie_txq_update_byte_cnt_tbl(struct iwl_trans *trans,
					     struct iwl_txq *txq, u16 byte_cnt)
{
	struct iwlagn_scd_bc_tbl *scd_bc_tbl;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int write_ptr = txq->q.write_ptr;
	int txq_id = txq->q.id;
	u8 sec_ctl = 0;
	u8 sta_id = 0;
	u16 len = byte_cnt + IWL_TX_CRC_SIZE + IWL_TX_DELIMITER_SIZE;
	__le16 bc_ent;
	struct iwl_tx_cmd *tx_cmd =
		(void *) txq->entries[txq->q.write_ptr].cmd->payload;

	scd_bc_tbl = trans_pcie->scd_bc_tbls.addr;

	WARN_ON(len > 0xFFF || write_ptr >= TFD_QUEUE_SIZE_MAX);

	sta_id = tx_cmd->sta_id;
	sec_ctl = tx_cmd->sec_ctl;

	switch (sec_ctl & TX_CMD_SEC_MSK) {
	case TX_CMD_SEC_CCM:
		len += CCMP_MIC_LEN;
		break;
	case TX_CMD_SEC_TKIP:
		len += TKIP_ICV_LEN;
		break;
	case TX_CMD_SEC_WEP:
		len += WEP_IV_LEN + WEP_ICV_LEN;
		break;
	}

	if (trans_pcie->bc_table_dword)
		len = DIV_ROUND_UP(len, 4);

	bc_ent = cpu_to_le16(len | (sta_id << 12));

	scd_bc_tbl[txq_id].tfd_offset[write_ptr] = bc_ent;

	if (write_ptr < TFD_QUEUE_SIZE_BC_DUP)
		scd_bc_tbl[txq_id].
			tfd_offset[TFD_QUEUE_SIZE_MAX + write_ptr] = bc_ent;
}

static void iwl_pcie_txq_inval_byte_cnt_tbl(struct iwl_trans *trans,
					    struct iwl_txq *txq)
{
	struct iwl_trans_pcie *trans_pcie =
		IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwlagn_scd_bc_tbl *scd_bc_tbl = trans_pcie->scd_bc_tbls.addr;
	int txq_id = txq->q.id;
	int read_ptr = txq->q.read_ptr;
	u8 sta_id = 0;
	__le16 bc_ent;
	struct iwl_tx_cmd *tx_cmd =
		(void *)txq->entries[txq->q.read_ptr].cmd->payload;

	WARN_ON(read_ptr >= TFD_QUEUE_SIZE_MAX);

	if (txq_id != trans_pcie->cmd_queue)
		sta_id = tx_cmd->sta_id;

	bc_ent = cpu_to_le16(1 | (sta_id << 12));
	scd_bc_tbl[txq_id].tfd_offset[read_ptr] = bc_ent;

	if (read_ptr < TFD_QUEUE_SIZE_BC_DUP)
		scd_bc_tbl[txq_id].
			tfd_offset[TFD_QUEUE_SIZE_MAX + read_ptr] = bc_ent;
}

/*
 * iwl_pcie_txq_inc_wr_ptr - Send new write index to hardware
 */
void iwl_pcie_txq_inc_wr_ptr(struct iwl_trans *trans, struct iwl_txq *txq)
{
	u32 reg = 0;
	int txq_id = txq->q.id;

	if (txq->need_update == 0)
		return;

	if (trans->cfg->base_params->shadow_reg_enable) {
		/* shadow register enabled */
		iwl_write32(trans, HBUS_TARG_WRPTR,
			    txq->q.write_ptr | (txq_id << 8));
	} else {
		struct iwl_trans_pcie *trans_pcie =
			IWL_TRANS_GET_PCIE_TRANS(trans);
		/* if we're trying to save power */
		if (test_bit(STATUS_TPOWER_PMI, &trans_pcie->status)) {
			/* wake up nic if it's powered down ...
			 * uCode will wake up, and interrupt us again, so next
			 * time we'll skip this part. */
			reg = iwl_read32(trans, CSR_UCODE_DRV_GP1);

			if (reg & CSR_UCODE_DRV_GP1_BIT_MAC_SLEEP) {
				IWL_DEBUG_INFO(trans,
					"Tx queue %d requesting wakeup,"
					" GP1 = 0x%x\n", txq_id, reg);
				iwl_set_bit(trans, CSR_GP_CNTRL,
					CSR_GP_CNTRL_REG_FLAG_MAC_ACCESS_REQ);
				return;
			}

			IWL_DEBUG_TX(trans, "Q:%d WR: 0x%x\n", txq_id,
				     txq->q.write_ptr);

			iwl_write_direct32(trans, HBUS_TARG_WRPTR,
				     txq->q.write_ptr | (txq_id << 8));

		/*
		 * else not in power-save mode,
		 * uCode will never sleep when we're
		 * trying to tx (during RFKILL, we're not trying to tx).
		 */
		} else
			iwl_write32(trans, HBUS_TARG_WRPTR,
				    txq->q.write_ptr | (txq_id << 8));
	}
	txq->need_update = 0;
}

static inline dma_addr_t iwl_pcie_tfd_tb_get_addr(struct iwl_tfd *tfd, u8 idx)
{
	struct iwl_tfd_tb *tb = &tfd->tbs[idx];

	dma_addr_t addr = get_unaligned_le32(&tb->lo);
	if (sizeof(dma_addr_t) > sizeof(u32))
		addr |=
		((dma_addr_t)(le16_to_cpu(tb->hi_n_len) & 0xF) << 16) << 16;

	return addr;
}

static inline u16 iwl_pcie_tfd_tb_get_len(struct iwl_tfd *tfd, u8 idx)
{
	struct iwl_tfd_tb *tb = &tfd->tbs[idx];

	return le16_to_cpu(tb->hi_n_len) >> 4;
}

static inline void iwl_pcie_tfd_set_tb(struct iwl_tfd *tfd, u8 idx,
				       dma_addr_t addr, u16 len)
{
	struct iwl_tfd_tb *tb = &tfd->tbs[idx];
	u16 hi_n_len = len << 4;

	put_unaligned_le32(addr, &tb->lo);
	if (sizeof(dma_addr_t) > sizeof(u32))
		hi_n_len |= ((addr >> 16) >> 16) & 0xF;

	tb->hi_n_len = cpu_to_le16(hi_n_len);

	tfd->num_tbs = idx + 1;
}

static inline u8 iwl_pcie_tfd_get_num_tbs(struct iwl_tfd *tfd)
{
	return tfd->num_tbs & 0x1f;
}

static void iwl_pcie_tfd_unmap(struct iwl_trans *trans,
			       struct iwl_cmd_meta *meta, struct iwl_tfd *tfd,
			       enum dma_data_direction dma_dir)
{
	int i;
	int num_tbs;

	/* Sanity check on number of chunks */
	num_tbs = iwl_pcie_tfd_get_num_tbs(tfd);

	if (num_tbs >= IWL_NUM_OF_TBS) {
		IWL_ERR(trans, "Too many chunks: %i\n", num_tbs);
		/* @todo issue fatal error, it is quite serious situation */
		return;
	}

	/* Unmap tx_cmd */
	if (num_tbs)
		dma_unmap_single(trans->dev,
				dma_unmap_addr(meta, mapping),
				dma_unmap_len(meta, len),
				DMA_BIDIRECTIONAL);

	/* Unmap chunks, if any. */
	for (i = 1; i < num_tbs; i++)
		dma_unmap_single(trans->dev, iwl_pcie_tfd_tb_get_addr(tfd, i),
				 iwl_pcie_tfd_tb_get_len(tfd, i), dma_dir);

	tfd->num_tbs = 0;
}

/*
 * iwl_pcie_txq_free_tfd - Free all chunks referenced by TFD [txq->q.read_ptr]
 * @trans - transport private data
 * @txq - tx queue
 * @dma_dir - the direction of the DMA mapping
 *
 * Does NOT advance any TFD circular buffer read/write indexes
 * Does NOT free the TFD itself (which is within circular buffer)
 */
static void iwl_pcie_txq_free_tfd(struct iwl_trans *trans, struct iwl_txq *txq,
				  enum dma_data_direction dma_dir)
{
	struct iwl_tfd *tfd_tmp = txq->tfds;

	/* rd_ptr is bounded by n_bd and idx is bounded by n_window */
	int rd_ptr = txq->q.read_ptr;
	int idx = get_cmd_index(&txq->q, rd_ptr);

	lockdep_assert_held(&txq->lock);

	/* We have only q->n_window txq->entries, but we use q->n_bd tfds */
	iwl_pcie_tfd_unmap(trans, &txq->entries[idx].meta, &tfd_tmp[rd_ptr],
			   dma_dir);

	/* free SKB */
	if (txq->entries) {
		struct sk_buff *skb;

		skb = txq->entries[idx].skb;

		/* Can be called from irqs-disabled context
		 * If skb is not NULL, it means that the whole queue is being
		 * freed and that the queue is not empty - free the skb
		 */
		if (skb) {
			iwl_op_mode_free_skb(trans->op_mode, skb);
			txq->entries[idx].skb = NULL;
		}
	}
}

static int iwl_pcie_txq_build_tfd(struct iwl_trans *trans, struct iwl_txq *txq,
				  dma_addr_t addr, u16 len, u8 reset)
{
	struct iwl_queue *q;
	struct iwl_tfd *tfd, *tfd_tmp;
	u32 num_tbs;

	q = &txq->q;
	tfd_tmp = txq->tfds;
	tfd = &tfd_tmp[q->write_ptr];

	if (reset)
		memset(tfd, 0, sizeof(*tfd));

	num_tbs = iwl_pcie_tfd_get_num_tbs(tfd);

	/* Each TFD can point to a maximum 20 Tx buffers */
	if (num_tbs >= IWL_NUM_OF_TBS) {
		IWL_ERR(trans, "Error can not send more than %d chunks\n",
			IWL_NUM_OF_TBS);
		return -EINVAL;
	}

	if (WARN_ON(addr & ~DMA_BIT_MASK(36)))
		return -EINVAL;

	if (unlikely(addr & ~IWL_TX_DMA_MASK))
		IWL_ERR(trans, "Unaligned address = %llx\n",
			(unsigned long long)addr);

	iwl_pcie_tfd_set_tb(tfd, num_tbs, addr, len);

	return 0;
}

static int iwl_pcie_txq_alloc(struct iwl_trans *trans,
			       struct iwl_txq *txq, int slots_num,
			       u32 txq_id)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	size_t tfd_sz = sizeof(struct iwl_tfd) * TFD_QUEUE_SIZE_MAX;
	int i;

	if (WARN_ON(txq->entries || txq->tfds))
		return -EINVAL;

	setup_timer(&txq->stuck_timer, iwl_pcie_txq_stuck_timer,
		    (unsigned long)txq);
	txq->trans_pcie = trans_pcie;

	txq->q.n_window = slots_num;

	txq->entries = kcalloc(slots_num,
			       sizeof(struct iwl_pcie_txq_entry),
			       GFP_KERNEL);

	if (!txq->entries)
		goto error;

	if (txq_id == trans_pcie->cmd_queue)
		for (i = 0; i < slots_num; i++) {
			txq->entries[i].cmd =
				kmalloc(sizeof(struct iwl_device_cmd),
					GFP_KERNEL);
			if (!txq->entries[i].cmd)
				goto error;
		}

	/* Circular buffer of transmit frame descriptors (TFDs),
	 * shared with device */
	txq->tfds = dma_alloc_coherent(trans->dev, tfd_sz,
				       &txq->q.dma_addr, GFP_KERNEL);
	if (!txq->tfds) {
		IWL_ERR(trans, "dma_alloc_coherent(%zd) failed\n", tfd_sz);
		goto error;
	}
	txq->q.id = txq_id;

	return 0;
error:
	if (txq->entries && txq_id == trans_pcie->cmd_queue)
		for (i = 0; i < slots_num; i++)
			kfree(txq->entries[i].cmd);
	kfree(txq->entries);
	txq->entries = NULL;

	return -ENOMEM;

}

static int iwl_pcie_txq_init(struct iwl_trans *trans, struct iwl_txq *txq,
			      int slots_num, u32 txq_id)
{
	int ret;

	txq->need_update = 0;

	/* TFD_QUEUE_SIZE_MAX must be power-of-two size, otherwise
	 * iwl_queue_inc_wrap and iwl_queue_dec_wrap are broken. */
	BUILD_BUG_ON(TFD_QUEUE_SIZE_MAX & (TFD_QUEUE_SIZE_MAX - 1));

	/* Initialize queue's high/low-water marks, and head/tail indexes */
	ret = iwl_queue_init(&txq->q, TFD_QUEUE_SIZE_MAX, slots_num,
			txq_id);
	if (ret)
		return ret;

	spin_lock_init(&txq->lock);

	/*
	 * Tell nic where to find circular buffer of Tx Frame Descriptors for
	 * given Tx queue, and enable the DMA channel used for that queue.
	 * Circular buffer (TFD queue in DRAM) physical base address */
	iwl_write_direct32(trans, FH_MEM_CBBC_QUEUE(txq_id),
			   txq->q.dma_addr >> 8);

	return 0;
}

/*
 * iwl_pcie_txq_unmap -  Unmap any remaining DMA mappings and free skb's
 */
static void iwl_pcie_txq_unmap(struct iwl_trans *trans, int txq_id)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq = &trans_pcie->txq[txq_id];
	struct iwl_queue *q = &txq->q;
	enum dma_data_direction dma_dir;

	if (!q->n_bd)
		return;

	/* In the command queue, all the TBs are mapped as BIDI
	 * so unmap them as such.
	 */
	if (txq_id == trans_pcie->cmd_queue)
		dma_dir = DMA_BIDIRECTIONAL;
	else
		dma_dir = DMA_TO_DEVICE;

	spin_lock_bh(&txq->lock);
	while (q->write_ptr != q->read_ptr) {
		iwl_pcie_txq_free_tfd(trans, txq, dma_dir);
		q->read_ptr = iwl_queue_inc_wrap(q->read_ptr, q->n_bd);
	}
	spin_unlock_bh(&txq->lock);
}

/*
 * iwl_pcie_txq_free - Deallocate DMA queue.
 * @txq: Transmit queue to deallocate.
 *
 * Empty queue by removing and destroying all BD's.
 * Free all buffers.
 * 0-fill, but do not free "txq" descriptor structure.
 */
static void iwl_pcie_txq_free(struct iwl_trans *trans, int txq_id)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq = &trans_pcie->txq[txq_id];
	struct device *dev = trans->dev;
	int i;

	if (WARN_ON(!txq))
		return;

	iwl_pcie_txq_unmap(trans, txq_id);

	/* De-alloc array of command/tx buffers */
	if (txq_id == trans_pcie->cmd_queue)
		for (i = 0; i < txq->q.n_window; i++) {
			kfree(txq->entries[i].cmd);
			kfree(txq->entries[i].copy_cmd);
			kfree(txq->entries[i].free_buf);
		}

	/* De-alloc circular buffer of TFDs */
	if (txq->q.n_bd) {
		dma_free_coherent(dev, sizeof(struct iwl_tfd) *
				  txq->q.n_bd, txq->tfds, txq->q.dma_addr);
		txq->q.dma_addr = 0;
	}

	kfree(txq->entries);
	txq->entries = NULL;

	del_timer_sync(&txq->stuck_timer);

	/* 0-fill queue descriptor structure */
	memset(txq, 0, sizeof(*txq));
}

/*
 * Activate/Deactivate Tx DMA/FIFO channels according tx fifos mask
 */
static void iwl_pcie_txq_set_sched(struct iwl_trans *trans, u32 mask)
{
	struct iwl_trans_pcie __maybe_unused *trans_pcie =
		IWL_TRANS_GET_PCIE_TRANS(trans);

	iwl_write_prph(trans, SCD_TXFACT, mask);
}

void iwl_pcie_tx_start(struct iwl_trans *trans, u32 scd_base_addr)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int nq = trans->cfg->base_params->num_of_queues;
	int chan;
	u32 reg_val;
	int clear_dwords = (SCD_TRANS_TBL_OFFSET_QUEUE(nq) -
				SCD_CONTEXT_MEM_LOWER_BOUND) / sizeof(u32);

	/* make sure all queue are not stopped/used */
	memset(trans_pcie->queue_stopped, 0, sizeof(trans_pcie->queue_stopped));
	memset(trans_pcie->queue_used, 0, sizeof(trans_pcie->queue_used));

	trans_pcie->scd_base_addr =
		iwl_read_prph(trans, SCD_SRAM_BASE_ADDR);

	WARN_ON(scd_base_addr != 0 &&
		scd_base_addr != trans_pcie->scd_base_addr);

	/* reset context data, TX status and translation data */
	iwl_trans_write_mem(trans, trans_pcie->scd_base_addr +
				   SCD_CONTEXT_MEM_LOWER_BOUND,
			    NULL, clear_dwords);

	iwl_write_prph(trans, SCD_DRAM_BASE_ADDR,
		       trans_pcie->scd_bc_tbls.dma >> 10);

	/* The chain extension of the SCD doesn't work well. This feature is
	 * enabled by default by the HW, so we need to disable it manually.
	 */
	iwl_write_prph(trans, SCD_CHAINEXT_EN, 0);

	iwl_trans_ac_txq_enable(trans, trans_pcie->cmd_queue,
				trans_pcie->cmd_fifo);

	/* Activate all Tx DMA/FIFO channels */
	iwl_pcie_txq_set_sched(trans, IWL_MASK(0, 7));

	/* Enable DMA channel */
	for (chan = 0; chan < FH_TCSR_CHNL_NUM; chan++)
		iwl_write_direct32(trans, FH_TCSR_CHNL_TX_CONFIG_REG(chan),
				   FH_TCSR_TX_CONFIG_REG_VAL_DMA_CHNL_ENABLE |
				   FH_TCSR_TX_CONFIG_REG_VAL_DMA_CREDIT_ENABLE);

	/* Update FH chicken bits */
	reg_val = iwl_read_direct32(trans, FH_TX_CHICKEN_BITS_REG);
	iwl_write_direct32(trans, FH_TX_CHICKEN_BITS_REG,
			   reg_val | FH_TX_CHICKEN_BITS_SCD_AUTO_RETRY_EN);

	/* Enable L1-Active */
	iwl_clear_bits_prph(trans, APMG_PCIDEV_STT_REG,
			    APMG_PCIDEV_STT_VAL_L1_ACT_DIS);
}

void iwl_trans_pcie_tx_reset(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int txq_id;

	for (txq_id = 0; txq_id < trans->cfg->base_params->num_of_queues;
	     txq_id++) {
		struct iwl_txq *txq = &trans_pcie->txq[txq_id];

		iwl_write_direct32(trans, FH_MEM_CBBC_QUEUE(txq_id),
				   txq->q.dma_addr >> 8);
		iwl_pcie_txq_unmap(trans, txq_id);
		txq->q.read_ptr = 0;
		txq->q.write_ptr = 0;
	}

	/* Tell NIC where to find the "keep warm" buffer */
	iwl_write_direct32(trans, FH_KW_MEM_ADDR_REG,
			   trans_pcie->kw.dma >> 4);

	iwl_pcie_tx_start(trans, trans_pcie->scd_base_addr);
}

/*
 * iwl_pcie_tx_stop - Stop all Tx DMA channels
 */
int iwl_pcie_tx_stop(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ch, txq_id, ret;
	unsigned long flags;

	/* Turn off all Tx DMA fifos */
	spin_lock_irqsave(&trans_pcie->irq_lock, flags);

	iwl_pcie_txq_set_sched(trans, 0);

	/* Stop each Tx DMA channel, and wait for it to be idle */
	for (ch = 0; ch < FH_TCSR_CHNL_NUM; ch++) {
		iwl_write_direct32(trans,
				   FH_TCSR_CHNL_TX_CONFIG_REG(ch), 0x0);
		ret = iwl_poll_direct_bit(trans, FH_TSSR_TX_STATUS_REG,
			FH_TSSR_TX_STATUS_REG_MSK_CHNL_IDLE(ch), 1000);
		if (ret < 0)
			IWL_ERR(trans,
				"Failing on timeout while stopping DMA channel %d [0x%08x]\n",
				ch,
				iwl_read_direct32(trans,
						  FH_TSSR_TX_STATUS_REG));
	}
	spin_unlock_irqrestore(&trans_pcie->irq_lock, flags);

	if (!trans_pcie->txq) {
		IWL_WARN(trans,
			 "Stopping tx queues that aren't allocated...\n");
		return 0;
	}

	/* Unmap DMA from host system and free skb's */
	for (txq_id = 0; txq_id < trans->cfg->base_params->num_of_queues;
	     txq_id++)
		iwl_pcie_txq_unmap(trans, txq_id);

	return 0;
}

/*
 * iwl_trans_tx_free - Free TXQ Context
 *
 * Destroy all TX DMA queues and structures
 */
void iwl_pcie_tx_free(struct iwl_trans *trans)
{
	int txq_id;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	/* Tx queues */
	if (trans_pcie->txq) {
		for (txq_id = 0;
		     txq_id < trans->cfg->base_params->num_of_queues; txq_id++)
			iwl_pcie_txq_free(trans, txq_id);
	}

	kfree(trans_pcie->txq);
	trans_pcie->txq = NULL;

	iwl_pcie_free_dma_ptr(trans, &trans_pcie->kw);

	iwl_pcie_free_dma_ptr(trans, &trans_pcie->scd_bc_tbls);
}

/*
 * iwl_pcie_tx_alloc - allocate TX context
 * Allocate all Tx DMA structures and initialize them
 */
static int iwl_pcie_tx_alloc(struct iwl_trans *trans)
{
	int ret;
	int txq_id, slots_num;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	u16 scd_bc_tbls_size = trans->cfg->base_params->num_of_queues *
			sizeof(struct iwlagn_scd_bc_tbl);

	/*It is not allowed to alloc twice, so warn when this happens.
	 * We cannot rely on the previous allocation, so free and fail */
	if (WARN_ON(trans_pcie->txq)) {
		ret = -EINVAL;
		goto error;
	}

	ret = iwl_pcie_alloc_dma_ptr(trans, &trans_pcie->scd_bc_tbls,
				   scd_bc_tbls_size);
	if (ret) {
		IWL_ERR(trans, "Scheduler BC Table allocation failed\n");
		goto error;
	}

	/* Alloc keep-warm buffer */
	ret = iwl_pcie_alloc_dma_ptr(trans, &trans_pcie->kw, IWL_KW_SIZE);
	if (ret) {
		IWL_ERR(trans, "Keep Warm allocation failed\n");
		goto error;
	}

	trans_pcie->txq = kcalloc(trans->cfg->base_params->num_of_queues,
				  sizeof(struct iwl_txq), GFP_KERNEL);
	if (!trans_pcie->txq) {
		IWL_ERR(trans, "Not enough memory for txq\n");
		ret = ENOMEM;
		goto error;
	}

	/* Alloc and init all Tx queues, including the command queue (#4/#9) */
	for (txq_id = 0; txq_id < trans->cfg->base_params->num_of_queues;
	     txq_id++) {
		slots_num = (txq_id == trans_pcie->cmd_queue) ?
					TFD_CMD_SLOTS : TFD_TX_CMD_SLOTS;
		ret = iwl_pcie_txq_alloc(trans, &trans_pcie->txq[txq_id],
					  slots_num, txq_id);
		if (ret) {
			IWL_ERR(trans, "Tx %d queue alloc failed\n", txq_id);
			goto error;
		}
	}

	return 0;

error:
	iwl_pcie_tx_free(trans);

	return ret;
}
int iwl_pcie_tx_init(struct iwl_trans *trans)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ret;
	int txq_id, slots_num;
	unsigned long flags;
	bool alloc = false;

	if (!trans_pcie->txq) {
		ret = iwl_pcie_tx_alloc(trans);
		if (ret)
			goto error;
		alloc = true;
	}

	spin_lock_irqsave(&trans_pcie->irq_lock, flags);

	/* Turn off all Tx DMA fifos */
	iwl_write_prph(trans, SCD_TXFACT, 0);

	/* Tell NIC where to find the "keep warm" buffer */
	iwl_write_direct32(trans, FH_KW_MEM_ADDR_REG,
			   trans_pcie->kw.dma >> 4);

	spin_unlock_irqrestore(&trans_pcie->irq_lock, flags);

	/* Alloc and init all Tx queues, including the command queue (#4/#9) */
	for (txq_id = 0; txq_id < trans->cfg->base_params->num_of_queues;
	     txq_id++) {
		slots_num = (txq_id == trans_pcie->cmd_queue) ?
					TFD_CMD_SLOTS : TFD_TX_CMD_SLOTS;
		ret = iwl_pcie_txq_init(trans, &trans_pcie->txq[txq_id],
					 slots_num, txq_id);
		if (ret) {
			IWL_ERR(trans, "Tx %d queue init failed\n", txq_id);
			goto error;
		}
	}

	return 0;
error:
	/*Upon error, free only if we allocated something */
	if (alloc)
		iwl_pcie_tx_free(trans);
	return ret;
}

static inline void iwl_pcie_txq_progress(struct iwl_trans_pcie *trans_pcie,
					   struct iwl_txq *txq)
{
	if (!trans_pcie->wd_timeout)
		return;

	/*
	 * if empty delete timer, otherwise move timer forward
	 * since we're making progress on this queue
	 */
	if (txq->q.read_ptr == txq->q.write_ptr)
		del_timer(&txq->stuck_timer);
	else
		mod_timer(&txq->stuck_timer, jiffies + trans_pcie->wd_timeout);
}

/* Frees buffers until index _not_ inclusive */
void iwl_trans_pcie_reclaim(struct iwl_trans *trans, int txq_id, int ssn,
			    struct sk_buff_head *skbs)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq = &trans_pcie->txq[txq_id];
	/* n_bd is usually 256 => n_bd - 1 = 0xff */
	int tfd_num = ssn & (txq->q.n_bd - 1);
	struct iwl_queue *q = &txq->q;
	int last_to_free;

	/* This function is not meant to release cmd queue*/
	if (WARN_ON(txq_id == trans_pcie->cmd_queue))
		return;

	spin_lock_bh(&txq->lock);

	if (txq->q.read_ptr == tfd_num)
		goto out;

	IWL_DEBUG_TX_REPLY(trans, "[Q %d] %d -> %d (%d)\n",
			   txq_id, txq->q.read_ptr, tfd_num, ssn);

	/*Since we free until index _not_ inclusive, the one before index is
	 * the last we will free. This one must be used */
	last_to_free = iwl_queue_dec_wrap(tfd_num, q->n_bd);

	if (!iwl_queue_used(q, last_to_free)) {
		IWL_ERR(trans,
			"%s: Read index for DMA queue txq id (%d), last_to_free %d is out of range [0-%d] %d %d.\n",
			__func__, txq_id, last_to_free, q->n_bd,
			q->write_ptr, q->read_ptr);
		goto out;
	}

	if (WARN_ON(!skb_queue_empty(skbs)))
		goto out;

	for (;
	     q->read_ptr != tfd_num;
	     q->read_ptr = iwl_queue_inc_wrap(q->read_ptr, q->n_bd)) {

		if (WARN_ON_ONCE(txq->entries[txq->q.read_ptr].skb == NULL))
			continue;

		__skb_queue_tail(skbs, txq->entries[txq->q.read_ptr].skb);

		txq->entries[txq->q.read_ptr].skb = NULL;

		iwl_pcie_txq_inval_byte_cnt_tbl(trans, txq);

		iwl_pcie_txq_free_tfd(trans, txq, DMA_TO_DEVICE);
	}

	iwl_pcie_txq_progress(trans_pcie, txq);

	if (iwl_queue_space(&txq->q) > txq->q.low_mark)
		iwl_wake_queue(trans, txq);
out:
	spin_unlock_bh(&txq->lock);
}

/*
 * iwl_pcie_cmdq_reclaim - Reclaim TX command queue entries already Tx'd
 *
 * When FW advances 'R' index, all entries between old and new 'R' index
 * need to be reclaimed. As result, some free space forms.  If there is
 * enough free space (> low mark), wake the stack that feeds us.
 */
static void iwl_pcie_cmdq_reclaim(struct iwl_trans *trans, int txq_id, int idx)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq = &trans_pcie->txq[txq_id];
	struct iwl_queue *q = &txq->q;
	int nfreed = 0;

	lockdep_assert_held(&txq->lock);

	if ((idx >= q->n_bd) || (!iwl_queue_used(q, idx))) {
		IWL_ERR(trans,
			"%s: Read index for DMA queue txq id (%d), index %d is out of range [0-%d] %d %d.\n",
			__func__, txq_id, idx, q->n_bd,
			q->write_ptr, q->read_ptr);
		return;
	}

	for (idx = iwl_queue_inc_wrap(idx, q->n_bd); q->read_ptr != idx;
	     q->read_ptr = iwl_queue_inc_wrap(q->read_ptr, q->n_bd)) {

		if (nfreed++ > 0) {
			IWL_ERR(trans, "HCMD skipped: index (%d) %d %d\n",
				idx, q->write_ptr, q->read_ptr);
			iwl_op_mode_nic_error(trans->op_mode);
		}
	}

	iwl_pcie_txq_progress(trans_pcie, txq);
}

static int iwl_pcie_txq_set_ratid_map(struct iwl_trans *trans, u16 ra_tid,
				 u16 txq_id)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u32 tbl_dw_addr;
	u32 tbl_dw;
	u16 scd_q2ratid;

	scd_q2ratid = ra_tid & SCD_QUEUE_RA_TID_MAP_RATID_MSK;

	tbl_dw_addr = trans_pcie->scd_base_addr +
			SCD_TRANS_TBL_OFFSET_QUEUE(txq_id);

	tbl_dw = iwl_trans_read_mem32(trans, tbl_dw_addr);

	if (txq_id & 0x1)
		tbl_dw = (scd_q2ratid << 16) | (tbl_dw & 0x0000FFFF);
	else
		tbl_dw = scd_q2ratid | (tbl_dw & 0xFFFF0000);

	iwl_trans_write_mem32(trans, tbl_dw_addr, tbl_dw);

	return 0;
}

static inline void iwl_pcie_txq_set_inactive(struct iwl_trans *trans,
					     u16 txq_id)
{
	/* Simply stop the queue, but don't change any configuration;
	 * the SCD_ACT_EN bit is the write-enable mask for the ACTIVE bit. */
	iwl_write_prph(trans,
		SCD_QUEUE_STATUS_BITS(txq_id),
		(0 << SCD_QUEUE_STTS_REG_POS_ACTIVE)|
		(1 << SCD_QUEUE_STTS_REG_POS_SCD_ACT_EN));
}

void iwl_trans_pcie_txq_enable(struct iwl_trans *trans, int txq_id, int fifo,
			       int sta_id, int tid, int frame_limit, u16 ssn)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	if (test_and_set_bit(txq_id, trans_pcie->queue_used))
		WARN_ONCE(1, "queue %d already used - expect issues", txq_id);

	/* Stop this Tx queue before configuring it */
	iwl_pcie_txq_set_inactive(trans, txq_id);

	/* Set this queue as a chain-building queue unless it is CMD queue */
	if (txq_id != trans_pcie->cmd_queue)
		iwl_set_bits_prph(trans, SCD_QUEUECHAIN_SEL, BIT(txq_id));

	/* If this queue is mapped to a certain station: it is an AGG queue */
	if (sta_id != IWL_INVALID_STATION) {
		u16 ra_tid = BUILD_RAxTID(sta_id, tid);

		/* Map receiver-address / traffic-ID to this queue */
		iwl_pcie_txq_set_ratid_map(trans, ra_tid, txq_id);

		/* enable aggregations for the queue */
		iwl_set_bits_prph(trans, SCD_AGGR_SEL, BIT(txq_id));
	} else {
		/*
		 * disable aggregations for the queue, this will also make the
		 * ra_tid mapping configuration irrelevant since it is now a
		 * non-AGG queue.
		 */
		iwl_clear_bits_prph(trans, SCD_AGGR_SEL, BIT(txq_id));
	}

	/* Place first TFD at index corresponding to start sequence number.
	 * Assumes that ssn_idx is valid (!= 0xFFF) */
	trans_pcie->txq[txq_id].q.read_ptr = (ssn & 0xff);
	trans_pcie->txq[txq_id].q.write_ptr = (ssn & 0xff);

	iwl_write_direct32(trans, HBUS_TARG_WRPTR,
			   (ssn & 0xff) | (txq_id << 8));
	iwl_write_prph(trans, SCD_QUEUE_RDPTR(txq_id), ssn);

	/* Set up Tx window size and frame limit for this queue */
	iwl_trans_write_mem32(trans, trans_pcie->scd_base_addr +
			SCD_CONTEXT_QUEUE_OFFSET(txq_id), 0);
	iwl_trans_write_mem32(trans, trans_pcie->scd_base_addr +
			SCD_CONTEXT_QUEUE_OFFSET(txq_id) + sizeof(u32),
			((frame_limit << SCD_QUEUE_CTX_REG2_WIN_SIZE_POS) &
				SCD_QUEUE_CTX_REG2_WIN_SIZE_MSK) |
			((frame_limit << SCD_QUEUE_CTX_REG2_FRAME_LIMIT_POS) &
				SCD_QUEUE_CTX_REG2_FRAME_LIMIT_MSK));

	/* Set up Status area in SRAM, map to Tx DMA/FIFO, activate the queue */
	iwl_write_prph(trans, SCD_QUEUE_STATUS_BITS(txq_id),
		       (1 << SCD_QUEUE_STTS_REG_POS_ACTIVE) |
		       (fifo << SCD_QUEUE_STTS_REG_POS_TXF) |
		       (1 << SCD_QUEUE_STTS_REG_POS_WSL) |
		       SCD_QUEUE_STTS_REG_MSK);
	IWL_DEBUG_TX_QUEUES(trans, "Activate queue %d on FIFO %d WrPtr: %d\n",
			    txq_id, fifo, ssn & 0xff);
}

void iwl_trans_pcie_txq_disable(struct iwl_trans *trans, int txq_id)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	u32 stts_addr = trans_pcie->scd_base_addr +
			SCD_TX_STTS_QUEUE_OFFSET(txq_id);
	static const u32 zero_val[4] = {};

	if (!test_and_clear_bit(txq_id, trans_pcie->queue_used)) {
		WARN_ONCE(1, "queue %d not used", txq_id);
		return;
	}

	iwl_pcie_txq_set_inactive(trans, txq_id);

	iwl_trans_write_mem(trans, stts_addr, (void *)zero_val,
			    ARRAY_SIZE(zero_val));

	iwl_pcie_txq_unmap(trans, txq_id);

	IWL_DEBUG_TX_QUEUES(trans, "Deactivate queue %d\n", txq_id);
}

/*************** HOST COMMAND QUEUE FUNCTIONS   *****/

/*
 * iwl_pcie_enqueue_hcmd - enqueue a uCode command
 * @priv: device private data point
 * @cmd: a point to the ucode command structure
 *
 * The function returns < 0 values to indicate the operation is
 * failed. On success, it turns the index (> 0) of command in the
 * command queue.
 */
static int iwl_pcie_enqueue_hcmd(struct iwl_trans *trans,
				 struct iwl_host_cmd *cmd)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq = &trans_pcie->txq[trans_pcie->cmd_queue];
	struct iwl_queue *q = &txq->q;
	struct iwl_device_cmd *out_cmd;
	struct iwl_cmd_meta *out_meta;
	void *dup_buf = NULL;
	dma_addr_t phys_addr;
	int idx;
	u16 copy_size, cmd_size, dma_size;
	bool had_nocopy = false;
	int i;
	u32 cmd_pos;
	const u8 *cmddata[IWL_MAX_CMD_TFDS];
	u16 cmdlen[IWL_MAX_CMD_TFDS];

	copy_size = sizeof(out_cmd->hdr);
	cmd_size = sizeof(out_cmd->hdr);

	/* need one for the header if the first is NOCOPY */
	BUILD_BUG_ON(IWL_MAX_CMD_TFDS > IWL_NUM_OF_TBS - 1);

	for (i = 0; i < IWL_MAX_CMD_TFDS; i++) {
		cmddata[i] = cmd->data[i];
		cmdlen[i] = cmd->len[i];

		if (!cmd->len[i])
			continue;

		/* need at least IWL_HCMD_MIN_COPY_SIZE copied */
		if (copy_size < IWL_HCMD_MIN_COPY_SIZE) {
			int copy = IWL_HCMD_MIN_COPY_SIZE - copy_size;

			if (copy > cmdlen[i])
				copy = cmdlen[i];
			cmdlen[i] -= copy;
			cmddata[i] += copy;
			copy_size += copy;
		}

		if (cmd->dataflags[i] & IWL_HCMD_DFL_NOCOPY) {
			had_nocopy = true;
			if (WARN_ON(cmd->dataflags[i] & IWL_HCMD_DFL_DUP)) {
				idx = -EINVAL;
				goto free_dup_buf;
			}
		} else if (cmd->dataflags[i] & IWL_HCMD_DFL_DUP) {
			/*
			 * This is also a chunk that isn't copied
			 * to the static buffer so set had_nocopy.
			 */
			had_nocopy = true;

			/* only allowed once */
			if (WARN_ON(dup_buf)) {
				idx = -EINVAL;
				goto free_dup_buf;
			}

			dup_buf = kmemdup(cmddata[i], cmdlen[i],
					  GFP_ATOMIC);
			if (!dup_buf)
				return -ENOMEM;
		} else {
			/* NOCOPY must not be followed by normal! */
			if (WARN_ON(had_nocopy)) {
				idx = -EINVAL;
				goto free_dup_buf;
			}
			copy_size += cmdlen[i];
		}
		cmd_size += cmd->len[i];
	}

	/*
	 * If any of the command structures end up being larger than
	 * the TFD_MAX_PAYLOAD_SIZE and they aren't dynamically
	 * allocated into separate TFDs, then we will need to
	 * increase the size of the buffers.
	 */
	if (WARN(copy_size > TFD_MAX_PAYLOAD_SIZE,
		 "Command %s (%#x) is too large (%d bytes)\n",
		 get_cmd_string(trans_pcie, cmd->id), cmd->id, copy_size)) {
		idx = -EINVAL;
		goto free_dup_buf;
	}

	spin_lock_bh(&txq->lock);

	if (iwl_queue_space(q) < ((cmd->flags & CMD_ASYNC) ? 2 : 1)) {
		spin_unlock_bh(&txq->lock);

		IWL_ERR(trans, "No space in command queue\n");
		iwl_op_mode_cmd_queue_full(trans->op_mode);
		idx = -ENOSPC;
		goto free_dup_buf;
	}

	idx = get_cmd_index(q, q->write_ptr);
	out_cmd = txq->entries[idx].cmd;
	out_meta = &txq->entries[idx].meta;

	memset(out_meta, 0, sizeof(*out_meta));	/* re-initialize to NULL */
	if (cmd->flags & CMD_WANT_SKB)
		out_meta->source = cmd;

	/* set up the header */

	out_cmd->hdr.cmd = cmd->id;
	out_cmd->hdr.flags = 0;
	out_cmd->hdr.sequence =
		cpu_to_le16(QUEUE_TO_SEQ(trans_pcie->cmd_queue) |
					 INDEX_TO_SEQ(q->write_ptr));

	/* and copy the data that needs to be copied */
	cmd_pos = offsetof(struct iwl_device_cmd, payload);
	copy_size = sizeof(out_cmd->hdr);
	for (i = 0; i < IWL_MAX_CMD_TFDS; i++) {
		int copy = 0;

		if (!cmd->len)
			continue;

		/* need at least IWL_HCMD_MIN_COPY_SIZE copied */
		if (copy_size < IWL_HCMD_MIN_COPY_SIZE) {
			copy = IWL_HCMD_MIN_COPY_SIZE - copy_size;

			if (copy > cmd->len[i])
				copy = cmd->len[i];
		}

		/* copy everything if not nocopy/dup */
		if (!(cmd->dataflags[i] & (IWL_HCMD_DFL_NOCOPY |
					   IWL_HCMD_DFL_DUP)))
			copy = cmd->len[i];

		if (copy) {
			memcpy((u8 *)out_cmd + cmd_pos, cmd->data[i], copy);
			cmd_pos += copy;
			copy_size += copy;
		}
	}

	WARN_ON_ONCE(txq->entries[idx].copy_cmd);

	/*
	 * since out_cmd will be the source address of the FH, it will write
	 * the retry count there. So when the user needs to receivce the HCMD
	 * that corresponds to the response in the response handler, it needs
	 * to set CMD_WANT_HCMD.
	 */
	if (cmd->flags & CMD_WANT_HCMD) {
		txq->entries[idx].copy_cmd =
			kmemdup(out_cmd, cmd_pos, GFP_ATOMIC);
		if (unlikely(!txq->entries[idx].copy_cmd)) {
			idx = -ENOMEM;
			goto out;
		}
	}

	IWL_DEBUG_HC(trans,
		     "Sending command %s (#%x), seq: 0x%04X, %d bytes at %d[%d]:%d\n",
		     get_cmd_string(trans_pcie, out_cmd->hdr.cmd),
		     out_cmd->hdr.cmd, le16_to_cpu(out_cmd->hdr.sequence),
		     cmd_size, q->write_ptr, idx, trans_pcie->cmd_queue);

	/*
	 * If the entire command is smaller than IWL_HCMD_MIN_COPY_SIZE, we must
	 * still map at least that many bytes for the hardware to write back to.
	 * We have enough space, so that's not a problem.
	 */
	dma_size = max_t(u16, copy_size, IWL_HCMD_MIN_COPY_SIZE);

	phys_addr = dma_map_single(trans->dev, &out_cmd->hdr, dma_size,
				   DMA_BIDIRECTIONAL);
	if (unlikely(dma_mapping_error(trans->dev, phys_addr))) {
		idx = -ENOMEM;
		goto out;
	}

	dma_unmap_addr_set(out_meta, mapping, phys_addr);
	dma_unmap_len_set(out_meta, len, dma_size);

	iwl_pcie_txq_build_tfd(trans, txq, phys_addr, copy_size, 1);

	/* map the remaining (adjusted) nocopy/dup fragments */
	for (i = 0; i < IWL_MAX_CMD_TFDS; i++) {
		const void *data = cmddata[i];

		if (!cmdlen[i])
			continue;
		if (!(cmd->dataflags[i] & (IWL_HCMD_DFL_NOCOPY |
					   IWL_HCMD_DFL_DUP)))
			continue;
		if (cmd->dataflags[i] & IWL_HCMD_DFL_DUP)
			data = dup_buf;
		phys_addr = dma_map_single(trans->dev, (void *)data,
					   cmdlen[i], DMA_BIDIRECTIONAL);
		if (dma_mapping_error(trans->dev, phys_addr)) {
			iwl_pcie_tfd_unmap(trans, out_meta,
					   &txq->tfds[q->write_ptr],
					   DMA_BIDIRECTIONAL);
			idx = -ENOMEM;
			goto out;
		}

		iwl_pcie_txq_build_tfd(trans, txq, phys_addr, cmdlen[i], 0);
	}

	out_meta->flags = cmd->flags;
	if (WARN_ON_ONCE(txq->entries[idx].free_buf))
		kfree(txq->entries[idx].free_buf);
	txq->entries[idx].free_buf = dup_buf;

	txq->need_update = 1;

	trace_iwlwifi_dev_hcmd(trans->dev, cmd, cmd_size, &out_cmd->hdr);

	/* start timer if queue currently empty */
	if (q->read_ptr == q->write_ptr && trans_pcie->wd_timeout)
		mod_timer(&txq->stuck_timer, jiffies + trans_pcie->wd_timeout);

	/* Increment and update queue's write index */
	q->write_ptr = iwl_queue_inc_wrap(q->write_ptr, q->n_bd);
	iwl_pcie_txq_inc_wr_ptr(trans, txq);

 out:
	spin_unlock_bh(&txq->lock);
 free_dup_buf:
	if (idx < 0)
		kfree(dup_buf);
	return idx;
}

/*
 * iwl_pcie_hcmd_complete - Pull unused buffers off the queue and reclaim them
 * @rxb: Rx buffer to reclaim
 * @handler_status: return value of the handler of the command
 *	(put in setup_rx_handlers)
 *
 * If an Rx buffer has an async callback associated with it the callback
 * will be executed.  The attached skb (if present) will only be freed
 * if the callback returns 1
 */
void iwl_pcie_hcmd_complete(struct iwl_trans *trans,
			    struct iwl_rx_cmd_buffer *rxb, int handler_status)
{
	struct iwl_rx_packet *pkt = rxb_addr(rxb);
	u16 sequence = le16_to_cpu(pkt->hdr.sequence);
	int txq_id = SEQ_TO_QUEUE(sequence);
	int index = SEQ_TO_INDEX(sequence);
	int cmd_index;
	struct iwl_device_cmd *cmd;
	struct iwl_cmd_meta *meta;
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct iwl_txq *txq = &trans_pcie->txq[trans_pcie->cmd_queue];

	/* If a Tx command is being handled and it isn't in the actual
	 * command queue then there a command routing bug has been introduced
	 * in the queue management code. */
	if (WARN(txq_id != trans_pcie->cmd_queue,
		 "wrong command queue %d (should be %d), sequence 0x%X readp=%d writep=%d\n",
		 txq_id, trans_pcie->cmd_queue, sequence,
		 trans_pcie->txq[trans_pcie->cmd_queue].q.read_ptr,
		 trans_pcie->txq[trans_pcie->cmd_queue].q.write_ptr)) {
		iwl_print_hex_error(trans, pkt, 32);
		return;
	}

	spin_lock_bh(&txq->lock);

	cmd_index = get_cmd_index(&txq->q, index);
	cmd = txq->entries[cmd_index].cmd;
	meta = &txq->entries[cmd_index].meta;

	iwl_pcie_tfd_unmap(trans, meta, &txq->tfds[index], DMA_BIDIRECTIONAL);

	/* Input error checking is done when commands are added to queue. */
	if (meta->flags & CMD_WANT_SKB) {
		struct page *p = rxb_steal_page(rxb);

		meta->source->resp_pkt = pkt;
		meta->source->_rx_page_addr = (unsigned long)page_address(p);
		meta->source->_rx_page_order = trans_pcie->rx_page_order;
		meta->source->handler_status = handler_status;
	}

	iwl_pcie_cmdq_reclaim(trans, txq_id, index);

	if (!(meta->flags & CMD_ASYNC)) {
		if (!test_bit(STATUS_HCMD_ACTIVE, &trans_pcie->status)) {
			IWL_WARN(trans,
				 "HCMD_ACTIVE already clear for command %s\n",
				 get_cmd_string(trans_pcie, cmd->hdr.cmd));
		}
		clear_bit(STATUS_HCMD_ACTIVE, &trans_pcie->status);
		IWL_DEBUG_INFO(trans, "Clearing HCMD_ACTIVE for command %s\n",
			       get_cmd_string(trans_pcie, cmd->hdr.cmd));
		wake_up(&trans_pcie->wait_command_queue);
	}

	meta->flags = 0;

	spin_unlock_bh(&txq->lock);
}

#define HOST_COMPLETE_TIMEOUT (2 * HZ)

static int iwl_pcie_send_hcmd_async(struct iwl_trans *trans,
				    struct iwl_host_cmd *cmd)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int ret;

	/* An asynchronous command can not expect an SKB to be set. */
	if (WARN_ON(cmd->flags & CMD_WANT_SKB))
		return -EINVAL;

	ret = iwl_pcie_enqueue_hcmd(trans, cmd);
	if (ret < 0) {
		IWL_ERR(trans,
			"Error sending %s: enqueue_hcmd failed: %d\n",
			get_cmd_string(trans_pcie, cmd->id), ret);
		return ret;
	}
	return 0;
}

static int iwl_pcie_send_hcmd_sync(struct iwl_trans *trans,
				   struct iwl_host_cmd *cmd)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	int cmd_idx;
	int ret;

	IWL_DEBUG_INFO(trans, "Attempting to send sync command %s\n",
		       get_cmd_string(trans_pcie, cmd->id));

	if (WARN_ON(test_and_set_bit(STATUS_HCMD_ACTIVE,
				     &trans_pcie->status))) {
		IWL_ERR(trans, "Command %s: a command is already active!\n",
			get_cmd_string(trans_pcie, cmd->id));
		return -EIO;
	}

	IWL_DEBUG_INFO(trans, "Setting HCMD_ACTIVE for command %s\n",
		       get_cmd_string(trans_pcie, cmd->id));

	cmd_idx = iwl_pcie_enqueue_hcmd(trans, cmd);
	if (cmd_idx < 0) {
		ret = cmd_idx;
		clear_bit(STATUS_HCMD_ACTIVE, &trans_pcie->status);
		IWL_ERR(trans,
			"Error sending %s: enqueue_hcmd failed: %d\n",
			get_cmd_string(trans_pcie, cmd->id), ret);
		return ret;
	}

	ret = wait_event_timeout(trans_pcie->wait_command_queue,
				 !test_bit(STATUS_HCMD_ACTIVE,
					   &trans_pcie->status),
				 HOST_COMPLETE_TIMEOUT);
	if (!ret) {
		if (test_bit(STATUS_HCMD_ACTIVE, &trans_pcie->status)) {
			struct iwl_txq *txq =
				&trans_pcie->txq[trans_pcie->cmd_queue];
			struct iwl_queue *q = &txq->q;

			IWL_ERR(trans,
				"Error sending %s: time out after %dms.\n",
				get_cmd_string(trans_pcie, cmd->id),
				jiffies_to_msecs(HOST_COMPLETE_TIMEOUT));

			IWL_ERR(trans,
				"Current CMD queue read_ptr %d write_ptr %d\n",
				q->read_ptr, q->write_ptr);

			clear_bit(STATUS_HCMD_ACTIVE, &trans_pcie->status);
			IWL_DEBUG_INFO(trans,
				       "Clearing HCMD_ACTIVE for command %s\n",
				       get_cmd_string(trans_pcie, cmd->id));
			ret = -ETIMEDOUT;
			goto cancel;
		}
	}

	if (test_bit(STATUS_FW_ERROR, &trans_pcie->status)) {
		IWL_ERR(trans, "FW error in SYNC CMD %s\n",
			get_cmd_string(trans_pcie, cmd->id));
		ret = -EIO;
		goto cancel;
	}

	if (test_bit(STATUS_RFKILL, &trans_pcie->status)) {
		IWL_DEBUG_RF_KILL(trans, "RFKILL in SYNC CMD... no rsp\n");
		ret = -ERFKILL;
		goto cancel;
	}

	if ((cmd->flags & CMD_WANT_SKB) && !cmd->resp_pkt) {
		IWL_ERR(trans, "Error: Response NULL in '%s'\n",
			get_cmd_string(trans_pcie, cmd->id));
		ret = -EIO;
		goto cancel;
	}

	return 0;

cancel:
	if (cmd->flags & CMD_WANT_SKB) {
		/*
		 * Cancel the CMD_WANT_SKB flag for the cmd in the
		 * TX cmd queue. Otherwise in case the cmd comes
		 * in later, it will possibly set an invalid
		 * address (cmd->meta.source).
		 */
		trans_pcie->txq[trans_pcie->cmd_queue].
			entries[cmd_idx].meta.flags &= ~CMD_WANT_SKB;
	}

	if (cmd->resp_pkt) {
		iwl_free_resp(cmd);
		cmd->resp_pkt = NULL;
	}

	return ret;
}

int iwl_trans_pcie_send_hcmd(struct iwl_trans *trans, struct iwl_host_cmd *cmd)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);

	if (test_bit(STATUS_FW_ERROR, &trans_pcie->status))
		return -EIO;

	if (test_bit(STATUS_RFKILL, &trans_pcie->status))
		return -ERFKILL;

	if (cmd->flags & CMD_ASYNC)
		return iwl_pcie_send_hcmd_async(trans, cmd);

	/* We still can fail on RFKILL that can be asserted while we wait */
	return iwl_pcie_send_hcmd_sync(trans, cmd);
}

int iwl_trans_pcie_tx(struct iwl_trans *trans, struct sk_buff *skb,
		      struct iwl_device_cmd *dev_cmd, int txq_id)
{
	struct iwl_trans_pcie *trans_pcie = IWL_TRANS_GET_PCIE_TRANS(trans);
	struct ieee80211_hdr *hdr = (struct ieee80211_hdr *)skb->data;
	struct iwl_tx_cmd *tx_cmd = (struct iwl_tx_cmd *)dev_cmd->payload;
	struct iwl_cmd_meta *out_meta;
	struct iwl_txq *txq;
	struct iwl_queue *q;
	dma_addr_t phys_addr = 0;
	dma_addr_t txcmd_phys;
	dma_addr_t scratch_phys;
	u16 len, firstlen, secondlen;
	u8 wait_write_ptr = 0;
	__le16 fc = hdr->frame_control;
	u8 hdr_len = ieee80211_hdrlen(fc);
	u16 __maybe_unused wifi_seq;

	txq = &trans_pcie->txq[txq_id];
	q = &txq->q;

	if (unlikely(!test_bit(txq_id, trans_pcie->queue_used))) {
		WARN_ON_ONCE(1);
		return -EINVAL;
	}

	spin_lock(&txq->lock);

	/* In AGG mode, the index in the ring must correspond to the WiFi
	 * sequence number. This is a HW requirements to help the SCD to parse
	 * the BA.
	 * Check here that the packets are in the right place on the ring.
	 */
#ifdef CONFIG_IWLWIFI_DEBUG
	wifi_seq = SEQ_TO_SN(le16_to_cpu(hdr->seq_ctrl));
	WARN_ONCE((iwl_read_prph(trans, SCD_AGGR_SEL) & BIT(txq_id)) &&
		  ((wifi_seq & 0xff) != q->write_ptr),
		  "Q: %d WiFi Seq %d tfdNum %d",
		  txq_id, wifi_seq, q->write_ptr);
#endif

	/* Set up driver data for this TFD */
	txq->entries[q->write_ptr].skb = skb;
	txq->entries[q->write_ptr].cmd = dev_cmd;

	dev_cmd->hdr.cmd = REPLY_TX;
	dev_cmd->hdr.sequence =
		cpu_to_le16((u16)(QUEUE_TO_SEQ(txq_id) |
			    INDEX_TO_SEQ(q->write_ptr)));

	/* Set up first empty entry in queue's array of Tx/cmd buffers */
	out_meta = &txq->entries[q->write_ptr].meta;

	/*
	 * Use the first empty entry in this queue's command buffer array
	 * to contain the Tx command and MAC header concatenated together
	 * (payload data will be in another buffer).
	 * Size of this varies, due to varying MAC header length.
	 * If end is not dword aligned, we'll have 2 extra bytes at the end
	 * of the MAC header (device reads on dword boundaries).
	 * We'll tell device about this padding later.
	 */
	len = sizeof(struct iwl_tx_cmd) +
		sizeof(struct iwl_cmd_header) + hdr_len;
	firstlen = (len + 3) & ~3;

	/* Tell NIC about any 2-byte padding after MAC header */
	if (firstlen != len)
		tx_cmd->tx_flags |= TX_CMD_FLG_MH_PAD_MSK;

	/* Physical address of this Tx command's header (not MAC header!),
	 * within command buffer array. */
	txcmd_phys = dma_map_single(trans->dev,
				    &dev_cmd->hdr, firstlen,
				    DMA_BIDIRECTIONAL);
	if (unlikely(dma_mapping_error(trans->dev, txcmd_phys)))
		goto out_err;
	dma_unmap_addr_set(out_meta, mapping, txcmd_phys);
	dma_unmap_len_set(out_meta, len, firstlen);

	if (!ieee80211_has_morefrags(fc)) {
		txq->need_update = 1;
	} else {
		wait_write_ptr = 1;
		txq->need_update = 0;
	}

	/* Set up TFD's 2nd entry to point directly to remainder of skb,
	 * if any (802.11 null frames have no payload). */
	secondlen = skb->len - hdr_len;
	if (secondlen > 0) {
		phys_addr = dma_map_single(trans->dev, skb->data + hdr_len,
					   secondlen, DMA_TO_DEVICE);
		if (unlikely(dma_mapping_error(trans->dev, phys_addr))) {
			dma_unmap_single(trans->dev,
					 dma_unmap_addr(out_meta, mapping),
					 dma_unmap_len(out_meta, len),
					 DMA_BIDIRECTIONAL);
			goto out_err;
		}
	}

	/* Attach buffers to TFD */
	iwl_pcie_txq_build_tfd(trans, txq, txcmd_phys, firstlen, 1);
	if (secondlen > 0)
		iwl_pcie_txq_build_tfd(trans, txq, phys_addr, secondlen, 0);

	scratch_phys = txcmd_phys + sizeof(struct iwl_cmd_header) +
				offsetof(struct iwl_tx_cmd, scratch);

	/* take back ownership of DMA buffer to enable update */
	dma_sync_single_for_cpu(trans->dev, txcmd_phys, firstlen,
				DMA_BIDIRECTIONAL);
	tx_cmd->dram_lsb_ptr = cpu_to_le32(scratch_phys);
	tx_cmd->dram_msb_ptr = iwl_get_dma_hi_addr(scratch_phys);

	/* Set up entry for this TFD in Tx byte-count array */
	iwl_pcie_txq_update_byte_cnt_tbl(trans, txq, le16_to_cpu(tx_cmd->len));

	dma_sync_single_for_device(trans->dev, txcmd_phys, firstlen,
				   DMA_BIDIRECTIONAL);

	trace_iwlwifi_dev_tx(trans->dev, skb,
			     &txq->tfds[txq->q.write_ptr],
			     sizeof(struct iwl_tfd),
			     &dev_cmd->hdr, firstlen,
			     skb->data + hdr_len, secondlen);
	trace_iwlwifi_dev_tx_data(trans->dev, skb,
				  skb->data + hdr_len, secondlen);

	/* start timer if queue currently empty */
	if (txq->need_update && q->read_ptr == q->write_ptr &&
	    trans_pcie->wd_timeout)
		mod_timer(&txq->stuck_timer, jiffies + trans_pcie->wd_timeout);

	/* Tell device the write index *just past* this latest filled TFD */
	q->write_ptr = iwl_queue_inc_wrap(q->write_ptr, q->n_bd);
	iwl_pcie_txq_inc_wr_ptr(trans, txq);

	/*
	 * At this point the frame is "transmitted" successfully
	 * and we will get a TX status notification eventually,
	 * regardless of the value of ret. "ret" only indicates
	 * whether or not we should update the write pointer.
	 */
	if (iwl_queue_space(q) < q->high_mark) {
		if (wait_write_ptr) {
			txq->need_update = 1;
			iwl_pcie_txq_inc_wr_ptr(trans, txq);
		} else {
			iwl_stop_queue(trans, txq);
		}
	}
	spin_unlock(&txq->lock);
	return 0;
out_err:
	spin_unlock(&txq->lock);
	return -1;
}