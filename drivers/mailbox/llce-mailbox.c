// SPDX-License-Identifier: GPL-2.0+ OR BSD-3-Clause
/*
 * Copyright 2020-2022 NXP
 */
#include <dt-bindings/mailbox/nxp-llce-mb.h>
#include <linux/can/dev.h>
#include <linux/clk.h>
#include <linux/ctype.h>
#include <linux/genalloc.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/kernel.h>
#include <linux/mailbox/nxp-llce/llce_can.h>
#include <linux/mailbox/nxp-llce/llce_interface_fifo.h>
#include <linux/mailbox/nxp-llce/llce_mailbox.h>
#include <linux/mailbox/nxp-llce/llce_sema42.h>
#include <linux/mailbox_client.h>
#include <linux/mailbox_controller.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_platform.h>
#include <linux/of_reserved_mem.h>
#include <linux/once.h>
#include <linux/platform_device.h>
#include <linux/processor.h>
#include <linux/slab.h>
#include <uapi/linux/can.h>

#include "mailbox.h"

#define LLCE_FIFO_SIZE			0x400
#define LLCE_RXMBEXTENSION_OFFSET	0x8F0U

#define LLCE_NFIFO_WITH_IRQ		16
#define LLCE_RXIN_N_FIFO		20

#define LLCE_NTXACK_FIFOS		21
#define LLCE_NRXOUT_FIFOS		21

#define LLCE_CAN_RXIN_ICSR_0_7		14
#define LLCE_CAN_RXIN_ICSR_8_15		15
#define LLCE_CAN_RXOUT_ICSR_0_7		16
#define LLCE_CAN_RXOUT_ICSR_8_15	17
#define LLCE_CAN_TXACK_ICSR_0_7		22
#define LLCE_CAN_TXACK_ICSR_8_15	23
#define LLCE_CAN_LOGGER_ICSR		24
#define LLCE_CAN_ICSR_N_ACK		8

#define LLCE_CAN_ICSR_RXIN_INDEX	0
#define LLCE_CAN_ICSR_RXOUT_INDEX	1
#define LLCE_CAN_ICSR_TXACK_INDEX	2

#define LLCE_CAN_LOGGER_IN_FIFO		16
#define LLCE_CAN_LOGGER_OUT_FIFO	17

#define LLCE_LOGGER_ICSR_IRQ		BIT(5)

#define LLCE_CAN_COMPATIBLE		"nxp,s32g-llce-can"
#define LLCE_SHMEM_REG_NAME		"shmem"
#define LLCE_STATUS_REG_NAME		"status"

#define LLCE_MODULE_ENTRY(MODULE) \
	[MODULE - LLCE_TX] = __stringify_1(MODULE)

/* Mask to extract the hw ctrl where the frame comes from */
#define HWCTRL_MBEXTENSION_MASK		0x0FFU
#define HWCTRL_MBEXTENSION_SHIFT	24

#define LLCE_FEATURE_DISABLED	'_'

#define LLCE_LOGGING		(4)

struct llce_icsr {
	uint8_t icsr0_num;
	uint8_t icsr8_num;
};

struct llce_fifoirq {
	int num;
	const char *name;
	irq_handler_t handler;
};

struct llce_pair_irq {
	struct llce_fifoirq irq0;
	struct llce_fifoirq irq8;
};

struct llce_mb {
	struct mbox_controller controller;
	struct llce_pair_irq rxin_irqs;
	struct llce_pair_irq rxout_irqs;
	struct llce_pair_irq txack_irqs;
	spinlock_t txack_lock;
	struct llce_can_shared_memory *sh_mem;
	void __iomem *status;
	void __iomem *rxout_fifo;
	void __iomem *rxin_fifo;
	void __iomem *txack_fifo;
	void __iomem *blrout_fifo;
	void __iomem *blrin_fifo;
	void __iomem *icsr;
	void __iomem *sema42;
	struct clk *clk;
	struct device *dev;
	DECLARE_BITMAP(chans_map, LLCE_NFIFO_WITH_IRQ);
	struct llce_fifoirq logger_irq;
	bool suspended;
	bool fw_logger_support;
};

struct llce_mb_desc {
	const char *name;
	unsigned int nchan;
	int (*startup)(struct mbox_chan *chan);
	void (*shutdown)(struct mbox_chan *chan);
};

struct llce_logger_data {
	bool has_leftover;
	struct llce_can_mb *frame;
	u32 index;
};

static int llce_rx_startup(struct mbox_chan *chan);
static int llce_tx_startup(struct mbox_chan *chan);
static void llce_rx_shutdown(struct mbox_chan *chan);
static void llce_tx_shutdown(struct mbox_chan *chan);
static int llce_hif_startup(struct mbox_chan *chan);
static int llce_logger_startup(struct mbox_chan *chan);
static void llce_logger_shutdown(struct mbox_chan *chan);
static int process_rx_cmd(struct mbox_chan *chan, struct llce_rx_msg *msg);
static int process_logger_cmd(struct mbox_chan *chan, struct llce_rx_msg *msg);

/**
 * Platform configuration can be skipped for cases when the HIF is completely
 * managed by another software
 */
static bool config_platform = true;
module_param(config_platform, bool, 0660);

const struct llce_error *get_llce_errors(size_t *n_elems);

static const char * const llce_modules[] = {
	LLCE_MODULE_ENTRY(LLCE_TX),
	LLCE_MODULE_ENTRY(LLCE_RX),
	LLCE_MODULE_ENTRY(LLCE_DTE),
	LLCE_MODULE_ENTRY(LLCE_FRPE),
};

static const struct llce_mb_desc mb_map[] = {
	[S32G_LLCE_HIF_CONF_MB] = {
		.name = "HIF Config",
		.nchan = 2,
		.startup = llce_hif_startup,
	},
	[S32G_LLCE_CAN_CONF_MB] = {
		.name = "CAN Config",
		.nchan = 16,
	},
	[S32G_LLCE_CAN_RX_MB] = {
		.name = "CAN RX",
		.nchan = 16,
		.startup = llce_rx_startup,
		.shutdown = llce_rx_shutdown,
	},
	[S32G_LLCE_CAN_TX_MB] = {
		.name = "CAN TX",
		.nchan = 16,
		.startup = llce_tx_startup,
		.shutdown = llce_tx_shutdown,
	},
	[S32G_LLCE_CAN_LOGGER_MB] = {
		.name = "CAN Logger",
		.nchan = 16,
		.startup = llce_logger_startup,
		.shutdown = llce_logger_shutdown,
	},
	[S32G_LLCE_CAN_LOGGER_CONFIG_MB] = {
		.name = "CAN Logger Config",
		.nchan = 16,
	},
};

static const struct llce_icsr icsrs[] = {
	[LLCE_CAN_ICSR_RXIN_INDEX] = {
		.icsr0_num = LLCE_CAN_RXIN_ICSR_0_7,
		.icsr8_num = LLCE_CAN_RXIN_ICSR_8_15,
	},
	[LLCE_CAN_ICSR_RXOUT_INDEX] = {
		.icsr0_num = LLCE_CAN_RXOUT_ICSR_0_7,
		.icsr8_num = LLCE_CAN_RXOUT_ICSR_8_15,
	},
	[LLCE_CAN_ICSR_TXACK_INDEX] = {
		.icsr0_num = LLCE_CAN_TXACK_ICSR_0_7,
		.icsr8_num = LLCE_CAN_TXACK_ICSR_8_15,
	},
};

static const char *get_module_name(enum llce_can_module module)
{
	uint32_t index = module - LLCE_TX;

	if (index >= ARRAY_SIZE(llce_modules))
		return "Unknown module";

	return llce_modules[index];
}

static unsigned int get_num_chans(void)
{
	size_t i;
	unsigned int num = 0;

	for (i = 0; i < ARRAY_SIZE(mb_map); i++)
		num += mb_map[i].nchan;

	return num;
}

static unsigned int get_channels_for_type(unsigned int type)
{
	return mb_map[type].nchan;
}

static unsigned int get_channel_offset(unsigned int type, unsigned int index)
{
	size_t i;
	unsigned int off = index;

	for (i = 0; i < ARRAY_SIZE(mb_map); i++) {
		if (type == i)
			return off;

		off += mb_map[i].nchan;
	}

	return off;
}

static const char *get_channel_type_name(unsigned int type)
{
	if (type >= ARRAY_SIZE(mb_map))
		return "Unknown channel type";

	return mb_map[type].name;
}

static bool is_tx_fifo_empty(void __iomem *tx_fifo)
{
	void __iomem *status = LLCE_FIFO_STATUS0(tx_fifo);

	return !(readl(status) & LLCE_FIFO_FNEMTY);
}

static void __iomem *get_fifo_by_index(void __iomem *fifo_base,
				       unsigned int max_index,
				       unsigned int index)
{
	if (index < max_index)
		return fifo_base + (LLCE_FIFO_SIZE * index);

	return NULL;
}

static void __iomem *get_rxin_by_index(struct llce_mb *mb, unsigned int index)
{
	return get_fifo_by_index(mb->rxin_fifo, LLCE_RXIN_N_FIFO, index);
}

static void __iomem *get_txack_by_index(struct llce_mb *mb, unsigned int index)
{
	return get_fifo_by_index(mb->txack_fifo, LLCE_NTXACK_FIFOS, index);
}

static void __iomem *get_rxout_by_index(struct llce_mb *mb, unsigned int index)
{
	return get_fifo_by_index(mb->rxout_fifo, LLCE_NRXOUT_FIFOS, index);
}

static void __iomem *get_host_rxin(struct llce_mb *mb, unsigned int host_index)
{
	if (host_index == LLCE_CAN_HIF0)
		return get_rxin_by_index(mb, 16);

	return get_rxin_by_index(mb, 18);
}

static void __iomem *get_host_notif(struct llce_mb *mb, unsigned int host_index)
{
	if (host_index == LLCE_CAN_HIF0)
		return get_rxin_by_index(mb, 0);

	return get_rxin_by_index(mb, 8);
}

static void __iomem *get_logger_in(struct llce_mb *mb)
{
	return get_rxout_by_index(mb, LLCE_CAN_LOGGER_IN_FIFO);
}

static void __iomem *get_logger_out(struct llce_mb *mb)
{
	return get_rxout_by_index(mb, LLCE_CAN_LOGGER_OUT_FIFO);
}

static void __iomem *get_host_txack(struct llce_mb *mb, unsigned int host_index)
{
	if (host_index == LLCE_CAN_HIF0)
		return get_txack_by_index(mb, 17);

	return get_txack_by_index(mb, 18);
}

static void __iomem *get_txack_fifo(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	if (priv->type == S32G_LLCE_HIF_CONF_MB)
		return get_host_txack(mb, LLCE_CAN_HIF0);

	return get_txack_by_index(mb, priv->index);
}

static void __iomem *get_rxout_fifo(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	return get_rxout_by_index(mb, priv->index);
}

static void __iomem *get_logger_out_fifo(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	return get_rxout_by_index(mb, LLCE_CAN_LOGGER_OUT_FIFO);
}

static void __iomem *get_blrout_fifo(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	return get_fifo_by_index(mb->blrout_fifo, LLCE_NFIFO_WITH_IRQ,
				 priv->index);
}

static void __iomem *get_blrin_fifo(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	return get_fifo_by_index(mb->blrin_fifo, LLCE_NFIFO_WITH_IRQ,
				 priv->index);
}

static bool is_config_chan(unsigned int chan_type)
{
	return chan_type == S32G_LLCE_CAN_CONF_MB ||
		chan_type == S32G_LLCE_HIF_CONF_MB;
}

static bool is_rx_chan(unsigned int chan_type)
{
	return chan_type == S32G_LLCE_CAN_RX_MB;
}

static bool is_tx_chan(unsigned int chan_type)
{
	return chan_type == S32G_LLCE_CAN_TX_MB;
}

static bool is_logger_chan(unsigned int chan_type)
{
	return chan_type == S32G_LLCE_CAN_LOGGER_MB;
}

static bool is_logger_config_chan(unsigned int chan_type)
{
	return chan_type == S32G_LLCE_CAN_LOGGER_CONFIG_MB;
}

static int init_chan_priv(struct mbox_chan *chan, struct llce_mb *mb,
			  unsigned int type, unsigned int index)
{
	struct llce_chan_priv *priv;

	priv = kmalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

	priv->mb = mb;
	priv->type = type;
	priv->index = index;

	chan->con_priv = priv;
	/* Polling for firmware configuration */
	if (is_config_chan(type) || is_logger_config_chan(type)) {
		chan->txdone_method = TXDONE_BY_POLL;
		priv->state = LLCE_REGISTERED_CHAN;
	} else {
		if (is_rx_chan(type) || is_logger_chan(type))
			chan->txdone_method = TXDONE_BY_ACK;
		else
			chan->txdone_method = TXDONE_BY_IRQ;

		priv->state = LLCE_UNREGISTERED_CHAN;
	}

	priv->data = NULL;
	spin_lock_init(&priv->lock);

	return 0;
}

static void deinit_chan_priv(struct mbox_chan *chan)
{
	kfree(chan->con_priv);
}

static struct mbox_chan *llce_mb_xlate(struct mbox_controller *mbox,
				       const struct of_phandle_args *args)
{
	struct llce_mb *mb = container_of(mbox, struct llce_mb, controller);
	struct device *dev = mbox->dev;
	struct mbox_chan *chan;
	unsigned int type = args->args[0];
	unsigned int index = args->args[1];
	unsigned int off;
	int ret;

	if (type >= ARRAY_SIZE(mb_map)) {
		dev_err(dev, "%u is not a valid channel type\n", type);
		return ERR_PTR(-EINVAL);
	}

	if (index >= get_channels_for_type(type)) {
		dev_err(dev, "%u exceeds the number of allocated channels for type : %d\n",
			index, type);
		return ERR_PTR(-EINVAL);
	}

	off = get_channel_offset(type, index);
	if (off >= mbox->num_chans) {
		dev_err(dev, "Out of bounds access\n");
		return ERR_PTR(-EINVAL);
	}

	chan = &mbox->chans[off];
	ret = init_chan_priv(chan, mb, type, index);
	if (ret)
		return ERR_PTR(ret);

	return chan;
}

static int execute_config_cmd(struct mbox_chan *chan,
			      struct llce_can_command *cmd)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	unsigned int idx = priv->index;
	struct llce_can_command *sh_cmd;
	void __iomem *txack, *push0;
	int ret = 0;
	unsigned long flags;

	spin_lock_irqsave(&mb->txack_lock, flags);

	txack = get_host_txack(mb, LLCE_CAN_HIF0);

	sh_cmd = &mb->sh_mem->can_cmd[LLCE_CAN_HIF0];
	push0 = LLCE_FIFO_PUSH0(txack);

	if (!is_tx_fifo_empty(txack)) {
		ret = -EBUSY;
		goto release_lock;
	}

	priv->last_msg = cmd;
	memcpy(sh_cmd, cmd, sizeof(*cmd));
	sh_cmd->return_value = LLCE_FW_NOTRUN;

	/* Trigger an interrupt to the LLCE */
	writel(idx, push0);

release_lock:
	spin_unlock_irqrestore(&mb->txack_lock, flags);
	return ret;
}

static bool is_blrin_full(struct mbox_chan *chan)
{
	void __iomem *blrin = get_blrin_fifo(chan);
	void __iomem *status1 = LLCE_FIFO_STATUS1(blrin);

	return !!(readl(status1) & LLCE_FIFO_FFULLD);
}

static uint32_t build_word0(bool rtr, bool ide, uint32_t std_id,
			    uint32_t ext_id)
{
	uint32_t word0;

	if (ide) {
		word0 = ext_id << CAN_SFF_ID_BITS | std_id | LLCE_CAN_MB_IDE;
	} else {
		word0 = (std_id << LLCE_CAN_MB_IDSTD_SHIFT);

		/* No retransmission with CAN FD */
		if (rtr)
			word0 |= LLCE_CAN_MB_RTR;
	}

	return word0;
}

static int send_can_msg(struct mbox_chan *chan, struct llce_tx_msg *msg)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	void __iomem *blrout = get_blrout_fifo(chan);
	void __iomem *pop0 = LLCE_FIFO_POP0(blrout);
	void __iomem *blrin = get_blrin_fifo(chan);
	void __iomem *push0 = LLCE_FIFO_PUSH0(blrin);
	struct llce_can_shared_memory *sh_cmd = mb->sh_mem;
	unsigned int idx = priv->index;
	struct canfd_frame *cf = msg->cf;
	uint32_t mb_index;
	uint16_t frame_index;
	uint32_t word0, std_id, ext_id;
	u8 dlc, *payload;
	uint32_t mb_config;

	/* Get a free message buffer from BLROUT queue */
	mb_index = readl(pop0);

	if (mb_index == LLCE_FIFO_NULL_VALUE)
		return -EAGAIN;

	mb_index &= LLCE_CAN_CONFIG_FIFO_FIXED_MASK;

	std_id = cf->can_id & CAN_SFF_MASK;
	ext_id = (cf->can_id & CAN_EFF_MASK) >> CAN_SFF_ID_BITS;

	word0 = build_word0(!!(cf->can_id & CAN_RTR_FLAG),
			    !!(cf->can_id & CAN_EFF_FLAG),
			    std_id, ext_id);

	/* Get the index of the frame reserved by the firmware */
	frame_index = sh_cmd->can_tx_mb_desc[mb_index].mb_frame_idx;
	/* Set CAN ID */
	sh_cmd->can_mb[frame_index].word0 = word0;
	/* Attach a token (channel ID) to be used in ACK handler */
	sh_cmd->can_tx_mb_desc[mb_index].frame_tag1 = idx;
	sh_cmd->can_tx_mb_desc[mb_index].enable_tx_frame_mac = false;
	/* Set the notification interface */
	sh_cmd->can_tx_mb_desc[mb_index].ack_interface = idx;
	payload = &sh_cmd->can_mb[frame_index].payload[0];

	memcpy(payload, cf->data, cf->len);
	dlc = can_len2dlc(cf->len);

	mb_config = dlc;
	if (msg->fd_msg) {
		/* Configure the tx mb as a CAN FD frame. */
		mb_config |= LLCE_CAN_MB_FDF;

		/* Enable BRS feature to allow receiveing of CAN FD frames */
		if (cf->flags & CANFD_BRS)
			mb_config |= LLCE_CAN_MB_BRS;

		if (cf->flags & CANFD_ESI)
			mb_config |= LLCE_CAN_MB_ESI;
	}

	sh_cmd->can_mb[frame_index].word1 = mb_config;

	spin_until_cond(!is_blrin_full(chan));

	/* Submit the buffer in BLRIN queue */
	writel(mb_index, push0);

	return 0;
}

static int process_logger_config_cmd(struct mbox_chan *chan, void *data)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct logger_config_msg *msg = data;
	struct llce_mb *mb = priv->mb;

	switch (msg->cmd) {
	case LOGGER_CMD_FW_SUPPORT:
		msg->fw_logger_support = mb->fw_logger_support;
		return 0;
	default:
		return -EINVAL;
	}
}

static int llce_mb_send_data(struct mbox_chan *chan, void *data)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	/* Client should not flush new tasks if suspended. */
	WARN_ON(mb->suspended);

	if (is_config_chan(priv->type))
		return execute_config_cmd(chan, data);

	if (is_tx_chan(priv->type))
		return send_can_msg(chan, data);

	if (is_rx_chan(priv->type))
		return process_rx_cmd(chan, data);

	if (is_logger_chan(priv->type))
		return process_logger_cmd(chan, data);

	if (is_logger_config_chan(priv->type))
		return process_logger_config_cmd(chan, data);

	return -EINVAL;
}

static void enable_fifo_irq(void __iomem *fifo)
{
	void __iomem *status1 = LLCE_FIFO_STATUS1(fifo);
	void __iomem *ier = LLCE_FIFO_IER(fifo);
	u32 ier_val;

	/* Clear interrupt status flags. */
	writel(readl(status1), status1);
	/* Enable interrupt */
	ier_val = readl(ier) | LLCE_FIFO_FNEMTY;
	writel(ier_val, ier);
}

static void disable_fifo_irq(void __iomem *fifo)
{
	void __iomem *ier = LLCE_FIFO_IER(fifo);
	u32 ier_val;

	ier_val = readl(ier) & ~LLCE_FIFO_FNEMTY;
	writel(ier_val, ier);
}

static enum llce_sema42_gate get_sema42_gate(u8 fifo)
{
	/**
	 * Semaphore used to protect acces to TXACK and RXOUT between LLCE and
	 * host on interrupt enable/disable.
	 */
	static const enum llce_sema42_gate
	sema4_ier[LLCE_CAN_CONFIG_IER_SEMA4_COUNT][LLCE_CAN_CONFIG_HIF_COUNT] = {
		{LLCE_SEMA42_GATE20, LLCE_SEMA42_GATE21},
		{LLCE_SEMA42_GATE22, LLCE_SEMA42_GATE23}
	};

	return sema4_ier[fifo][LLCE_CAN_HIF0];
}

static void ctrl_rxout_irq_with_lock(struct llce_mb *mb, void __iomem *rxout,
				     bool enable)
{
	enum llce_sema42_gate gate = get_sema42_gate(LLCE_FIFO_RXOUT_INDEX);

	llce_sema42_lock(mb->sema42, gate, LLCE_HOST_CORE_SEMA42_DOMAIN);

	if (enable)
		enable_fifo_irq(rxout);
	else
		disable_fifo_irq(rxout);

	llce_sema42_unlock(mb->sema42, gate);
}

static void disable_rx_irq(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	void __iomem *rxout = get_rxout_fifo(chan);

	ctrl_rxout_irq_with_lock(mb, rxout, false);
}

static void enable_rx_irq(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	void __iomem *rxout = get_rxout_fifo(chan);

	ctrl_rxout_irq_with_lock(mb, rxout, true);
}

static void enable_logger_irq(struct llce_mb *mb)
{
	void __iomem *rxout = get_logger_out(mb);

	ctrl_rxout_irq_with_lock(mb, rxout, true);
}

static void disable_logger_irq(struct llce_mb *mb)
{
	void __iomem *rxout = get_logger_out(mb);

	ctrl_rxout_irq_with_lock(mb, rxout, false);
}

static int request_llce_irq(struct llce_mb *mb, struct llce_fifoirq *fifo_irq)
{
	int ret;

	ret = devm_request_irq(mb->dev, fifo_irq->num, fifo_irq->handler,
			       IRQF_SHARED, fifo_irq->name, mb);
	if (ret < 0)
		dev_err(mb->dev, "Failed to register '%s' IRQ\n",
			fifo_irq->name);

	return ret;
}

static int request_llce_pair_irq(struct llce_mb *mb, struct llce_pair_irq *pair)
{
	int ret;

	ret = request_llce_irq(mb, &pair->irq0);
	if (ret)
		return ret;

	ret = request_llce_irq(mb, &pair->irq8);
	if (ret)
		return ret;

	return 0;
}

static int llce_hif_startup(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	request_llce_pair_irq(mb, &mb->rxin_irqs);

	return 0;
}

static int llce_rx_startup(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	unsigned long flags;

	request_llce_pair_irq(mb, &mb->rxout_irqs);

	/* State change must go under the lock protection */
	spin_lock_irqsave(&priv->lock, flags);

	priv->state = LLCE_REGISTERED_CHAN;
	enable_rx_irq(chan);

	spin_unlock_irqrestore(&priv->lock, flags);
	return 0;
}

static void llce_rx_shutdown(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	priv->state = LLCE_UNREGISTERED_CHAN;
	disable_rx_irq(chan);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static void enable_bus_off_irq(struct llce_mb *mb)
{
	void __iomem *rxin = get_host_notif(mb, LLCE_CAN_HIF0);
	void __iomem *ier = LLCE_FIFO_IER(rxin);
	uint32_t ier_val;

	/* Enable BusOff for host 0 only */
	ier_val = readl(ier) | LLCE_FIFO_FNEMTY;
	writel(ier_val, ier);
}

static int llce_tx_startup(struct mbox_chan *chan)
{
	void __iomem *txack = get_txack_fifo(chan);
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	unsigned long flags;

	request_llce_pair_irq(mb, &mb->txack_irqs);

	spin_lock_irqsave(&priv->lock, flags);
	priv->state = LLCE_REGISTERED_CHAN;

	enable_fifo_irq(txack);
	enable_bus_off_irq(mb);

	spin_unlock_irqrestore(&priv->lock, flags);

	return 0;
}

static void llce_tx_shutdown(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	void __iomem *txack = get_txack_fifo(chan);
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);
	priv->state = LLCE_UNREGISTERED_CHAN;

	/* Disable interrupts */
	disable_fifo_irq(txack);

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int llce_logger_startup(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	struct llce_logger_data *data;
	unsigned long flags;
	int ret = 0;

	request_llce_irq(mb, &mb->logger_irq);

	spin_lock_irqsave(&priv->lock, flags);
	priv->state = LLCE_REGISTERED_CHAN;

	/* Make room for POP leftovers */
	priv->data = kmalloc(sizeof(struct llce_logger_data), GFP_KERNEL);
	if (!priv->data)
		ret = -ENOMEM;

	if (!ret) {
		data = priv->data;
		data->has_leftover = false;
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	return ret;
}

static void llce_logger_shutdown(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	priv->state = LLCE_UNREGISTERED_CHAN;
	kfree(priv->data);
	priv->data = NULL;

	spin_unlock_irqrestore(&priv->lock, flags);
}

static int llce_mb_startup(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;

	if (mb_map[priv->type].startup)
		return mb_map[priv->type].startup(chan);

	return 0;
}

static bool is_chan_registered(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	unsigned long flags;
	bool ret;

	if (!priv)
		return false;

	spin_lock_irqsave(&priv->lock, flags);
	ret = (priv->state == LLCE_REGISTERED_CHAN);
	spin_unlock_irqrestore(&priv->lock, flags);

	return ret;
}

static void llce_mbox_chan_received_data(struct mbox_chan *chan, void *msg)
{
	struct llce_chan_priv *priv = chan->con_priv;

	if (!is_chan_registered(chan)) {
		dev_err(chan->mbox->dev, "Received a message on an unregistered channel (type: %s, index: %u)\n",
			get_channel_type_name(priv->type), priv->index);
		return;
	}

	mbox_chan_received_data(chan, msg);
}

static bool llce_mb_last_tx_done(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	void __iomem *txack;
	struct llce_mb *mb = priv->mb;
	struct llce_can_command *cmd;
	struct llce_can_command *sh_cmd;
	unsigned long flags;

	if (is_logger_config_chan(priv->type)) {
		llce_mbox_chan_received_data(chan, NULL);
		return true;
	}

	if (!is_config_chan(priv->type))
		return false;

	spin_lock_irqsave(&mb->txack_lock, flags);

	txack = get_host_txack(mb, LLCE_CAN_HIF0);

	/* Wait an answer from LLCE FW */
	spin_until_cond(is_tx_fifo_empty(txack));

	cmd = priv->last_msg;
	sh_cmd = &mb->sh_mem->can_cmd[LLCE_CAN_HIF0];

	memcpy(cmd, sh_cmd, sizeof(*cmd));

	spin_unlock_irqrestore(&mb->txack_lock, flags);

	if (priv->type != S32G_LLCE_HIF_CONF_MB)
		llce_mbox_chan_received_data(chan, cmd);

	return true;
}

static void llce_mb_shutdown(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;

	if (mb_map[priv->type].shutdown)
		mb_map[priv->type].shutdown(chan);

	deinit_chan_priv(chan);
}

static const struct mbox_chan_ops llce_mb_ops = {
	.send_data = llce_mb_send_data,
	.startup = llce_mb_startup,
	.shutdown = llce_mb_shutdown,
	.last_tx_done = llce_mb_last_tx_done,
};

static struct device_node *get_sram_node(struct device *dev, const char *name)
{
	struct device_node *node, *dev_node;
	int idx;

	dev_node = dev->of_node;
	idx = of_property_match_string(dev_node, "memory-region-names", name);
	node = of_parse_phandle(dev_node, "memory-region", idx);
	if (!node) {
		dev_err(dev, "Failed to get '%s' memory region\n", name);
		return ERR_PTR(-EIO);
	}

	return node;
}

static int map_sram_node(struct device *dev, const char *name,
			 void __iomem **addr)
{
	struct device_node *sram_node;
	struct resource r;
	resource_size_t size;
	int ret;

	sram_node = get_sram_node(dev, name);
	if (IS_ERR(sram_node))
		return PTR_ERR(sram_node);

	ret = of_address_to_resource(sram_node, 0, &r);
	of_node_put(sram_node);
	if (ret)
		return ret;

	size = resource_size(&r);

	*addr = devm_ioremap_wc(dev, r.start, size);
	if (!*addr) {
		dev_err(dev, "Failed to map '%s' memory region\n", name);
		return -ENOMEM;
	}

	return 0;
}

static int map_llce_status(struct llce_mb *mb)
{
	return map_sram_node(mb->dev, LLCE_STATUS_REG_NAME, &mb->status);
}

static int map_llce_shmem(struct llce_mb *mb)
{
	return map_sram_node(mb->dev, LLCE_SHMEM_REG_NAME,
			     (void __iomem **)&mb->sh_mem);
}

static void __iomem *get_icsr_addr(struct llce_mb *mb, uint32_t icsr_id)
{
	return mb->icsr + icsr_id * sizeof(uint32_t);
}

static void __iomem *get_icsr(struct llce_mb *mb, struct llce_pair_irq *irqs,
			      uint32_t icsr_index, int irq,
			      uint8_t *base_id)
{
	uint32_t icsr_id;
	const struct llce_icsr *icsrs_conf = &icsrs[icsr_index];

	if (irq == irqs->irq0.num) {
		icsr_id = icsrs_conf->icsr0_num;
		*base_id = 0;
	} else {
		icsr_id = icsrs_conf->icsr8_num;
		*base_id = 8;
	}

	return get_icsr_addr(mb, icsr_id);
}

static void __iomem *get_txack_icsr(struct llce_mb *mb, int irq,
				    uint8_t *base_id)
{
	return get_icsr(mb, &mb->txack_irqs, LLCE_CAN_ICSR_TXACK_INDEX,
			irq, base_id);
}

static void __iomem *get_rxin_icsr(struct llce_mb *mb, int irq,
				   uint8_t *base_id)
{
	return get_icsr(mb, &mb->rxin_irqs, LLCE_CAN_ICSR_RXIN_INDEX,
			irq, base_id);
}

static void __iomem *get_rxout_icsr(struct llce_mb *mb, int irq,
				   uint8_t *base_id)
{
	return get_icsr(mb, &mb->rxout_irqs, LLCE_CAN_ICSR_RXOUT_INDEX,
			irq, base_id);
}

static void __iomem *get_logger_icsr(struct llce_mb *mb)
{
	return get_icsr_addr(mb, LLCE_CAN_LOGGER_ICSR);
}

static void llce_process_tx_ack(struct llce_mb *mb, uint8_t index)
{
	void __iomem *tx_ack = get_txack_by_index(mb, index);
	void __iomem *status1 = LLCE_FIFO_STATUS1(tx_ack);
	void __iomem *pop0 = LLCE_FIFO_POP0(tx_ack);
	struct mbox_controller *ctrl = &mb->controller;
	struct llce_can_shared_memory *sh_mem = mb->sh_mem;
	struct llce_can_tx2host_ack_info *info;
	struct llce_tx_notif notif;
	uint32_t ack_id;
	unsigned int chan_index;

	while (!(readl(status1) & LLCE_FIFO_FEMTYD)) {
		/* Get ACK mailbox */
		ack_id = readl(pop0) & LLCE_CAN_CONFIG_FIFO_FIXED_MASK;

		info = &sh_mem->can_tx_ack_info[ack_id];
		chan_index = get_channel_offset(S32G_LLCE_CAN_TX_MB,
						info->frame_tag1);
		notif.error = 0;
		notif.tx_timestamp = info->tx_timestamp;

		/* Notify the client and send the timestamp */
		llce_mbox_chan_received_data(&ctrl->chans[chan_index], &notif);
		mbox_chan_txdone(&ctrl->chans[chan_index], 0);
	}

	/* Clear the interrupt status flag. */
	writel(LLCE_FIFO_FNEMTY, status1);
}

static void process_chan_err(struct llce_mb *mb, uint32_t chan_type,
			     struct llce_can_channel_error_notif *error)
{
	unsigned int chan_index;
	struct mbox_controller *ctrl = &mb->controller;
	void *notif;
	struct llce_rx_msg rx_notif;
	struct llce_tx_notif tx_notif;

	chan_index = get_channel_offset(chan_type, error->hw_ctrl);

	if (is_tx_chan(chan_type)) {
		tx_notif.error = error->error_info.error_code;

		/* Release the channel if an error occurred */
		mbox_chan_txdone(&ctrl->chans[chan_index], 0);
		notif = &tx_notif;
	} else {
		rx_notif.cmd = LLCE_ERROR;
		rx_notif.error = error->error_info.error_code;

		notif = &rx_notif;
	}

	llce_mbox_chan_received_data(&ctrl->chans[chan_index], notif);
}

static void process_channel_err(struct llce_mb *mb,
				struct llce_can_channel_error_notif *error)
{
	enum llce_can_module module_id = error->error_info.module_id;
	enum llce_fw_return err = error->error_info.error_code;

	switch (module_id) {
	case LLCE_TX:
		return process_chan_err(mb, S32G_LLCE_CAN_TX_MB, error);
	case LLCE_RX:
		return process_chan_err(mb, S32G_LLCE_CAN_RX_MB, error);
	default:
		net_warn_ratelimited("%s: Error module:%s Error:%d HW module:%d\n",
				     dev_name(mb->controller.dev),
				     get_module_name(module_id), err,
				     error->hw_ctrl);
		break;
	}
}

static void process_platform_err(struct llce_mb *mb,
				 struct llce_can_error_notif *error)
{
}

static void process_ctrl_err(struct llce_mb *mb,
			     struct llce_can_ctrl_mode_notif *error)
{
}

static void llce_process_rxin(struct llce_mb *mb, uint8_t index)
{
	void __iomem *rxin = get_rxin_by_index(mb, index);
	void __iomem *status1 = LLCE_FIFO_STATUS1(rxin);
	void __iomem *pop0 = LLCE_FIFO_POP0(rxin);
	struct llce_can_shared_memory *sh_mem = mb->sh_mem;
	struct llce_can_notification_table *table;
	struct llce_can_notification *notif;
	union llce_can_notification_list *list;

	uint32_t rxin_id;

	while (!(readl(status1) & LLCE_FIFO_FEMTYD)) {
		/* Get notification mailbox */
		rxin_id = readl(pop0) & LLCE_CAN_CONFIG_FIFO_FIXED_MASK;
		table = &sh_mem->can_notification_table;
		notif = &table->can_notif0_table[LLCE_CAN_HIF0][rxin_id];
		list = &notif->notif_list;

		switch (notif->notif_id) {
		case LLCE_CAN_NOTIF_CHANNELERROR:
			process_channel_err(mb, &list->channel_error);
			break;
		case LLCE_CAN_NOTIF_PLATFORMERROR:
			process_platform_err(mb, &list->platform_error);
			break;
		case LLCE_CAN_NOTIF_CTRLMODE:
			process_ctrl_err(mb, &list->ctrl_mode);
			break;
		case LLCE_CAN_NOTIF_NOERROR:
			break;
		}
	}

	/* Clear the interrupt status flag. */
	writel(LLCE_FIFO_FNEMTY, status1);
}

static bool has_leftovers(struct mbox_chan *chan)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_logger_data *data = priv->data;
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&priv->lock, flags);
	ret = data->has_leftover;
	spin_unlock_irqrestore(&priv->lock, flags);

	return ret;
}

static void push_llce_logger_data(struct mbox_chan *chan, struct llce_can_mb *frame,
				  uint32_t index)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_logger_data *data = priv->data;
	unsigned long flags;

	spin_lock_irqsave(&priv->lock, flags);

	if (data->has_leftover)
		dev_err(chan->cl->dev, "Overwriting logger's internal frame\n");

	data->has_leftover = true;
	data->frame = frame;
	data->index = index;

	spin_unlock_irqrestore(&priv->lock, flags);
}

static bool pop_llce_logger_data(struct mbox_chan *chan, struct llce_can_mb **frame,
				 uint32_t *index)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_logger_data *data = priv->data;
	unsigned long flags;
	bool ret;

	spin_lock_irqsave(&priv->lock, flags);

	if (!data->has_leftover) {
		ret = false;
	} else {
		ret = true;
		data->has_leftover = false;
		*frame = data->frame;
		*index = data->index;
	}

	spin_unlock_irqrestore(&priv->lock, flags);

	return ret;
}

static int process_is_rx_empty(struct mbox_chan *chan, struct llce_rx_msg *msg)
{
	void __iomem *rxout = get_rxout_fifo(chan);
	void __iomem *status1 = LLCE_FIFO_STATUS1(rxout);

	msg->is_rx_empty = !!(readl(status1) & LLCE_FIFO_FEMTYD);
	return 0;
}

static int process_is_logger_empty(struct mbox_chan *chan,
				   struct llce_rx_msg *msg)
{
	void __iomem *rxout = get_logger_out_fifo(chan);
	void __iomem *status1 = LLCE_FIFO_STATUS1(rxout);

	if (has_leftovers(chan)) {
		msg->is_rx_empty = false;
		return 0;
	}

	msg->is_rx_empty = !!(readl(status1) & LLCE_FIFO_FEMTYD);
	return 0;
}

static int process_pop_rxout(struct mbox_chan *chan, struct llce_rx_msg *msg)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	void __iomem *rxout = get_rxout_fifo(chan);
	void __iomem *pop0 = LLCE_FIFO_POP0(rxout);
	struct llce_can_shared_memory *sh_mem = mb->sh_mem;
	unsigned int chan_index;
	u32 rx_mb, frame_id;
	u16 filter_id;


	/* Get RX mailbox */
	rx_mb = readl(pop0) & LLCE_CAN_CONFIG_FIFO_FIXED_MASK;

	frame_id = sh_mem->can_rx_mb_desc[rx_mb].mb_frame_idx;
	filter_id = sh_mem->can_rx_mb_desc[rx_mb].filter_id;

	chan_index = get_channel_offset(S32G_LLCE_CAN_RX_MB, priv->index);

	if (filter_id == USE_LONG_MB) {
		msg->rx_pop.mb.data.longm = &sh_mem->can_mb[frame_id];
		msg->rx_pop.mb.is_long = true;
	} else {
		msg->rx_pop.mb.data.shortm = &sh_mem->can_short_mb[frame_id];
		msg->rx_pop.mb.is_long = false;
	}

	msg->rx_pop.index = rx_mb;
	msg->rx_pop.skip = false;

	return 0;
}

static u32 *get_ctrl_extension(struct llce_mb *mb)
{
	return (uint32_t *)(mb->status + LLCE_RXMBEXTENSION_OFFSET);
}

static uint8_t get_hwctrl(struct llce_mb *mb, uint32_t frame_id,
			  uint32_t mb_index)
{
	u32 *ctrl_extensions = get_ctrl_extension(mb);

	return (ctrl_extensions[frame_id] >> HWCTRL_MBEXTENSION_SHIFT) &
	    HWCTRL_MBEXTENSION_MASK;
}

static void pop_logger_frame(struct llce_mb *mb, struct llce_can_mb **frame,
			     u32 *index, u32 *hw_ctrl)
{
	struct llce_can_shared_memory *sh_mem = mb->sh_mem;
	void __iomem *out_fifo = get_logger_out(mb);
	void __iomem *pop0 = LLCE_FIFO_POP0(out_fifo);
	u32 frame_id;

	*index = readl(pop0) & LLCE_CAN_CONFIG_FIFO_FIXED_MASK;

	frame_id = sh_mem->can_rx_mb_desc[*index].mb_frame_idx;

	*frame = &sh_mem->can_mb[frame_id];
	*hw_ctrl = get_hwctrl(mb, frame_id, *index);
}

static void release_logger_index(struct llce_mb *mb, uint32_t index)
{
	void __iomem *in_fifo = get_logger_in(mb);
	void __iomem *push0 = LLCE_FIFO_PUSH0(in_fifo);

	writel(index, push0);
}

static void send_llce_chan_notif(struct llce_mb *mb, u32 hw_ctrl,
				 struct llce_can_mb *frame, u32 mb_index)
{
	struct mbox_controller *ctrl = &mb->controller;
	struct mbox_chan *chan;
	unsigned int chan_index;
	struct llce_rx_msg msg = {
		.error = 0,
		.cmd = LLCE_RX_NOTIF,
	};

	chan_index = get_channel_offset(S32G_LLCE_CAN_LOGGER_MB, hw_ctrl);

	chan = &ctrl->chans[chan_index];
	if (chan->con_priv && is_chan_registered(chan)) {
		push_llce_logger_data(chan, frame, mb_index);
		llce_mbox_chan_received_data(chan, &msg);
	} else {
		/* Release the index if there are no clients to process it */
		release_logger_index(mb, mb_index);
		enable_logger_irq(mb);
	}
}

static int process_pop_logger(struct mbox_chan *chan, struct llce_rx_msg *msg)
{
	u32 hw_ctrl, mb_index;
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	struct llce_can_mb *frame;
	bool pop;

	/* Use a stashed frame */
	pop = pop_llce_logger_data(chan, &frame, &mb_index);
	if (pop) {
		msg->rx_pop.skip = false;
		msg->rx_pop.mb.data.longm = frame;
		msg->rx_pop.index = mb_index;

		return 0;
	}

	pop_logger_frame(mb, &frame, &mb_index, &hw_ctrl);

	/* Skip the frame as it's not for this channel */
	if (hw_ctrl != priv->index) {
		msg->rx_pop.skip = true;
		send_llce_chan_notif(mb, hw_ctrl, frame, mb_index);
		return 0;
	}

	msg->rx_pop.skip = false;
	msg->rx_pop.mb.data.longm = frame;
	msg->rx_pop.index = mb_index;

	return 0;
}

static int process_release_rxout_index(struct mbox_chan *chan,
				       struct llce_rx_msg *msg)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;
	void __iomem *host_rxin = get_host_rxin(mb, LLCE_CAN_HIF0);
	void __iomem *host_push0 = LLCE_FIFO_PUSH0(host_rxin);

	writel(msg->rx_release.index, host_push0);
	return 0;
}

static int process_release_logger_index(struct mbox_chan *chan,
					struct llce_rx_msg *msg)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	release_logger_index(mb, msg->rx_release.index);
	return 0;
}

static int process_disable_rx_notif(struct mbox_chan *chan,
				    struct llce_rx_msg __always_unused *msg)
{
	disable_rx_irq(chan);
	return 0;
}

static int process_disable_logger_notif(struct mbox_chan *chan,
					struct llce_rx_msg __always_unused *msg)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	disable_logger_irq(mb);
	return 0;
}

static int process_enable_rx_notif(struct mbox_chan *chan,
				   struct llce_rx_msg __always_unused *msg)
{
	enable_rx_irq(chan);
	return 0;
}

static int process_enable_logger_notif(struct mbox_chan *chan,
				       struct llce_rx_msg __always_unused *msg)
{
	struct llce_chan_priv *priv = chan->con_priv;
	struct llce_mb *mb = priv->mb;

	enable_logger_irq(mb);
	return 0;
}

static int process_rx_cmd(struct mbox_chan *chan, struct llce_rx_msg *msg)
{
	if (msg->cmd == LLCE_DISABLE_RX_NOTIF)
		return process_disable_rx_notif(chan, msg);
	if (msg->cmd == LLCE_ENABLE_RX_NOTIF)
		return process_enable_rx_notif(chan, msg);
	if (msg->cmd == LLCE_IS_RX_EMPTY)
		return process_is_rx_empty(chan, msg);
	if (msg->cmd == LLCE_POP_RX)
		return process_pop_rxout(chan, msg);
	if (msg->cmd == LLCE_RELEASE_RX_INDEX)
		return process_release_rxout_index(chan, msg);

	return 0;
}

static int process_logger_cmd(struct mbox_chan *chan, struct llce_rx_msg *msg)
{
	if (msg->cmd == LLCE_DISABLE_RX_NOTIF)
		return process_disable_logger_notif(chan, msg);
	if (msg->cmd == LLCE_ENABLE_RX_NOTIF)
		return process_enable_logger_notif(chan, msg);
	if (msg->cmd == LLCE_IS_RX_EMPTY)
		return process_is_logger_empty(chan, msg);
	if (msg->cmd == LLCE_POP_RX)
		return process_pop_logger(chan, msg);
	if (msg->cmd == LLCE_RELEASE_RX_INDEX)
		return process_release_logger_index(chan, msg);

	return 0;
}

static void llce_process_rxout(struct llce_mb *mb, uint8_t index)
{
	struct mbox_controller *ctrl = &mb->controller;
	unsigned int chan_index;
	struct llce_rx_msg msg = {
		.error = 0,
		.cmd = LLCE_RX_NOTIF,
	};

	chan_index = get_channel_offset(S32G_LLCE_CAN_RX_MB, index);
	disable_rx_irq(&ctrl->chans[chan_index]);

	llce_mbox_chan_received_data(&ctrl->chans[chan_index], &msg);
}

typedef void (*icsr_consumer_t)(struct llce_mb *, uint8_t);

static void llce_consume_icsr(struct llce_mb *mb, void __iomem *icsr_addr,
			      uint8_t base, icsr_consumer_t callback)
{
	uint32_t icsr;
	uint8_t i;

	icsr = readl(icsr_addr);
	for (i = 0; i < LLCE_CAN_ICSR_N_ACK; i++) {
		if (!(icsr & BIT(i)))
			continue;

		callback(mb, base + i);
	}
}

static irqreturn_t llce_rxin_fifo_irq(int irq, void *data)
{
	struct llce_mb *mb = data;
	uint8_t base_icsr;
	void __iomem *icsr_addr = get_rxin_icsr(mb, irq, &base_icsr);

	llce_consume_icsr(mb, icsr_addr, base_icsr, llce_process_rxin);

	return IRQ_HANDLED;
}

static irqreturn_t llce_txack_fifo_irq(int irq, void *data)
{
	struct llce_mb *mb = data;
	uint8_t base_icsr;
	void __iomem *icsr_addr = get_txack_icsr(mb, irq, &base_icsr);

	llce_consume_icsr(mb, icsr_addr, base_icsr, llce_process_tx_ack);

	return IRQ_HANDLED;
}

static irqreturn_t llce_rxout_fifo_irq(int irq, void *data)
{
	struct llce_mb *mb = data;
	uint8_t base_icsr;
	void __iomem *icsr_addr = get_rxout_icsr(mb, irq, &base_icsr);

	llce_consume_icsr(mb, icsr_addr, base_icsr, llce_process_rxout);

	return IRQ_HANDLED;
}

static irqreturn_t llce_logger_rx_irq(int irq, void *data)
{
	struct llce_mb *mb = data;
	void __iomem *icsr_addr = get_logger_icsr(mb);
	u32 icsr, mb_index, hw_ctrl;
	struct llce_can_mb *frame;

	icsr = readl(icsr_addr);
	if (!(icsr & LLCE_LOGGER_ICSR_IRQ))
		return IRQ_NONE;

	disable_logger_irq(mb);

	pop_logger_frame(mb, &frame, &mb_index, &hw_ctrl);

	send_llce_chan_notif(mb, hw_ctrl, frame, mb_index);

	return IRQ_HANDLED;
}

static int init_llce_irq_resources(struct platform_device *pdev,
				   struct llce_mb *mb)
{
	int irq;
	size_t i;
	struct device *dev = &pdev->dev;
	struct llce_fifoirq *fifo_irq;
	struct {
		const char *name;
		irq_handler_t handler;
		struct llce_fifoirq *fifo_irq;
	} resources[] = {
		{
			.name = "rxin_fifo_0_7",
			.handler = llce_rxin_fifo_irq,
			.fifo_irq = &mb->rxin_irqs.irq0,
		},
		{
			.name = "rxin_fifo_8_15",
			.handler = llce_rxin_fifo_irq,
			.fifo_irq = &mb->rxin_irqs.irq8,
		},
		{
			.name = "rxout_fifo_0_7",
			.handler = llce_rxout_fifo_irq,
			.fifo_irq = &mb->rxout_irqs.irq0,
		},
		{
			.name = "rxout_fifo_8_15",
			.handler = llce_rxout_fifo_irq,
			.fifo_irq = &mb->rxout_irqs.irq8,
		},
		{
			.name = "txack_fifo_0_7",
			.handler = llce_txack_fifo_irq,
			.fifo_irq = &mb->txack_irqs.irq0,
		},
		{
			.name = "txack_fifo_8_15",
			.handler = llce_txack_fifo_irq,
			.fifo_irq = &mb->txack_irqs.irq8,
		},
		{
			.name = "logger_rx",
			.handler = llce_logger_rx_irq,
			.fifo_irq = &mb->logger_irq,
		},
	};

	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		irq = platform_get_irq_byname(pdev, resources[i].name);
		if (irq < 0) {
			dev_err(dev, "Failed to request '%s' IRQ\n",
				resources[i].name);
			return irq;
		}

		fifo_irq = resources[i].fifo_irq;
		fifo_irq->name = resources[i].name;
		fifo_irq->handler = resources[i].handler;
		fifo_irq->num = irq;
	}

	return 0;
}

static int init_llce_mem_resources(struct platform_device *pdev,
				   struct llce_mb *mb)
{
	size_t i;
	struct resource *res;
	void __iomem *vaddr;
	struct device *dev = &pdev->dev;
	struct {
		const char *res_name;
		void __iomem **vaddr;
	} resources[] = {
		{ .res_name = "rxout_fifo", .vaddr = &mb->rxout_fifo, },
		{ .res_name = "txack_fifo", .vaddr = &mb->txack_fifo, },
		{ .res_name = "blrout_fifo", .vaddr = &mb->blrout_fifo, },
		{ .res_name = "blrin_fifo", .vaddr = &mb->blrin_fifo, },
		{ .res_name = "rxin_fifo", .vaddr = &mb->rxin_fifo, },
		{ .res_name = "icsr", .vaddr = &mb->icsr, },
		{ .res_name = "sema42", .vaddr = &mb->sema42, },
	};

	for (i = 0; i < ARRAY_SIZE(resources); i++) {
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
						   resources[i].res_name);
		if (!res) {
			dev_err(dev, "Missing '%s' reg region.\n",
				resources[i].res_name);
			return -EIO;
		}

		vaddr = devm_ioremap(dev, res->start,
				     resource_size(res));
		if (!vaddr) {
			dev_err(dev, "Failed to map '%s'\n",
				resources[i].res_name);
			return -ENOMEM;
		}

		*resources[i].vaddr = vaddr;
	}

	return 0;
}

static int get_llce_can_id(const char *node_name, unsigned long *id)
{
	const char *p = node_name + strlen(node_name) - 1;

	while (isdigit(*p))
		p--;

	return kstrtoul(p + 1, 10, id);
}

static struct mbox_chan *get_hif_cfg_chan(struct llce_mb *mb)
{
	struct mbox_controller *ctrl = &mb->controller;
	unsigned int chan_index;

	chan_index = get_channel_offset(S32G_LLCE_HIF_CONF_MB,
					LLCE_CAN_HIF0);
	return &ctrl->chans[chan_index];
}

static int init_hif_config_chan(struct llce_mb *mb)
{
	struct mbox_chan *chan = get_hif_cfg_chan(mb);
	int ret;

	ret = init_chan_priv(chan, mb, S32G_LLCE_HIF_CONF_MB,
			     LLCE_CAN_HIF0);
	if (ret)
		return ret;

	return llce_hif_startup(chan);
}

static void deinit_hif_config_chan(struct llce_mb *mb)
{
	struct mbox_chan *chan = get_hif_cfg_chan(mb);

	deinit_chan_priv(chan);
}

static int execute_hif_cmd(struct llce_mb *mb,
			   struct llce_can_command *cmd)
{
	struct mbox_controller *ctrl = &mb->controller;
	struct device *dev = ctrl->dev;
	static struct mbox_chan *chan;
	int ret;

	chan = get_hif_cfg_chan(mb);

	ret = execute_config_cmd(chan, cmd);
	if (ret) {
		dev_err(dev, "Failed to send command\n");
		return ret;
	}

	/* Wait for command completion */
	if (!llce_mb_last_tx_done(chan))
		return -EIO;

	if (cmd->return_value != LLCE_FW_SUCCESS) {
		dev_err(dev, "LLCE FW error %d\n", cmd->return_value);
		return -EIO;
	}

	return 0;
}

static int llce_init_chan_map(struct device *dev, struct llce_mb *mb)
{
	const char *node_name;
	struct device_node *child;
	unsigned long id;
	int ret;

	for_each_child_of_node(dev->of_node->parent, child) {
		if (!(of_device_is_compatible(child, LLCE_CAN_COMPATIBLE) &&
		      of_device_is_available(child)))
			continue;

		node_name = child->name;
		ret = get_llce_can_id(node_name, &id);
		if (ret) {
			dev_err(dev, "Failed to get ID of the node: %s\n",
				node_name);
			return ret;
		}

		if (id >= LLCE_NFIFO_WITH_IRQ)
			continue;

		set_bit(id, mb->chans_map);
	}

	return 0;
}

static int llce_platform_init(struct device *dev, struct llce_mb *mb)
{
	struct llce_can_init_platform_cmd *pcmd;
	unsigned long id;

	struct llce_can_command cmd = {
		.cmd_id = LLCE_CAN_CMD_INIT_PLATFORM,
		.cmd_list.init_platform = {
			.can_error_reporting = {
				.can_protocol_err = NOTIF_FIFO0,
				.data_lost_err = NOTIF_FIFO0,
				.init_err = NOTIF_FIFO0,
				.internal_err = NOTIF_FIFO0,
			},
		},
	};

	if (!config_platform)
		return 0;

	pcmd = &cmd.cmd_list.init_platform;
	memset(&pcmd->ctrl_init_status, UNINITIALIZED,
	       sizeof(pcmd->ctrl_init_status));
	memset(&pcmd->max_int_tx_ack_count, 0,
	       sizeof(pcmd->max_int_tx_ack_count));
	memset(&pcmd->max_poll_tx_ack_count, 0,
	       sizeof(pcmd->max_poll_tx_ack_count));
	memset(&pcmd->can_error_reporting.bus_off_err, IGNORE,
	       sizeof(pcmd->can_error_reporting.bus_off_err));
	memset(&pcmd->max_regular_filter_count, 0,
	       sizeof(pcmd->max_regular_filter_count));
	memset(&pcmd->max_advanced_filter_count, 0,
	       sizeof(pcmd->max_advanced_filter_count));
	memset(&pcmd->max_int_mb_count, 0, sizeof(pcmd->max_int_mb_count));
	memset(&pcmd->max_poll_mb_count, 0, sizeof(pcmd->max_poll_mb_count));

	for_each_set_bit(id, mb->chans_map, LLCE_NFIFO_WITH_IRQ) {
		pcmd->ctrl_init_status[id] = INITIALIZED;
		pcmd->max_regular_filter_count[id] = 16;
		pcmd->max_advanced_filter_count[id] = 16;
		pcmd->max_int_mb_count[id] = 100;
		pcmd->max_int_tx_ack_count[id] = 16;
		pcmd->can_error_reporting.bus_off_err[id] = NOTIF_FIFO0;
	}

	return execute_hif_cmd(mb, &cmd);
}

static int llce_platform_deinit(struct llce_mb *mb)
{
	struct llce_can_command cmd = {
		.cmd_id = LLCE_CAN_CMD_DEINIT_PLATFORM,
	};

	if (!config_platform)
		return 0;

	return execute_hif_cmd(mb, &cmd);
}

static int get_fw_version(struct llce_mb *mb,
			  struct llce_can_get_fw_version *ver)
{
	struct llce_can_command cmd = {
		.cmd_id = LLCE_CAN_CMD_GETFWVERSION,
	};
	int ret;

	if (!config_platform)
		return 0;

	ret = execute_hif_cmd(mb, &cmd);
	if (ret)
		return ret;

	*ver = cmd.cmd_list.get_fw_version;

	return 0;
}

static void fw_logger_support(struct llce_mb *mb,
			      struct llce_can_get_fw_version *ver)
{
	struct device *dev = mb->controller.dev;

	mb->fw_logger_support =
		!(ver->version_string[LLCE_LOGGING] == LLCE_FEATURE_DISABLED);
	dev_info(dev, "LLCE firmware: logging support %s\n",
		 mb->fw_logger_support ? "enabled" : "disabled");
}

static void print_fw_version(struct llce_mb *mb,
			     struct llce_can_get_fw_version *ver)
{
	struct device *dev = mb->controller.dev;

	dev_info(dev, "LLCE firmware version: %s\n", ver->version_string);
}

static int init_core_clock(struct device *dev, struct clk **clk)
{
	int ret;

	*clk = devm_clk_get(dev, "llce_sys");
	if (IS_ERR(*clk)) {
		dev_err(dev, "No clock available\n");
		return PTR_ERR(*clk);
	}

	ret = clk_prepare_enable(*clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock\n");
		return ret;
	}

	return 0;
}

static void deinit_core_clock(struct clk *clk)
{
	clk_disable_unprepare(clk);
}

static int llce_mb_probe(struct platform_device *pdev)
{
	struct llce_can_get_fw_version ver;
	struct mbox_controller *ctrl;
	struct llce_mb *mb;
	struct device *dev = &pdev->dev;
	int ret;

	mb = devm_kzalloc(&pdev->dev, sizeof(*mb), GFP_KERNEL);
	if (!mb)
		return -ENOMEM;

	spin_lock_init(&mb->txack_lock);

	ctrl = &mb->controller;
	ctrl->txdone_irq = false;
	ctrl->txdone_poll = true;
	ctrl->txpoll_period = 1;
	ctrl->of_xlate = llce_mb_xlate;
	ctrl->num_chans = get_num_chans();
	ctrl->dev = dev;
	ctrl->ops = &llce_mb_ops;

	ctrl->chans = devm_kcalloc(dev, ctrl->num_chans,
				   sizeof(*ctrl->chans), GFP_KERNEL);
	if (!ctrl->chans)
		return -ENOMEM;

	platform_set_drvdata(pdev, mb);

	mb->dev = dev;

	ret = llce_init_chan_map(dev, mb);
	if (ret)
		return ret;

	ret = init_llce_mem_resources(pdev, mb);
	if (ret)
		return ret;

	ret = init_llce_irq_resources(pdev, mb);
	if (ret)
		return ret;

	ret = map_llce_status(mb);
	if (ret)
		return ret;

	ret = map_llce_shmem(mb);
	if (ret)
		return ret;

	ret = init_hif_config_chan(mb);
	if (ret) {
		dev_err(dev, "Failed to initialize HIF config channel\n");
		return ret;
	}

	ret = init_core_clock(dev, &mb->clk);
	if (ret)
		goto hif_deinit;

	ret = get_fw_version(mb, &ver);
	if (ret) {
		dev_err(dev, "Failed to get firmware version\n");
		goto disable_clk;
	}

	print_fw_version(mb, &ver);

	fw_logger_support(mb, &ver);

	ret = llce_platform_init(dev, mb);
	if (ret) {
		dev_err(dev, "Failed to initialize platform\n");
		goto disable_clk;
	}

	ret = devm_mbox_controller_register(dev, ctrl);
	if (ret < 0) {
		dev_err(dev, "Failed to register can config mailbox: %d\n",
			ret);
		goto deinit_plat;
	}

deinit_plat:
	if (ret)
		llce_platform_deinit(mb);

disable_clk:
	if (ret)
		deinit_core_clock(mb->clk);

hif_deinit:
	if (ret)
		deinit_hif_config_chan(mb);

	return ret;
}

static int llce_mb_remove(struct platform_device *pdev)
{
	struct llce_mb *mb = platform_get_drvdata(pdev);
	struct device *dev = &pdev->dev;
	int ret;

	ret = llce_platform_deinit(mb);
	if (ret) {
		dev_err(dev, "Failed to deinitialize LLCE platform");
		return ret;
	}

	deinit_core_clock(mb->clk);
	deinit_hif_config_chan(mb);

	return 0;
}

static int __maybe_unused llce_mb_suspend(struct device *dev)
{
	struct llce_mb *mb = dev_get_drvdata(dev);
	int ret;

	mb->suspended = true;

	ret = llce_platform_deinit(mb);
	if (ret)
		dev_err(dev, "Failed to deinitialize LLCE platform");

	clk_disable_unprepare(mb->clk);

	return 0;
}

static int __maybe_unused llce_mb_resume(struct device *dev)
{
	int ret;
	struct llce_mb *mb = dev_get_drvdata(dev);

	ret = clk_prepare_enable(mb->clk);
	if (ret) {
		dev_err(dev, "Failed to enable clock\n");
		return ret;
	}

	ret = llce_platform_init(dev, mb);
	if (ret)
		dev_err(dev, "Failed to initialize platform\n");

	mb->suspended = false;

	return ret;
}

static const struct of_device_id llce_mb_match[] = {
	{
		.compatible = "nxp,s32g-llce-mailbox",
	},
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, llce_mb_match);

static SIMPLE_DEV_PM_OPS(llce_mb_pm_ops, llce_mb_suspend, llce_mb_resume);

static struct platform_driver llce_mb_driver = {
	.probe = llce_mb_probe,
	.remove = llce_mb_remove,
	.driver = {
		.name = "llce_mb",
		.of_match_table = llce_mb_match,
		.pm = &llce_mb_pm_ops,
	},
};
module_platform_driver(llce_mb_driver)

MODULE_AUTHOR("Ghennadi Procopciuc <ghennadi.procopciuc@nxp.com>");
MODULE_DESCRIPTION("NXP LLCE Mailbox");
MODULE_LICENSE("GPL");
