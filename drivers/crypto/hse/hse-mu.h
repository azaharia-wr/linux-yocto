/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * NXP HSE Driver - Messaging Unit Interface
 *
 * This file defines the interface specification for the Messaging Unit
 * instance used by host application cores to request services from HSE.
 *
 * Copyright 2019-2021 NXP
 */

#ifndef HSE_MU_H
#define HSE_MU_H

#define HSE_MU_INST    "mu" __stringify(CONFIG_CRYPTO_DEV_NXP_HSE_MU_ID) "b"

#define HSE_NUM_CHANNELS    16u /* number of available service channels */
#define HSE_STREAM_COUNT    4u /* number of usable streams per MU instance */

#define HSE_CHANNEL_INV    0xFFu /* invalid acquired service channel index */
#define HSE_CH_MASK_ALL    0x0000FFFFul /* all available channels irq mask */

#define HSE_STATUS_MASK    0xFFFF0000ul /* HSE global status FSR mask */

#define HSE_EVT_MASK_ERR     0x000000FFul /* fatal error GSR mask */
#define HSE_EVT_MASK_WARN    0x0000FF00ul /* warning GSR mask */
#define HSE_EVT_MASK_INTL    0xFFFF0000ul /* NXP internal flags GSR mask */
#define HSE_EVT_MASK_ALL     0xFFFFFFFFul /* all events GSR mask */

/**
 * enum hse_irq_type - HSE interrupt type
 * @HSE_INT_ACK_REQUEST: TX Interrupt, triggered when HSE acknowledged the
 *                       service request and released the service channel
 * @HSE_INT_RESPONSE: RX Interrupt, triggered when HSE wrote the response
 * @HSE_INT_SYS_EVENT: General Purpose Interrupt, triggered when HSE sends
 *                     a system event, generally an error notification
 */
enum hse_irq_type {
	HSE_INT_ACK_REQUEST = 0u,
	HSE_INT_RESPONSE = 1u,
	HSE_INT_SYS_EVENT = 2u,
};

void *hse_mu_init(struct device *dev, irqreturn_t (*rx_isr)(int irq, void *dev),
		  irqreturn_t (*evt_isr)(int irq, void *dev));

void __iomem *hse_mu_desc_base_ptr(void *mu);
dma_addr_t hse_mu_desc_base_dma(void *mu);

u16 hse_mu_check_status(void *mu);
u32 hse_mu_check_event(void *mu);
void hse_mu_trigger_event(void *mu, u32 evt);

void hse_mu_irq_enable(void *mu, enum hse_irq_type irq_type, u32 irq_mask);
void hse_mu_irq_disable(void *mu, enum hse_irq_type irq_type, u32 irq_mask);
void hse_mu_irq_clear(void *mu, enum hse_irq_type irq_type, u32 irq_mask);

u8 hse_mu_next_pending_channel(void *mu);

int hse_mu_msg_send(void *mu, u8 channel, u32 msg);
int hse_mu_msg_recv(void *mu, u8 channel, u32 *msg);

#endif /* HSE_MU_H */
