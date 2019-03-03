/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.

 * Copyright (c) 2019 Jonathan Sieber
 */

/* Twinkie dongle configuration */

#ifndef __CROS_EC_BOARD_H
#define __CROS_EC_BOARD_H

/* 48 MHz SYSCLK clock frequency */
#define CPU_CLOCK 48000000

/* the UART console is on USART1 (PA9/PA10) */
#undef CONFIG_UART_CONSOLE
#define CONFIG_UART_CONSOLE 1

#define CONFIG_USB_POWER_DELIVERY
#define CONFIG_USB_PD_ALT_MODE
#define CONFIG_USB_PD_ALT_MODE_DFP
/*#define CONFIG_USB_PD_DUAL_ROLE*/
#define CONFIG_USB_PD_PORT_COUNT 1
#define CONFIG_USB_PD_TCPC
#define CONFIG_USB_PD_TCPM_STUB

#define CONFIG_USB_PD_INTERNAL_COMP

/* #define CONFIG_USBC_BACKWARDS_COMPATIBLE_DFP*/

#define CONFIG_USB_PD_DEBUG_LEVEL 2

/*
#define CONFIG_USB_PD_VBUS_DETECT_GPIO
#define CONFIG_PD_USE_DAC_AS_REF
*/

#define CONFIG_HW_CRC

#ifndef HAS_TASK_PD_C0 /* PD sniffer mode */
#undef CONFIG_DMA_DEFAULT_HANDLERS
#define CONFIG_USB_PD_TX_PHY_ONLY
/* override the comparator interrupt handler */
#undef CONFIG_USB_PD_RX_COMP_IRQ
#endif

#define CONFIG_ADC
#define CONFIG_BOARD_PRE_INIT
#define CONFIG_CMD_REBOOT_DFU
#define CONFIG_CMD_USB_PD_PE

#define CONFIG_CMD_GPIO_EXTENDED
#define CONFIG_CMD_PD_CONTROL

#define CONFIG_SOFTWARE_PANIC

#define CONFIG_STM_HWTIMER32
#undef CONFIG_WATCHDOG
#undef CONFIG_WATCHDOG_HELP

#undef CONFIG_LID_SWITCH


#undef CONFIG_TASK_PROFILING

/* By default, enable all console messages excepted USB */
#define CC_DEFAULT     (CC_ALL & ~CC_MASK(CC_USB))

/*
 * Allow dangerous commands all the time, since we don't have a write protect
 * switch.
 */
#define CONFIG_SYSTEM_UNLOCKED

#ifndef __ASSEMBLER__

struct rx_header;

void sniffer_init(void);

int wait_packet(int pol, uint32_t min_edges, uint32_t timeout_us);

int expect_packet(int pol, uint8_t cmd, uint32_t timeout_us);

uint8_t recording_enable(uint8_t mask);

void trace_packets(void);

void set_trace_mode(int mode);

/* Timer selection */
#define TIM_ADC       3
/* Timer selection */
#define TIM_CLOCK32 2
#define TIM_WATCHDOG 7



#include "gpio_signal.h"

/* ADC signal */
enum adc_channel {
	ADC_CH_CC1_PD = 0,
	ADC_CH_CC2_PD,
	/* Number of ADC channels */
	ADC_CH_COUNT
};

/* Standard-current Rp */
/*
#define PD_SRC_VNC           PD_SRC_DEF_VNC_MV
*/

#define PD_SRC_VNC           2900


#define PD_SRC_RD_THRESHOLD  PD_SRC_DEF_RD_THRESH_MV

/* delay necessary for the voltage transition on the power supply */
#define PD_POWER_SUPPLY_TURN_ON_DELAY  50000 /* us */
#define PD_POWER_SUPPLY_TURN_OFF_DELAY 50000 /* us */

/* Define typical operating power and max power */
#define PD_OPERATING_POWER_MW 15000
#define PD_MAX_POWER_MW       60000
#define PD_MAX_CURRENT_MA     3000
#define PD_MAX_VOLTAGE_MV     20000

#endif /* !__ASSEMBLER__ */

#endif /* __CROS_EC_BOARD_H */
