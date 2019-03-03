/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "atomic.h"
#include "clock.h"
#include "common.h"
#include "console.h"
#include "dma.h"
#include "gpio.h"
#include "hwtimer.h"
#include "hooks.h"
#include "link_defs.h"
#include "registers.h"
#include "task.h"
#include "timer.h"
#include "usb_descriptor.h"
#include "usb_hw.h"
#include "usb_pd.h"
#include "util.h"
#include "ina2xx.h"

/* Size of one USB packet buffer */
#define EP_BUF_SIZE 64

#define EP_PACKET_HEADER_SIZE 4
/* Size of the payload (packet minus the header) */
#define EP_PAYLOAD_SIZE (EP_BUF_SIZE - EP_PACKET_HEADER_SIZE)

/* Buffer enough to avoid overflowing due to USB latencies on both sides */
#define RX_COUNT (16 * EP_PAYLOAD_SIZE)

/* Task event for the USB transfer interrupt */
#define USB_EVENTS TASK_EVENT_CUSTOM(3)

/* Bitmap of enabled capture channels : CC1+CC2 by default */
static uint8_t channel_mask = 0x3;

/* edge timing samples */
static uint8_t samples[2][RX_COUNT];
/* bitmap of the samples sub-buffer filled with DMA data */
static volatile uint32_t filled_dma;
/* timestamps of the beginning of DMA buffers */
static uint16_t sample_tstamp[4];
/* sequence number of the beginning of DMA buffers */
static uint16_t sample_seq[4];

/* USB Buffers not used, ready to be filled */
static volatile uint32_t free_usb = 3;

static inline void led_set_activity(int ch)
{
	static int accumul[2];
	static uint32_t last_ts[2];
	uint32_t now = __hw_clock_source_read();
	int delta = now - last_ts[ch];
	last_ts[ch] = now;
	accumul[ch] = MAX(0, accumul[ch] + (30000 - delta));
	gpio_set_level(ch ? GPIO_LED_R_L : GPIO_LED_G_L, !accumul[ch]);
}

static inline void led_set_record(void)
{
	gpio_set_level(GPIO_LED_B_L, 0);
}

static inline void led_reset_record(void)
{
	gpio_set_level(GPIO_LED_B_L, 1);
}


/* --- RX operation using comparator linked to timer --- */
/* RX on CC1 is using COMP1 triggering TIM1 CH1 */
#define TIM_RX1 1
#define DMAC_TIM_RX1 STM32_DMAC_CH6
#define TIM_RX1_CCR_IDX 1
/* RX on CC1 is using COMP2 triggering TIM2 CH4 */
#define TIM_RX2 2
#define DMAC_TIM_RX2 STM32_DMAC_CH7
#define TIM_RX2_CCR_IDX 4

/* Clock divider for RX edges timings (2.4Mhz counter from 48Mhz clock) */
#define RX_CLOCK_DIV (20 - 1)

static const struct dma_option dma_tim_cc1 = {
	DMAC_TIM_RX1, (void *)&STM32_TIM_CCRx(TIM_RX1, TIM_RX1_CCR_IDX),
	STM32_DMA_CCR_MSIZE_8_BIT | STM32_DMA_CCR_PSIZE_8_BIT |
	STM32_DMA_CCR_CIRC | STM32_DMA_CCR_TCIE | STM32_DMA_CCR_HTIE
};

static const struct dma_option dma_tim_cc2 = {
	DMAC_TIM_RX2, (void *)&STM32_TIM_CCRx(TIM_RX2, TIM_RX2_CCR_IDX),
	STM32_DMA_CCR_MSIZE_8_BIT | STM32_DMA_CCR_PSIZE_8_BIT |
	STM32_DMA_CCR_CIRC | STM32_DMA_CCR_TCIE | STM32_DMA_CCR_HTIE
};

/* sequence number for sample buffers */
static volatile uint32_t seq;
/* Buffer overflow count */
static uint32_t oflow;

#define SNIFFER_CHANNEL_CC1 0
#define SNIFFER_CHANNEL_CC2 1

#define get_channel(b)   (((b) >> 12) & 0x1)

void tim_rx1_handler(uint32_t stat)
{
	stm32_dma_regs_t *dma = STM32_DMA1_REGS;
	int idx = !(stat & STM32_DMA_ISR_HTIF(DMAC_TIM_RX1));
	uint32_t mask = idx ? 0xFF00 : 0x00FF;
	uint32_t next = idx ? 0x0001 : 0x0100;

	sample_tstamp[idx] = __hw_clock_source_read();
	sample_seq[idx] = ((seq++ << 3) & 0x0ff8) |
			(SNIFFER_CHANNEL_CC1<<12);
	if (filled_dma & next) {
		oflow++;
		sample_seq[idx] |= 0x8000;
	} else {
		led_set_record();
	}
	filled_dma |= mask;
	dma->ifcr = STM32_DMA_ISR_ALL(DMAC_TIM_RX1);
	led_set_activity(0);
}

void tim_rx2_handler(uint32_t stat)
{
	stm32_dma_regs_t *dma = STM32_DMA1_REGS;
	int idx = !(stat & STM32_DMA_ISR_HTIF(DMAC_TIM_RX2));
	uint32_t mask = idx ? 0xFF000000 : 0x00FF0000;
	uint32_t next = idx ? 0x00010000 : 0x01000000;

	idx += 2;
	sample_tstamp[idx] = __hw_clock_source_read();
	sample_seq[idx] = ((seq++ << 3) & 0x0ff8) |
			(SNIFFER_CHANNEL_CC2<<12);
	if (filled_dma & next) {
		oflow++;
		sample_seq[idx] |= 0x8000;
	} else {
		led_set_record();
	}
	filled_dma |= mask;
	dma->ifcr = STM32_DMA_ISR_ALL(DMAC_TIM_RX2);
	led_set_activity(1);
}

void tim_dma_handler(void)
{
	stm32_dma_regs_t *dma = STM32_DMA1_REGS;
	uint32_t stat = dma->isr & (STM32_DMA_ISR_HTIF(DMAC_TIM_RX1)
				  | STM32_DMA_ISR_TCIF(DMAC_TIM_RX1)
				  | STM32_DMA_ISR_HTIF(DMAC_TIM_RX2)
				  | STM32_DMA_ISR_TCIF(DMAC_TIM_RX2));
	if (stat & STM32_DMA_ISR_ALL(DMAC_TIM_RX2))
		tim_rx2_handler(stat);
	else
		tim_rx1_handler(stat);
	/* time to process the samples */
	task_set_event(TASK_ID_SNIFFER, TASK_EVENT_CUSTOM(stat), 0);
}
DECLARE_IRQ(STM32_IRQ_DMA_CHANNEL_4_7, tim_dma_handler, 1);

static void rx_timer_init(int tim_id, timer_ctlr_t *tim, int ch_idx, int up_idx)
{
	int bit_idx = 8 * ((ch_idx - 1) % 2);
	/* --- set counter for RX timing : 2.4Mhz rate, free-running --- */
	__hw_timer_enable_clock(tim_id, 1);
	/* Timer configuration */
	tim->cr1 = 0x0004;
	tim->cr2 = 0x0000;
	/* Auto-reload value : 8-bit free running counter */
	tim->arr = 0xFF;
	/* Counter reloading event after 106us */
	tim->ccr[1] = 0xFF;
	/* Timer ICx input configuration */
	if (ch_idx <= 2)
		tim->ccmr1 = 1 << bit_idx;
	else
		tim->ccmr2 = 1 << bit_idx;
	tim->ccer = 0xB << ((ch_idx - 1) * 4);
	/* TODO: add input filtering */
	/* configure DMA request on CCRx update and overflow/update event */
	tim->dier = (1 << (8 + ch_idx)) | (1 << (8 + up_idx));
	/* set prescaler to /26 (F=2.4Mhz, T=0.4us) */
	tim->psc = RX_CLOCK_DIV;
	/* Reload the pre-scaler and reset the counter, clear CCRx */
	tim->egr = 0x001F;
	/* clear update event from reloading */
	tim->sr = 0;
}



void sniffer_init(void)
{
	/* remap TIM1 CH1/2/3 to DMA channel 6 */
	STM32_SYSCFG_CFGR1 |= 1 << 28;

	/* TIM1 CH1 for CC1 RX */
	rx_timer_init(TIM_RX1, (void *)STM32_TIM_BASE(TIM_RX1),
		      TIM_RX1_CCR_IDX, 2);
	/* TIM3 CH4 for CC2 RX */
	rx_timer_init(TIM_RX2, (void *)STM32_TIM_BASE(TIM_RX2),
		      TIM_RX2_CCR_IDX, 2);

	/* turn on COMP/SYSCFG */
	STM32_RCC_APB2ENR |= 1 << 0;
	STM32_COMP_CSR = STM32_COMP_CMP1EN | STM32_COMP_CMP1MODE_HSPEED |
			 STM32_COMP_CMP1INSEL_VREF12 |
			 STM32_COMP_CMP1OUTSEL_TIM1_IC1 |
			 STM32_COMP_CMP1HYST_HI |
			 STM32_COMP_CMP2EN | STM32_COMP_CMP2MODE_HSPEED |
			 STM32_COMP_CMP2INSEL_VREF12 |
			 STM32_COMP_CMP2OUTSEL_TIM2_IC4 |
			 STM32_COMP_CMP2HYST_HI;

	/* start sampling the edges on the CC lines using the RX timers */
	dma_start_rx(&dma_tim_cc1, RX_COUNT, samples[0]);
	dma_start_rx(&dma_tim_cc2, RX_COUNT, samples[1]);
	task_enable_irq(STM32_IRQ_DMA_CHANNEL_4_7);
	/* start RX timers on CC1 and CC2 */
	STM32_TIM_CR1(TIM_RX1) |= 1;
	STM32_TIM_CR1(TIM_RX2) |= 1;
}
DECLARE_HOOK(HOOK_INIT, sniffer_init, HOOK_PRIO_DEFAULT);

/* state of the simple text tracer */
extern int trace_mode;

/* Index of the next buffer to use inside the 'samples' array */
static uint32_t sp_idx;
/* bitmap of the 'samples' sub-buffer filled with packet binary traces */
static volatile uint32_t filled_pkt;

/* Task to post-process the samples and copy them the USB endpoint buffer */
void sniffer_task(void)
{
	while (1) {
		/* Wait for a new buffer of samples or a new USB free buffer */
		task_wait_event(-1);
		/* send the available samples over USB if we have a buffer*/
		led_reset_record();
	}
}

void sniffer_trace_reload(void)
{
	/* copy a new buffer to send over USB if needed */
}

void sniffer_trace_packet(int head, uint32_t *payload)
{
	uint32_t tstamp = __hw_clock_source_read();
	uint32_t *buf = (uint32_t *)
		&samples[sp_idx >> 4][(sp_idx & 0xF) * EP_PAYLOAD_SIZE];

	buf[0] = tstamp;
	buf[1] = sp_idx | 0xfada0000; /* reserved */
	buf[2] = head;
	memcpy(buf + 3, payload, 7 * sizeof(uint32_t));
	filled_pkt |= 1 << sp_idx;
	sp_idx = (sp_idx + 1) & 31;

	/* copy a new buffer to send over USB if starved */
	if (free_usb == 3)
		sniffer_trace_reload();
}

int wait_packet(int pol, uint32_t min_edges, uint32_t timeout_us)
{
	stm32_dma_chan_t *chan = dma_get_channel(pol ? DMAC_TIM_RX2
						     : DMAC_TIM_RX1);
	uint32_t t0 = __hw_clock_source_read();
	uint32_t c0 = chan->cndtr;
	uint32_t t_gap = t0;
	uint32_t c_gap = c0;
	uint32_t total_edges = 0;

	while (1) {
		uint32_t t = __hw_clock_source_read();
		uint32_t c = chan->cndtr;
		if (t - t0 > timeout_us) /* Timeout */
			break;
		if (min_edges) { /* real packet detection */
			int nb = (int)c_gap - (int)c;
			if (nb < 0)
				nb = RX_COUNT - nb;
			if (nb > 3) { /* NOT IDLE */
				t_gap = t;
				c_gap = c;
				total_edges += nb;
			} else {
				if ((t - t_gap) > 20 &&
				    (total_edges - (t - t0)/256) >= min_edges)
					/* real gap after the packet */
					break;
			}
		}
	}
	return (__hw_clock_source_read() - t0 > timeout_us);
}

uint8_t recording_enable(uint8_t new_mask)
{
	uint8_t old_mask = channel_mask;
	uint8_t diff = channel_mask ^ new_mask;
	/* start/stop RX timers according to the channel mask */
	if (diff & 1) {
		if (new_mask & 1)
			STM32_TIM_CR1(TIM_RX1) |= 1;
		else
			STM32_TIM_CR1(TIM_RX1) &= ~1;
	}
	if (diff & 2) {
		if (new_mask & 2)
			STM32_TIM_CR1(TIM_RX2) |= 1;
		else
			STM32_TIM_CR1(TIM_RX2) &= ~1;
	}
	channel_mask = new_mask;
	return old_mask;
}

static void sniffer_sysjump(void)
{
	/* Stop DMA before jumping to avoid memory corruption */
	recording_enable(0);
}
DECLARE_HOOK(HOOK_SYSJUMP, sniffer_sysjump, HOOK_PRIO_DEFAULT);

static int command_sniffer(int argc, char **argv)
{
	ccprintf("Seq number:%d Overflows: %d\n", seq, oflow);

	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(sniffer, command_sniffer,
			"[]", "Buffering status");
