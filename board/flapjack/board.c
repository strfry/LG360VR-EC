/* Copyright 2019 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "adc_chip.h"
#include "backlight.h"
#include "button.h"
#include "charge_manager.h"
#include "charge_state.h"
#include "charger.h"
#include "chipset.h"
#include "common.h"
#include "console.h"
#include "cros_board_info.h"
#include "driver/accelgyro_bmi160.h"
#include "driver/als_opt3001.h"
#include "driver/battery/max17055.h"
#include "driver/charger/rt946x.h"
#include "driver/sync.h"
#include "driver/tcpm/mt6370.h"
#include "driver/temp_sensor/tmp432.h"
#include "ec_commands.h"
#include "extpower.h"
#include "gpio.h"
#include "hooks.h"
#include "host_command.h"
#include "i2c.h"
#include "power.h"
#include "power_button.h"
#include "pwm.h"
#include "pwm_chip.h"
#include "registers.h"
#include "spi.h"
#include "switch.h"
#include "system.h"
#include "task.h"
#include "tcpm.h"
#include "temp_sensor.h"
#include "temp_sensor_chip.h"
#include "thermal.h"
#include "timer.h"
#include "usb_charge.h"
#include "usb_mux.h"
#include "usb_pd_tcpm.h"
#include "util.h"

#define CPRINTS(format, args...) cprints(CC_USBCHARGE, format, ## args)
#define CPRINTF(format, args...) cprintf(CC_USBCHARGE, format, ## args)

uint16_t board_version;
uint8_t oem;
uint32_t sku;

static void board_setup_panel(void)
{
	uint8_t channel;
	uint8_t dim;
	int rv = 0;

	channel = sku & SKU_ID_PANEL_SIZE_MASK ? 0xfe : 0xfa;
	dim = sku & SKU_ID_PANEL_SIZE_MASK ? 0xc4 : 0xc8;

	rv |= i2c_write8(I2C_PORT_CHARGER, RT946X_ADDR, MT6370_BACKLIGHT_BLEN,
		channel);
	rv |= i2c_write8(I2C_PORT_CHARGER, RT946X_ADDR, MT6370_BACKLIGHT_BLDIM,
		dim);
	rv |= i2c_write8(I2C_PORT_CHARGER, RT946X_ADDR, MT6370_BACKLIGHT_BLPWM,
		0xac);
	if (rv)
		CPRINTS("Board setup panel failed\n");
}

static void cbi_init(void)
{
	uint32_t val;

	if (cbi_get_board_version(&val) == EC_SUCCESS && val <= UINT16_MAX)
		board_version = val;
	CPRINTS("Board Version: 0x%04x", board_version);

	if (cbi_get_oem_id(&val) == EC_SUCCESS && val <= PROJECT_COUNT)
		oem = val;
	CPRINTS("OEM: %d", oem);

	if (cbi_get_sku_id(&val) == EC_SUCCESS)
		sku = val;
	CPRINTS("SKU: 0x%08x", sku);
}
DECLARE_HOOK(HOOK_INIT, cbi_init, HOOK_PRIO_INIT_I2C + 1);

static void tcpc_alert_event(enum gpio_signal signal)
{
	schedule_deferred_pd_interrupt(0 /* port */);
}

static void hall_interrupt(enum gpio_signal signal)
{
	/* TODO(b/111378000): Implement hall_interrupt */
}

static void gauge_interrupt(enum gpio_signal signal)
{
	task_wake(TASK_ID_CHARGER);
}

#include "gpio_list.h"

/******************************************************************************/
/* ADC channels. Must be in the exactly same order as in enum adc_channel. */
const struct adc_t adc_channels[] = {
	[ADC_BOARD_ID] = {"BOARD_ID", 3300, 4096, 0, STM32_AIN(10)},
	[ADC_EC_SKU_ID] = {"EC_SKU_ID", 3300, 4096, 0, STM32_AIN(8)},
	[ADC_BATT_ID] = {"BATT_ID", 3300, 4096, 0, STM32_AIN(7)},
	[ADC_USBC_THERM] = {"USBC_THERM", 3300, 4096, 0, STM32_AIN(14)},
};
BUILD_ASSERT(ARRAY_SIZE(adc_channels) == ADC_CH_COUNT);

/******************************************************************************/
/* I2C ports */
const struct i2c_port_t i2c_ports[] = {
	{"charger",   I2C_PORT_CHARGER,   400, GPIO_I2C1_SCL, GPIO_I2C1_SDA},
	{"tcpc0",     I2C_PORT_TCPC0,     400, GPIO_I2C1_SCL, GPIO_I2C1_SDA},
	{"battery",   I2C_PORT_BATTERY,   400, GPIO_I2C2_SCL, GPIO_I2C2_SDA},
	{"accelgyro", I2C_PORT_ACCEL,     400, GPIO_I2C2_SCL, GPIO_I2C2_SDA},
	{"eeprom",    I2C_PORT_EEPROM,    400, GPIO_I2C2_SCL, GPIO_I2C2_SDA},
};
const unsigned int i2c_ports_used = ARRAY_SIZE(i2c_ports);

/* power signal list.  Must match order of enum power_signal. */
const struct power_signal_info power_signal_list[] = {
	{GPIO_AP_IN_SLEEP_L,   POWER_SIGNAL_ACTIVE_LOW,  "AP_IN_S3_L"},
	{GPIO_PMIC_EC_RESETB,  POWER_SIGNAL_ACTIVE_HIGH, "PMIC_PWR_GOOD"},
};
BUILD_ASSERT(ARRAY_SIZE(power_signal_list) == POWER_SIGNAL_COUNT);

#ifdef CONFIG_TEMP_SENSOR_TMP432
/* Temperature sensors data; must be in same order as enum temp_sensor_id. */
const struct temp_sensor_t temp_sensors[] = {
	{"TMP432_Internal", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
		TMP432_IDX_LOCAL, 4},
	{"TMP432_Sensor_1", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
		TMP432_IDX_REMOTE1, 4},
	{"TMP432_Sensor_2", TEMP_SENSOR_TYPE_BOARD, tmp432_get_val,
		TMP432_IDX_REMOTE2, 4},
};
BUILD_ASSERT(ARRAY_SIZE(temp_sensors) == TEMP_SENSOR_COUNT);

/*
 * Thermal limits for each temp sensor. All temps are in degrees K. Must be in
 * same order as enum temp_sensor_id. To always ignore any temp, use 0.
 */
struct ec_thermal_config thermal_params[] = {
	{{0, 0, 0}, 0, 0}, /* TMP432_Internal */
	{{0, 0, 0}, 0, 0}, /* TMP432_Sensor_1 */
	{{0, 0, 0}, 0, 0}, /* TMP432_Sensor_2 */
};
BUILD_ASSERT(ARRAY_SIZE(thermal_params) == TEMP_SENSOR_COUNT);
#endif

/******************************************************************************/
/* SPI devices */
const struct spi_device_t spi_devices[] = {
};
const unsigned int spi_devices_used = ARRAY_SIZE(spi_devices);

/******************************************************************************/
const struct tcpc_config_t tcpc_config[CONFIG_USB_PD_PORT_COUNT] = {
	{I2C_PORT_TCPC0, MT6370_TCPC_I2C_ADDR, &mt6370_tcpm_drv},
};

struct usb_mux usb_muxes[CONFIG_USB_PD_PORT_COUNT] = {
	{
		.driver = &virtual_usb_mux_driver,
		.hpd_update = &virtual_hpd_update,
	},
};

void board_reset_pd_mcu(void)
{
}

uint16_t tcpc_get_alert_status(void)
{
	uint16_t status = 0;

	if (!gpio_get_level(GPIO_USB_C0_PD_INT_ODL))
		status |= PD_STATUS_TCPC_ALERT_0;

	return status;
}

int board_set_active_charge_port(int charge_port)
{
	CPRINTS("New chg p%d", charge_port);

	switch (charge_port) {
	case 0:
		/* Don't charge from a source port */
		if (board_vbus_source_enabled(charge_port))
			return -1;
		break;
	case CHARGE_PORT_NONE:
		/*
		 * To ensure the fuel gauge (max17055) is always powered
		 * even when battery is disconnected, keep VBAT rail on but
		 * set the charging current to minimum.
		 */
		charger_set_current(0);
		break;
	default:
		panic("Invalid charge port\n");
		break;
	}

	return EC_SUCCESS;
}

void board_set_charge_limit(int port, int supplier, int charge_ma,
			    int max_ma, int charge_mv)
{
	charge_set_input_current_limit(MAX(charge_ma,
			       CONFIG_CHARGER_INPUT_CURRENT), charge_mv);
}

int extpower_is_present(void)
{
	/*
	 * The charger will indicate VBUS presence if we're sourcing 5V,
	 * so exclude such ports.
	 */
	if (board_vbus_source_enabled(0))
		return 0;
	else
		return tcpm_get_vbus_level(0);
}

int pd_snk_is_vbus_provided(int port)
{
	if (port)
		panic("Invalid charge port\n");

	return rt946x_is_vbus_ready();
}

static void board_init(void)
{
	/* Set SPI1 PB13/14/15 pins to high speed */
	STM32_GPIO_OSPEEDR(GPIO_B) |= 0xfc000000;

	/* Enable TCPC alert interrupts */
	gpio_enable_interrupt(GPIO_USB_C0_PD_INT_ODL);

	/* Enable charger interrupts */
	gpio_enable_interrupt(GPIO_CHARGER_INT_ODL);

#ifdef SECTION_IS_RW
	/* Enable interrupts from BMI160 sensor. */
	gpio_enable_interrupt(GPIO_ACCEL_INT_ODL);

	/* Enable interrupt for the camera vsync. */
	gpio_enable_interrupt(GPIO_SYNC_INT);
#endif /* SECTION_IS_RW */

	/* Enable interrupt from PMIC. */
	gpio_enable_interrupt(GPIO_PMIC_EC_RESETB);

	/* Enable gauge interrupt from max17055 */
	gpio_enable_interrupt(GPIO_GAUGE_INT_ODL);
	board_setup_panel();
}
DECLARE_HOOK(HOOK_INIT, board_init, HOOK_PRIO_DEFAULT);

void board_config_pre_init(void)
{
	STM32_RCC_AHBENR |= STM32_RCC_HB_DMA1;
	/*
	 * Remap USART1 and SPI2 DMA:
	 *
	 * Ch4: USART1_TX / Ch5: USART1_RX (1000)
	 * Ch6: SPI2_RX / Ch7: SPI2_TX (0011)
	 */
	STM32_DMA_CSELR(STM32_DMAC_CH4) = (8 << 12) | (8 << 16) |
					  (3 << 20) | (3 << 24);
}

enum kukui_board_version {
	BOARD_VERSION_UNKNOWN = -1,
	BOARD_VERSION_REV0 = 0,
	BOARD_VERSION_REV1 = 1,
	BOARD_VERSION_REV2 = 2,
	BOARD_VERSION_REV3 = 3,
	BOARD_VERSION_REV4 = 4,
	BOARD_VERSION_REV5 = 5,
	BOARD_VERSION_REV6 = 6,
	BOARD_VERSION_REV7 = 7,
	BOARD_VERSION_REV8 = 8,
	BOARD_VERSION_REV9 = 9,
	BOARD_VERSION_REV10 = 10,
	BOARD_VERSION_REV11 = 11,
	BOARD_VERSION_REV12 = 12,
	BOARD_VERSION_REV13 = 13,
	BOARD_VERSION_REV14 = 14,
	BOARD_VERSION_REV15 = 15,
	BOARD_VERSION_COUNT,
};

struct {
	enum kukui_board_version version;
	int expect_mv;
} const kukui_boards[] = {
	{ BOARD_VERSION_REV0, 109 },   /* 51.1K , 2.2K(gru 3.3K) ohm */
	{ BOARD_VERSION_REV1, 211 },   /* 51.1k , 6.8K ohm */
	{ BOARD_VERSION_REV2, 319 },   /* 51.1K , 11K ohm */
	{ BOARD_VERSION_REV3, 427 },   /* 56K   , 17.4K ohm */
	{ BOARD_VERSION_REV4, 542 },   /* 51.1K , 22K ohm */
	{ BOARD_VERSION_REV5, 666 },   /* 51.1K , 30K ohm */
	{ BOARD_VERSION_REV6, 781 },   /* 51.1K , 39.2K ohm */
	{ BOARD_VERSION_REV7, 900 },   /* 56K   , 56K ohm */
	{ BOARD_VERSION_REV8, 1023 },  /* 47K   , 61.9K ohm */
	{ BOARD_VERSION_REV9, 1137 },  /* 47K   , 80.6K ohm */
	{ BOARD_VERSION_REV10, 1240 }, /* 56K   , 124K ohm */
	{ BOARD_VERSION_REV11, 1343 }, /* 51.1K , 150K ohm */
	{ BOARD_VERSION_REV12, 1457 }, /* 47K   , 200K ohm */
	{ BOARD_VERSION_REV13, 1576 }, /* 47K   , 330K ohm */
	{ BOARD_VERSION_REV14, 1684 }, /* 47K   , 680K ohm */
	{ BOARD_VERSION_REV15, 1800 }, /* 56K   , NC */
};
BUILD_ASSERT(ARRAY_SIZE(kukui_boards) == BOARD_VERSION_COUNT);

#define THRESHOLD_MV 56 /* Simply assume 1800/16/2 */

int board_get_version(void)
{
	static int version = BOARD_VERSION_UNKNOWN;
	int mv;
	int i;

	if (version != BOARD_VERSION_UNKNOWN)
		return version;

	gpio_set_level(GPIO_EC_BOARD_ID_EN_L, 0);
	/* Wait to allow cap charge */
	msleep(10);
	mv = adc_read_channel(ADC_BOARD_ID);

	if (mv == ADC_READ_ERROR)
		mv = adc_read_channel(ADC_BOARD_ID);

	gpio_set_level(GPIO_EC_BOARD_ID_EN_L, 1);

	for (i = 0; i < BOARD_VERSION_COUNT; ++i) {
		if (mv < kukui_boards[i].expect_mv + THRESHOLD_MV) {
			version = kukui_boards[i].version;
			break;
		}
	}

	return version;
}

/* Motion sensors */
/* Mutexes */
#ifdef SECTION_IS_RW
static struct mutex g_lid_mutex;

static struct bmi160_drv_data_t g_bmi160_data;
static struct opt3001_drv_data_t g_opt3001_data = {
	.scale = 1,
	.uscale = 0,
	.offset = 0,
};

/* Matrix to rotate accelerometer into standard reference frame */
const mat33_fp_t lid_standard_ref = {
	{ 0,  FLOAT_TO_FP(-1), 0},
	{ FLOAT_TO_FP(-1), 0,  0},
	{ 0,  0, FLOAT_TO_FP(-1)}
};

struct motion_sensor_t motion_sensors[] = {
	/*
	 * Note: bmi160: supports accelerometer and gyro sensor
	 * Requirement: accelerometer sensor must init before gyro sensor
	 * DO NOT change the order of the following table.
	 */
	[LID_ACCEL] = {
	 .name = "Accel",
	 .active_mask = SENSOR_ACTIVE_S0_S3,
	 .chip = MOTIONSENSE_CHIP_BMI160,
	 .type = MOTIONSENSE_TYPE_ACCEL,
	 .location = MOTIONSENSE_LOC_LID,
	 .drv = &bmi160_drv,
	 .mutex = &g_lid_mutex,
	 .drv_data = &g_bmi160_data,
	 .port = I2C_PORT_ACCEL,
	 .addr = BMI160_ADDR0,
	 .rot_standard_ref = &lid_standard_ref,
	 .default_range = 4,  /* g */
	 .min_frequency = BMI160_ACCEL_MIN_FREQ,
	 .max_frequency = BMI160_ACCEL_MAX_FREQ,
	 .config = {
		 /* Enable accel in S0 */
		 [SENSOR_CONFIG_EC_S0] = {
			 .odr = 10000 | ROUND_UP_FLAG,
			 .ec_rate = 100 * MSEC,
		 },
	 },
	},
	[LID_GYRO] = {
	 .name = "Gyro",
	 .active_mask = SENSOR_ACTIVE_S0_S3,
	 .chip = MOTIONSENSE_CHIP_BMI160,
	 .type = MOTIONSENSE_TYPE_GYRO,
	 .location = MOTIONSENSE_LOC_LID,
	 .drv = &bmi160_drv,
	 .mutex = &g_lid_mutex,
	 .drv_data = &g_bmi160_data,
	 .port = I2C_PORT_ACCEL,
	 .addr = BMI160_ADDR0,
	 .default_range = 1000, /* dps */
	 .rot_standard_ref = &lid_standard_ref,
	 .min_frequency = BMI160_GYRO_MIN_FREQ,
	 .max_frequency = BMI160_GYRO_MAX_FREQ,
	},
	[LID_ALS] = {
	.name = "Light",
	.active_mask = SENSOR_ACTIVE_S0_S3,
	.chip = MOTIONSENSE_CHIP_OPT3001,
	.type = MOTIONSENSE_TYPE_LIGHT,
	.location = MOTIONSENSE_LOC_LID,
	.drv = &opt3001_drv,
	.drv_data = &g_opt3001_data,
	.port = I2C_PORT_ALS,
	.addr = OPT3001_I2C_ADDR1,
	.rot_standard_ref = NULL,
	.default_range = 0x10000, /* scale = 1; uscale = 0 */
	.min_frequency = OPT3001_LIGHT_MIN_FREQ,
	.max_frequency = OPT3001_LIGHT_MAX_FREQ,
	.config = {
		[SENSOR_CONFIG_EC_S0] = {
			.odr = 1000,
		},
	 },
	},
	[VSYNC] = {
	 .name = "Camera vsync",
	 .active_mask = SENSOR_ACTIVE_S0,
	 .chip = MOTIONSENSE_CHIP_GPIO,
	 .type = MOTIONSENSE_TYPE_SYNC,
	 .location = MOTIONSENSE_LOC_CAMERA,
	 .drv = &sync_drv,
	 .default_range = 0,
	 .min_frequency = 0,
	 .max_frequency = 1,
	},
};
const unsigned int motion_sensor_count = ARRAY_SIZE(motion_sensors);
const struct motion_sensor_t *motion_als_sensors[] = {
	&motion_sensors[LID_ALS],
};
BUILD_ASSERT(ARRAY_SIZE(motion_als_sensors) == ALS_COUNT);
#endif /* SECTION_IS_RW */

int board_allow_i2c_passthru(int port)
{
	return (port == I2C_PORT_VIRTUAL_BATTERY);
}

void usb_charger_set_switches(int port, enum usb_switch setting)
{
}
