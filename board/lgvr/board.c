/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
/* LGVR Adapter configuration */

#include "adc.h"
#include "adc_chip.h"
#include "common.h"
#include "console.h"
#include "ec_version.h"
#include "gpio.h"
#include "hooks.h"
#include "i2c.h"
#include "printf.h"
#include "registers.h"
#include "system.h"
#include "task.h"
#include "usb_descriptor.h"
#include "util.h"

void cc2_event(enum gpio_signal signal)
{
	ccprintf("INA!\n");
}

void vbus_event(enum gpio_signal signal)
{
	ccprintf("INA!\n");
}

#include "gpio_list.h"

/* Initialize board. */
void board_config_pre_init(void)
{
	/* enable SYSCFG clock */
	STM32_RCC_APB2ENR |= 1 << 0;
	/* Remap USART DMA to match the USART driver and TIM2 DMA */
	STM32_SYSCFG_CFGR1 |= (1 << 9) | (1 << 10) /* Remap USART1 RX/TX DMA */
			   |  (1 << 29);/* Remap TIM2 DMA */
	/* 40 MHz pin speed on UART PA9/PA10 */
	STM32_GPIO_OSPEEDR(GPIO_A) |= 0x003C0000;
	/* 40 MHz pin speed on TX clock out PB9 */
	STM32_GPIO_OSPEEDR(GPIO_B) |= 0x000C0000;
}

static void board_init(void)
{
	/* Signal startup via LEDs */

}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

/* ADC channels */
const struct adc_t adc_channels[] = {
	/* USB PD CC lines sensing. Converted to mV (3300mV/4096). */
	[ADC_CH_CC1_PD] = {"CC1_PD", 3300, 4096, 0, STM32_AIN(1)},
	[ADC_CH_CC2_PD] = {"CC2_PD", 3300, 4096, 0, STM32_AIN(9)},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

