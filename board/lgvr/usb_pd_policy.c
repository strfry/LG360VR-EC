/* Copyright (c) 2014 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "config.h"
#include "console.h"
#include "gpio.h"
#include "hooks.h"
#include "registers.h"
#include "task.h"
#include "timer.h"
#include "util.h"
#include "usb_pd.h"

#define CPRINTF(format, args...) cprintf(CC_USBPD, format, ## args)
#define CPRINTS(format, args...) cprints(CC_USBPD, format, ## args)

#define PDO_FIXED_FLAGS (PDO_FIXED_EXTERNAL | PDO_FIXED_DATA_SWAP)

const uint32_t pd_src_pdo[] = {
		PDO_FIXED(5000,  3000, PDO_FIXED_FLAGS),
		PDO_FIXED(12000, 3000, PDO_FIXED_FLAGS),
		PDO_FIXED(20000, 3000, PDO_FIXED_FLAGS),
};
const int pd_src_pdo_cnt = ARRAY_SIZE(pd_src_pdo);

void pd_set_input_current_limit(int port, uint32_t max_ma,
				uint32_t supply_voltage)
{
}

int pd_is_valid_input_voltage(int mv)
{
	/* Any voltage is allowed */
	return 1;
}

void pd_transition_voltage(int idx)
{
}

int pd_set_power_supply_ready(int port)
{
	CPRINTS("PD: enable VBUS");
	gpio_set_level(GPIO_LED_G_L, 0);
	gpio_set_level(GPIO_VBUS_EN_L, 0);
	return EC_SUCCESS; /* we are ready */
}

void pd_power_supply_reset(int port)
{
	CPRINTS("PD: disable VBUS");
	gpio_set_level(GPIO_LED_R_L, 1);
	gpio_set_level(GPIO_LED_G_L, 1);
	gpio_set_level(GPIO_LED_B_L, 1);
	gpio_set_level(GPIO_VBUS_EN_L, 1);
}

int pd_snk_is_vbus_provided(int port)
{
	return (gpio_get_level(GPIO_VBUS_EN_L) == 0);
}

int pd_board_checks(void)
{
	return EC_SUCCESS;
}

int pd_check_power_swap(int port)
{
	/* Always refuse power swap */
	return 0;
}

int pd_check_data_swap(int port, int data_role)
{
	/* Always allow data swap */
	return 1;
}

void pd_check_pr_role(int port, int pr_role, int flags)
{
}

void pd_check_dr_role(int port, int dr_role, int flags)
{
}

void pd_execute_data_swap(int port, int data_role)
{
	/* Do nothing */
}

/* ----------------- Vendor Defined Messages ------------------ */
const struct svdm_response svdm_rsp = {
	.identity = NULL,
	.svids = NULL,
	.modes = NULL,
};

int pd_custom_vdm(int port, int cnt, uint32_t *payload,
		  uint32_t **rpayload)
{
	return 0;
}



static int dp_flags[CONFIG_USB_PD_PORT_COUNT];
static uint32_t dp_status[CONFIG_USB_PD_PORT_COUNT];

static void svdm_safe_dp_mode(int port)
{
	/* make DP interface safe until configure */
	dp_flags[port] = 0;
        dp_status[port] = 0;
}

static int svdm_enter_dp_mode(int port, uint32_t mode_caps)
{
	CPRINTS("DP: enter_dp_mode caps=%x", mode_caps);
	/* Only enter mode if device is DFP_D capable */
	if (mode_caps & MODE_DP_SNK) {
		svdm_safe_dp_mode(port);
		return 0;
	}

	return -1;
}

static int svdm_dp_status(int port, uint32_t *payload)
{
	int opos = pd_alt_mode(port, USB_SID_DISPLAYPORT);
	payload[0] = VDO(USB_SID_DISPLAYPORT, 1,
			 CMD_DP_STATUS | VDO_OPOS(opos));
	payload[1] = VDO_DP_STATUS(0, /* HPD IRQ  ... not applicable */
				   0, /* HPD level ... not applicable */
				   0, /* exit DP? ... no */
				   0, /* usb mode? ... no */
				   0, /* multi-function ... no */
				   (!!(dp_flags[port] & DP_FLAGS_DP_ON)),
				   0, /* power low? ... no */
				   (!!(dp_flags[port] & DP_FLAGS_DP_ON)));

	CPRINTS("DP: dp_status payload0=%x payload1=%x", payload[0], payload[1]);
	return 2;
};

static int svdm_dp_config(int port, uint32_t *payload)
{
	int opos = pd_alt_mode(port, USB_SID_DISPLAYPORT);
	/* board_set_usb_mux(port, TYPEC_MUX_DP, pd_get_polarity(port)); */
	payload[0] = VDO(USB_SID_DISPLAYPORT, 1,
			 CMD_DP_CONFIG | VDO_OPOS(opos));
	payload[1] = VDO_DP_CFG(MODE_DP_PIN_E, /* pin mode */
				1,             /* DPv1.3 signaling */
				2);            /* UFP connected */

	CPRINTS("DP: dp_config payload0=%x payload1=%x", payload[0], payload[1]);
	return 2;
};

static uint64_t hpd_deadline[CONFIG_USB_PD_PORT_COUNT];

static void svdm_dp_post_config(int port)
{
	CPRINTS("DP: post_config");
        dp_flags[port] |= DP_FLAGS_DP_ON;
        if (!(dp_flags[port] & DP_FLAGS_HPD_HI_PENDING))
                return;

        gpio_set_level(GPIO_DP_HPD, 1);
	gpio_set_level(GPIO_LED_B_L, 0);

        /* set the minimum time delay (2ms) for the next HPD IRQ */
        hpd_deadline[port] = get_time().val + HPD_USTREAM_DEBOUNCE_LVL;
}

static int svdm_dp_attention(int port, uint32_t *payload)
{
        int cur_lvl;
        int lvl = PD_VDO_DPSTS_HPD_LVL(payload[1]);
        int irq = PD_VDO_DPSTS_HPD_IRQ(payload[1]);
        enum gpio_signal hpd = GPIO_DP_HPD;

        cur_lvl = gpio_get_level(hpd);
        dp_status[port] = payload[1];

        /* Its initial DP status message prior to config */
        if (!(dp_flags[port] & DP_FLAGS_DP_ON)) {
                if (lvl)
                        dp_flags[port] |= DP_FLAGS_HPD_HI_PENDING;
                return 1;
        }

        if (irq & cur_lvl) {
                uint64_t now = get_time().val;
                /* wait for the minimum spacing between IRQ_HPD if needed */
                if (now < hpd_deadline[port])
                        usleep(hpd_deadline[port] - now);

                /* generate IRQ_HPD pulse */
                gpio_set_level(hpd, 0);
		gpio_set_level(GPIO_LED_B_L, 1);
                usleep(HPD_DSTREAM_DEBOUNCE_IRQ);
                gpio_set_level(hpd, 1);
		gpio_set_level(GPIO_LED_B_L, 0);

                /* set the minimum time delay (2ms) for the next HPD IRQ */
                hpd_deadline[port] = get_time().val + HPD_USTREAM_DEBOUNCE_LVL;
        } else if (irq & !cur_lvl) {
                CPRINTF("ERR:HPD:IRQ&LOW\n");
                return 0; /* nak */
        } else {
                gpio_set_level(hpd, lvl);
		gpio_set_level(GPIO_LED_B_L, !lvl);
                /* set the minimum time delay (2ms) for the next HPD IRQ */
                hpd_deadline[port] = get_time().val + HPD_USTREAM_DEBOUNCE_LVL;
        }

	/* ack */
	return 1;
}

static void svdm_exit_dp_mode(int port)
{
	CPRINTS("DP: exit_dp_mode");
	svdm_safe_dp_mode(port);
	gpio_set_level(GPIO_DP_HPD, 0);
	gpio_set_level(GPIO_LED_B_L, 1);
}

const struct svdm_amode_fx supported_modes[] = {
	{
		.svid = USB_SID_DISPLAYPORT,
		.enter = &svdm_enter_dp_mode,
		.status = &svdm_dp_status,
		.config = &svdm_dp_config,
		.post_config = &svdm_dp_post_config,
		.attention = &svdm_dp_attention,
		.exit = &svdm_exit_dp_mode,
	},
};
const int supported_modes_cnt = ARRAY_SIZE(supported_modes);
