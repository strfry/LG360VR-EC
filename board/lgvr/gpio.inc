/* -*- mode:c -*-
 *
 * Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* Declare symbolic names for all the GPIOs that we care about.
 * Note: Those with interrupt handlers must be declared first. */

GPIO(CC1_PD,       PIN(B, 0),  GPIO_ANALOG)
GPIO(CC2_PD,       PIN(B, 1),  GPIO_ANALOG)

GPIO(CC2_TX_DATA,  PIN(A, 6),  GPIO_INPUT)
GPIO(CC1_TX_EN,    PIN(B, 5),  GPIO_ODR_HIGH)

GPIO(CC2_TX_EN,    PIN(B, 2),  GPIO_ODR_HIGH)
GPIO(CC1_TX_DATA,  PIN(B, 4),  GPIO_INPUT)

GPIO(USB_DM,       PIN(A, 11), GPIO_ANALOG)
GPIO(USB_DP,       PIN(A, 12), GPIO_ANALOG)

GPIO(LED_B_L,      PIN(B, 11), GPIO_ODR_HIGH)
GPIO(LED_R_L,      PIN(B, 13), GPIO_ODR_HIGH)
GPIO(LED_G_L,      PIN(B, 14), GPIO_ODR_HIGH)

GPIO(VBUS_EN,    PIN(B, 9), GPIO_OUT_LOW)

GPIO(DP_HPD,    PIN(A, 8), GPIO_ODR_LOW)


/* Unimplemented signals which we need to emulate for now */
UNIMPLEMENTED(ENTERING_RW)
UNIMPLEMENTED(WP_L)

UNIMPLEMENTED(CC1_RPUSB)
UNIMPLEMENTED(CC1_RP3A0)
UNIMPLEMENTED(CC1_RP1A5)
UNIMPLEMENTED(CC1_RA)
UNIMPLEMENTED(CC1_RD)

UNIMPLEMENTED(CC2_RPUSB)
UNIMPLEMENTED(CC2_RP1A5)
UNIMPLEMENTED(CC2_RP3A0)
UNIMPLEMENTED(CC2_RA)
UNIMPLEMENTED(CC2_RD)

ALTERNATE(PIN_MASK(B, 0x0008), 0, MODULE_USB_PD, 0)            /* SPI1: SCK(PB3) */
ALTERNATE(PIN_MASK(B, 0x0100), 2, MODULE_USB_PD, 0)            /* TIM16_CH1: PB8 */
ALTERNATE(PIN_MASK(A, 0x0600), 1, MODULE_UART, GPIO_PULL_UP) /* USART1: PA9/PA10 */
