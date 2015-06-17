/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#ifndef __CROS_EC_USART_STM32F0_H
#define __CROS_EC_USART_STM32F0_H

#include "usart.h"

#define STM32_USARTS_MAX 4

/*
 * The STM32F0 series can have as many as four UARTS.  These are the HW configs
 * for those UARTS.  They can be used to initialize STM32 generic UART configs.
 */
extern struct usart_hw_config const usart1_hw;
extern struct usart_hw_config const usart2_hw;
extern struct usart_hw_config const usart3_hw;
extern struct usart_hw_config const usart4_hw;

#endif /* __CROS_EC_USART_STM32F0_H */
