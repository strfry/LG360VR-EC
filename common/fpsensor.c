/* Copyright 2017 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "aes.h"
#include "aes-gcm.h"
#include "atomic.h"
#include "clock.h"
#include "common.h"
#include "console.h"
#include "ec_commands.h"
#include "fpsensor.h"
#include "gpio.h"
#include "host_command.h"
#include "link_defs.h"
#include "mkbp_event.h"
#include "rollback.h"
#include "sha256.h"
#include "spi.h"
#include "system.h"
#include "task.h"
#include "trng.h"
#include "timer.h"
#include "util.h"
#include "watchdog.h"

#if !defined(CONFIG_AES) || !defined(CONFIG_AES_GCM) || \
	!defined(CONFIG_ROLLBACK_SECRET_SIZE) || !defined(CONFIG_RNG)
#error "fpsensor requires AES, AES_GCM, ROLLBACK_SECRET_SIZE and RNG"
#endif

#if defined(HAVE_PRIVATE) && !defined(TEST_BUILD)
#define HAVE_FP_PRIVATE_DRIVER
#define PRIV_HEADER(header) STRINGIFY(header)
#include PRIV_HEADER(FP_SENSOR_PRIVATE)
#else
#define FP_SENSOR_IMAGE_SIZE 0
#define FP_SENSOR_RES_X 0
#define FP_SENSOR_RES_Y 0
#define FP_ALGORITHM_TEMPLATE_SIZE 0
#define FP_MAX_FINGER_COUNT 0
#endif
#define SBP_ENC_KEY_LEN 16
#define FP_ALGORITHM_ENCRYPTED_TEMPLATE_SIZE \
	(FP_ALGORITHM_TEMPLATE_SIZE + \
		sizeof(struct ec_fp_template_encryption_metadata))

/* if no special memory regions are defined, fallback on regular SRAM */
#ifndef FP_FRAME_SECTION
#define FP_FRAME_SECTION
#endif
#ifndef FP_TEMPLATE_SECTION
#define FP_TEMPLATE_SECTION
#endif

/* Last acquired frame (aligned as it is used by arbitrary binary libraries) */
static uint8_t fp_buffer[FP_SENSOR_IMAGE_SIZE] FP_FRAME_SECTION __aligned(4);
/* Fingers templates for the current user */
static uint8_t fp_template[FP_MAX_FINGER_COUNT][FP_ALGORITHM_TEMPLATE_SIZE]
	FP_TEMPLATE_SECTION;
/* Encryption/decryption buffer */
/* TODO: On-the-fly encryption/decryption without a dedicated buffer */
/*
 * Store the encryption metadata at the beginning of the buffer containing the
 * ciphered data.
 */
static uint8_t fp_enc_buffer[FP_ALGORITHM_ENCRYPTED_TEMPLATE_SIZE]
	FP_TEMPLATE_SECTION;
/* Number of used templates */
static uint32_t templ_valid;
/* Bitmap of the templates with local modifications */
static uint32_t templ_dirty;
/* Current user ID */
static uint32_t user_id[FP_CONTEXT_USERID_WORDS];
/* Ready to encrypt a template. */
static timestamp_t encryption_deadline;
/* Part of the IKM used to derive encryption keys received from the TPM. */
static uint8_t tpm_seed[FP_CONTEXT_TPM_BYTES];
/* Flag indicating whether the seed has been initialised or not. */
static int fp_tpm_seed_is_set;

#define CPRINTF(format, args...) cprintf(CC_FP, format, ## args)
#define CPRINTS(format, args...) cprints(CC_FP, format, ## args)

/* raw image offset inside the acquired frame */
#ifndef FP_SENSOR_IMAGE_OFFSET
#define FP_SENSOR_IMAGE_OFFSET 0
#endif

/* Events for the FPSENSOR task */
#define TASK_EVENT_SENSOR_IRQ     TASK_EVENT_CUSTOM(1)
#define TASK_EVENT_UPDATE_CONFIG  TASK_EVENT_CUSTOM(2)

#define FP_MODE_ANY_CAPTURE (FP_MODE_CAPTURE | FP_MODE_ENROLL_IMAGE | \
			     FP_MODE_MATCH)
#define FP_MODE_ANY_DETECT_FINGER (FP_MODE_FINGER_DOWN | FP_MODE_FINGER_UP | \
				   FP_MODE_ANY_CAPTURE)
#define FP_MODE_ANY_WAIT_IRQ      (FP_MODE_FINGER_DOWN | FP_MODE_ANY_CAPTURE)

/* Delay between 2 s of the sensor to detect finger removal */
#define FINGER_POLLING_DELAY (100*MSEC)

static uint32_t fp_events;
static uint32_t sensor_mode;

/* Timing statistics. */
static uint32_t capture_time_us;
static uint32_t matching_time_us;
static uint32_t overall_time_us;
static timestamp_t overall_t0;
static uint8_t timestamps_invalid;
static int8_t template_matched;

/* Forward declaration of static function */
static void fp_clear_context(void);

BUILD_ASSERT(sizeof(struct ec_fp_template_encryption_metadata) % 4 == 0);

/* Interrupt line from the fingerprint sensor */
void fps_event(enum gpio_signal signal)
{
	task_set_event(TASK_ID_FPSENSOR, TASK_EVENT_SENSOR_IRQ, 0);
}

static void send_mkbp_event(uint32_t event)
{
	atomic_or(&fp_events, event);
	mkbp_send_event(EC_MKBP_EVENT_FINGERPRINT);
}

static inline int is_raw_capture(uint32_t mode)
{
	int capture_type = FP_CAPTURE_TYPE(mode);

	return (capture_type == FP_CAPTURE_VENDOR_FORMAT
	     || capture_type == FP_CAPTURE_QUALITY_TEST);
}

#ifdef HAVE_FP_PRIVATE_DRIVER
static inline int is_test_capture(uint32_t mode)
{
	int capture_type = FP_CAPTURE_TYPE(mode);

	return (mode & FP_MODE_CAPTURE)
		&& (capture_type == FP_CAPTURE_PATTERN0
		    || capture_type == FP_CAPTURE_PATTERN1
		    || capture_type == FP_CAPTURE_RESET_TEST);
}

/*
 * contains the bit FP_MODE_ENROLL_SESSION if a finger enrollment is on-going.
 * It is used to detect the ENROLL_SESSION transition when sensor_mode is
 * updated by the host.
 */
static uint32_t enroll_session;

static uint32_t fp_process_enroll(void)
{
	int percent = 0;
	int res;

	/* begin/continue enrollment */
	CPRINTS("[%d]Enrolling ...", templ_valid);
	res = fp_finger_enroll(fp_buffer, &percent);
	CPRINTS("[%d]Enroll =>%d (%d%%)", templ_valid, res, percent);
	if (res < 0)
		return EC_MKBP_FP_ENROLL
		     | EC_MKBP_FP_ERRCODE(EC_MKBP_FP_ERR_ENROLL_INTERNAL);
	templ_dirty |= (1 << templ_valid);
	if (percent == 100) {
		res = fp_enrollment_finish(fp_template[templ_valid]);
		if (res)
			res = EC_MKBP_FP_ERR_ENROLL_INTERNAL;
		else
			templ_valid++;
		sensor_mode &= ~FP_MODE_ENROLL_SESSION;
		enroll_session &= ~FP_MODE_ENROLL_SESSION;
	}
	return EC_MKBP_FP_ENROLL | EC_MKBP_FP_ERRCODE(res)
	     | (percent << EC_MKBP_FP_ENROLL_PROGRESS_OFFSET);
}

static uint32_t fp_process_match(void)
{
	timestamp_t t0 = get_time();
	int res = -1;
	uint32_t updated = 0;
	int32_t fgr = -1;

	/* match finger against current templates */
	template_matched = -1;
	CPRINTS("Matching/%d ...", templ_valid);
	if (templ_valid) {
		res = fp_finger_match(fp_template[0], templ_valid, fp_buffer,
				      &fgr, &updated);
		CPRINTS("Match =>%d (finger %d)", res, fgr);
		if (res < 0) {
			res = EC_MKBP_FP_ERR_MATCH_NO_INTERNAL;
			timestamps_invalid |= FPSTATS_MATCHING_INV;
		} else {
			template_matched = (int8_t)fgr;
		}
		if (res == EC_MKBP_FP_ERR_MATCH_YES_UPDATED)
			templ_dirty |= updated;
	} else {
		CPRINTS("No enrolled templates");
		res = EC_MKBP_FP_ERR_MATCH_NO_TEMPLATES;
		timestamps_invalid |= FPSTATS_MATCHING_INV;
	}
	matching_time_us = time_since32(t0);
	return EC_MKBP_FP_MATCH | EC_MKBP_FP_ERRCODE(res)
	| ((fgr << EC_MKBP_FP_MATCH_IDX_OFFSET) & EC_MKBP_FP_MATCH_IDX_MASK);
}

static void fp_process_finger(void)
{
	timestamp_t t0 = get_time();
	int res = fp_sensor_acquire_image_with_mode(fp_buffer,
			FP_CAPTURE_TYPE(sensor_mode));
	capture_time_us = time_since32(t0);
	if (!res) {
		uint32_t evt = EC_MKBP_FP_IMAGE_READY;

		/* Clean up SPI before clocking up to avoid hang on the dsb
		 * in dma_go. Ignore the return value to let the WDT reboot
		 * the MCU (and avoid getting trapped in the loop).
		 * b/112781659 */
		res = spi_transaction_flush(&spi_devices[0]);
		if (res)
			CPRINTS("Failed to flush SPI: 0x%x", res);
		/* we need CPU power to do the computations */
		clock_enable_module(MODULE_FAST_CPU, 1);

		if (sensor_mode & FP_MODE_ENROLL_IMAGE)
			evt = fp_process_enroll();
		else if (sensor_mode & FP_MODE_MATCH)
			evt = fp_process_match();

		sensor_mode &= ~FP_MODE_ANY_CAPTURE;
		overall_time_us = time_since32(overall_t0);
		send_mkbp_event(evt);

		/* go back to lower power mode */
		clock_enable_module(MODULE_FAST_CPU, 0);
	} else {
		timestamps_invalid |= FPSTATS_CAPTURE_INV;
	}
}
#endif /* HAVE_FP_PRIVATE_DRIVER */

void fp_task(void)
{
	int timeout_us = -1;

	/* configure the SPI controller (also ensure that CS_N is high) */
	gpio_config_module(MODULE_SPI_MASTER, 1);
	spi_enable(CONFIG_SPI_FP_PORT, 1);

#ifdef HAVE_FP_PRIVATE_DRIVER
	/* Reset and initialize the sensor IC */
	fp_sensor_init();

	while (1) {
		uint32_t evt;
		enum finger_state st = FINGER_NONE;

		/* Wait for a sensor IRQ or a new mode configuration */
		evt = task_wait_event(timeout_us);

		if (evt & TASK_EVENT_UPDATE_CONFIG) {
			uint32_t mode = sensor_mode;

			gpio_disable_interrupt(GPIO_FPS_INT);
			if ((mode ^ enroll_session) & FP_MODE_ENROLL_SESSION) {
				if (mode & FP_MODE_ENROLL_SESSION) {
					if (fp_enrollment_begin())
						sensor_mode &=
							~FP_MODE_ENROLL_SESSION;
				} else {
					fp_enrollment_finish(NULL);
				}
				enroll_session =
					sensor_mode & FP_MODE_ENROLL_SESSION;
			}
			if (is_test_capture(mode)) {
				fp_sensor_acquire_image_with_mode(fp_buffer,
					FP_CAPTURE_TYPE(mode));
				sensor_mode &= ~FP_MODE_CAPTURE;
				send_mkbp_event(EC_MKBP_FP_IMAGE_READY);
				continue;
			} else if (sensor_mode & FP_MODE_ANY_DETECT_FINGER) {
				/* wait for a finger on the sensor */
				fp_sensor_configure_detect();
			}
			if (sensor_mode & FP_MODE_DEEPSLEEP)
				/* Shutdown the sensor */
				fp_sensor_low_power();
			if (sensor_mode & FP_MODE_FINGER_UP)
				/* Poll the sensor to detect finger removal */
				timeout_us = FINGER_POLLING_DELAY;
			else
				timeout_us = -1;
			if (mode & FP_MODE_ANY_WAIT_IRQ) {
				gpio_enable_interrupt(GPIO_FPS_INT);
			} else if (mode & FP_MODE_RESET_SENSOR) {
				fp_clear_context();
				fp_sensor_init();
				sensor_mode &= ~FP_MODE_RESET_SENSOR;
			} else {
				fp_sensor_low_power();
			}
		} else if (evt & (TASK_EVENT_SENSOR_IRQ | TASK_EVENT_TIMER)) {
			overall_t0 = get_time();
			timestamps_invalid = 0;
			gpio_disable_interrupt(GPIO_FPS_INT);
			if (sensor_mode & FP_MODE_ANY_DETECT_FINGER) {
				st = fp_sensor_finger_status();
				if (st == FINGER_PRESENT &&
				    sensor_mode & FP_MODE_FINGER_DOWN) {
					CPRINTS("Finger!");
					sensor_mode &= ~FP_MODE_FINGER_DOWN;
					send_mkbp_event(EC_MKBP_FP_FINGER_DOWN);
				}
				if (st == FINGER_NONE &&
				    sensor_mode & FP_MODE_FINGER_UP) {
					sensor_mode &= ~FP_MODE_FINGER_UP;
					timeout_us = -1;
					send_mkbp_event(EC_MKBP_FP_FINGER_UP);
				}
			}

			if (st == FINGER_PRESENT &&
			    sensor_mode & FP_MODE_ANY_CAPTURE)
				fp_process_finger();

			if (sensor_mode & FP_MODE_ANY_WAIT_IRQ) {
				fp_sensor_configure_detect();
				gpio_enable_interrupt(GPIO_FPS_INT);
			} else {
				fp_sensor_low_power();
			}
		}
	}
#else /* !HAVE_FP_PRIVATE_DRIVER */
	while (1) {
		uint32_t evt = task_wait_event(timeout_us);

		send_mkbp_event(evt);
	}
#endif /* !HAVE_FP_PRIVATE_DRIVER */
}

static int derive_encryption_key(uint8_t *out_key, uint8_t *salt)
{
	int ret;
	uint8_t key_buf[SHA256_DIGEST_SIZE];
	uint8_t prk[SHA256_DIGEST_SIZE];
	uint8_t message[sizeof(user_id) + 1];
	uint8_t ikm[CONFIG_ROLLBACK_SECRET_SIZE + sizeof(tpm_seed)];

	BUILD_ASSERT(SBP_ENC_KEY_LEN <= SHA256_DIGEST_SIZE);
	BUILD_ASSERT(SBP_ENC_KEY_LEN <= CONFIG_ROLLBACK_SECRET_SIZE);
	BUILD_ASSERT(sizeof(user_id) == SHA256_DIGEST_SIZE);

	if (!fp_tpm_seed_is_set) {
		CPRINTS("Seed hasn't been set.");
		return EC_RES_ERROR;
	}

	/*
	 * The first CONFIG_ROLLBACK_SECRET_SIZE bytes of IKM are read from the
	 * anti-rollback blocks.
	 */
	ret = rollback_get_secret(ikm);
	if (ret != EC_SUCCESS) {
		CPRINTS("Failed to read rollback secret: %d", ret);
		return EC_RES_ERROR;
	}
	/*
	 * IKM is the concatenation of the rollback secret and the seed from
	 * the TPM.
	 */
	memcpy(ikm + CONFIG_ROLLBACK_SECRET_SIZE, tpm_seed, sizeof(tpm_seed));

	/*
	 * Derive a key with the "extract" step of HKDF
	 * https://tools.ietf.org/html/rfc5869#section-2.2
	 */
	hmac_SHA256(prk, salt, FP_CONTEXT_SALT_BYTES, ikm, sizeof(ikm));
	memset(ikm, 0, sizeof(ikm));

	/*
	 * Only 1 "expand" step of HKDF since the size of the "info" context
	 * (user_id in our case) is exactly SHA256_DIGEST_SIZE.
	 * https://tools.ietf.org/html/rfc5869#section-2.3
	 */
	memcpy(message, user_id, sizeof(user_id));
	/* 1 step, set the counter byte to 1. */
	message[sizeof(message) - 1] = 0x01;
	hmac_SHA256(key_buf, prk, sizeof(prk), message, sizeof(message));
	memset(prk, 0, sizeof(prk));

	memcpy(out_key, key_buf, SBP_ENC_KEY_LEN);
	memset(key_buf, 0, sizeof(key_buf));

	return EC_RES_SUCCESS;
}

static void fp_clear_finger_context(int idx)
{
	memset(fp_template[idx], 0, sizeof(fp_template[0]));
}

static void fp_clear_context(void)
{
	int idx;

	templ_valid = 0;
	templ_dirty = 0;
	memset(fp_buffer, 0, sizeof(fp_buffer));
	memset(fp_enc_buffer, 0, sizeof(fp_enc_buffer));
	memset(user_id, 0, sizeof(user_id));
	for (idx = 0; idx < FP_MAX_FINGER_COUNT; idx++)
		fp_clear_finger_context(idx);
	/* TODO maybe shutdown and re-init the private libraries ? */
}

static int fp_get_next_event(uint8_t *out)
{
	uint32_t event_out = atomic_read_clear(&fp_events);

	memcpy(out, &event_out, sizeof(event_out));

	return sizeof(event_out);
}
DECLARE_EVENT_SOURCE(EC_MKBP_EVENT_FINGERPRINT, fp_get_next_event);

static int fp_command_passthru(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_passthru *params = args->params;
	void *out = args->response;
	int rc;
	int ret = EC_RES_SUCCESS;

	if (system_is_locked())
		return EC_RES_ACCESS_DENIED;

	if (params->len > args->params_size +
	    offsetof(struct ec_params_fp_passthru, data) ||
	    params->len > args->response_max)
		return EC_RES_INVALID_PARAM;

	rc = spi_transaction_async(&spi_devices[0], params->data,
				   params->len, out, SPI_READBACK_ALL);
	if (params->flags & EC_FP_FLAG_NOT_COMPLETE)
		rc |= spi_transaction_wait(&spi_devices[0]);
	else
		rc |= spi_transaction_flush(&spi_devices[0]);

	if (rc == EC_ERROR_TIMEOUT)
		ret = EC_RES_TIMEOUT;
	else if (rc)
		ret = EC_RES_ERROR;

	args->response_size = params->len;
	return ret;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_PASSTHRU, fp_command_passthru, EC_VER_MASK(0));

static int validate_fp_mode(const uint32_t mode)
{
	uint32_t capture_type = FP_CAPTURE_TYPE(mode);
	uint32_t algo_mode = mode & ~FP_MODE_CAPTURE_TYPE_MASK;
	uint32_t cur_mode = sensor_mode;

	if (capture_type >= FP_CAPTURE_TYPE_MAX)
		return EC_ERROR_INVAL;

	if (algo_mode & ~FP_VALID_MODES)
		return EC_ERROR_INVAL;

	/* Don't allow sensor reset if any other mode is
	 * set (including FP_MODE_RESET_SENSOR itself). */
	if (mode & FP_MODE_RESET_SENSOR) {
		if (cur_mode & FP_VALID_MODES)
			return EC_ERROR_INVAL;
	}

	return EC_SUCCESS;
}

static int fp_command_mode(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_mode *p = args->params;
	struct ec_response_fp_mode *r = args->response;
	int ret;

	ret = validate_fp_mode(p->mode);
	if (ret != EC_SUCCESS) {
		CPRINTS("Invalid FP mode 0x%x", p->mode);
		return EC_RES_INVALID_PARAM;
	}

	if (!(p->mode & FP_MODE_DONT_CHANGE)) {
		sensor_mode = p->mode;
		task_set_event(TASK_ID_FPSENSOR, TASK_EVENT_UPDATE_CONFIG, 0);
	}

	r->mode = sensor_mode;
	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_MODE, fp_command_mode, EC_VER_MASK(0));

static int fp_command_info(struct host_cmd_handler_args *args)
{
	struct ec_response_fp_info *r = args->response;

#ifdef HAVE_FP_PRIVATE_DRIVER
	if (fp_sensor_get_info(r) < 0)
#endif
		return EC_RES_UNAVAILABLE;

	r->template_size = FP_ALGORITHM_ENCRYPTED_TEMPLATE_SIZE;
	r->template_max = FP_MAX_FINGER_COUNT;
	r->template_valid = templ_valid;
	r->template_dirty = templ_dirty;
	r->template_version = FP_TEMPLATE_FORMAT_VERSION;

	/* V1 is identical to V0 with more information appended */
	args->response_size = args->version ? sizeof(*r) :
			sizeof(struct ec_response_fp_info_v0);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_INFO, fp_command_info,
		     EC_VER_MASK(0) | EC_VER_MASK(1));

BUILD_ASSERT(FP_CONTEXT_NONCE_BYTES == 12);

static int aes_gcm_encrypt(const uint8_t *key, int key_size,
			   const uint8_t *plaintext,
			   uint8_t *ciphertext, int plaintext_size,
			   const uint8_t *nonce, int nonce_size,
			   uint8_t *tag, int tag_size)
{
	int res;
	AES_KEY aes_key;
	GCM128_CONTEXT ctx;

	if (nonce_size != FP_CONTEXT_NONCE_BYTES) {
		CPRINTS("Invalid nonce size %d bytes", nonce_size);
		return EC_RES_INVALID_PARAM;
	}

	res = AES_set_encrypt_key(key, 8 * key_size, &aes_key);
	if (res) {
		CPRINTS("Failed to set encryption key: %d", res);
		return EC_RES_ERROR;
	}
	CRYPTO_gcm128_init(&ctx, &aes_key, (block128_f)AES_encrypt, 0);
	CRYPTO_gcm128_setiv(&ctx, &aes_key, nonce, nonce_size);
	/* CRYPTO functions return 1 on success, 0 on error. */
	res = CRYPTO_gcm128_encrypt(&ctx, &aes_key, plaintext, ciphertext,
				    plaintext_size);
	if (!res) {
		CPRINTS("Failed to encrypt: %d", res);
		return EC_RES_ERROR;
	}
	CRYPTO_gcm128_tag(&ctx, tag, tag_size);
	return EC_RES_SUCCESS;
}

static int aes_gcm_decrypt(const uint8_t *key, int key_size, uint8_t *plaintext,
			   const uint8_t *ciphertext, int plaintext_size,
			   const uint8_t *nonce, int nonce_size,
			   const uint8_t *tag, int tag_size)
{
	int res;
	AES_KEY aes_key;
	GCM128_CONTEXT ctx;

	if (nonce_size != FP_CONTEXT_NONCE_BYTES) {
		CPRINTS("Invalid nonce size %d bytes", nonce_size);
		return EC_RES_INVALID_PARAM;
	}

	res = AES_set_encrypt_key(key, 8 * key_size, &aes_key);
	if (res) {
		CPRINTS("Failed to set decryption key: %d", res);
		return EC_RES_ERROR;
	}
	CRYPTO_gcm128_init(&ctx, &aes_key, (block128_f)AES_encrypt, 0);
	CRYPTO_gcm128_setiv(&ctx, &aes_key, nonce, nonce_size);
	/* CRYPTO functions return 1 on success, 0 on error. */
	res = CRYPTO_gcm128_decrypt(&ctx, &aes_key, ciphertext, plaintext,
				    plaintext_size);
	if (!res) {
		CPRINTS("Failed to decrypt: %d", res);
		return EC_RES_ERROR;
	}
	res = CRYPTO_gcm128_finish(&ctx, tag, tag_size);
	if (!res) {
		CPRINTS("Found incorrect tag: %d", res);
		return EC_RES_ERROR;
	}
	return EC_RES_SUCCESS;
}

static int validate_fp_buffer_offset(const uint32_t buffer_size,
				     const uint32_t offset, const uint32_t size)
{
	if (size > buffer_size || offset > buffer_size ||
	    size + offset > buffer_size)
		return EC_ERROR_INVAL;
	return EC_SUCCESS;
}

static int fp_command_frame(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_frame *params = args->params;
	void *out = args->response;
	uint32_t idx = FP_FRAME_GET_BUFFER_INDEX(params->offset);
	uint32_t offset = params->offset & FP_FRAME_OFFSET_MASK;
	uint32_t size = params->size;
	uint32_t fgr;
	uint8_t key[SBP_ENC_KEY_LEN];
	struct ec_fp_template_encryption_metadata *enc_info;
	int ret;

	if (size > args->response_max)
		return EC_RES_INVALID_PARAM;

	if (idx == FP_FRAME_INDEX_RAW_IMAGE) {
		/* The host requested a frame. */
		if (system_is_locked())
			return EC_RES_ACCESS_DENIED;
		if (!is_raw_capture(sensor_mode))
			offset += FP_SENSOR_IMAGE_OFFSET;

		ret = validate_fp_buffer_offset(sizeof(fp_buffer), offset,
						size);
		if (ret != EC_SUCCESS)
			return EC_RES_INVALID_PARAM;

		memcpy(out, fp_buffer + offset, size);
		args->response_size = size;
		return EC_RES_SUCCESS;
	}

	/* The host requested a template. */

	/* Templates are numbered from 1 in this host request. */
	fgr = idx - FP_FRAME_INDEX_TEMPLATE;

	if (fgr >= FP_MAX_FINGER_COUNT)
		return EC_RES_INVALID_PARAM;
	if (fgr >= templ_valid)
		return EC_RES_UNAVAILABLE;
	ret = validate_fp_buffer_offset(sizeof(fp_enc_buffer), offset, size);
	if (ret != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	if (!offset) {
		/* Host has requested the first chunk, do the encryption. */
		timestamp_t now = get_time();

		/* b/114160734: Not more than 1 encrypted message per second. */
		if (!timestamp_expired(encryption_deadline, &now))
			return EC_RES_BUSY;
		encryption_deadline.val = now.val + (1 * SECOND);

		memset(fp_enc_buffer, 0, sizeof(fp_enc_buffer));
		/* The beginning of the buffer contains nonce/salt/tag. */
		enc_info = (void *)fp_enc_buffer;
		enc_info->struct_version = FP_TEMPLATE_FORMAT_VERSION;
		init_trng();
		rand_bytes(enc_info->nonce, FP_CONTEXT_NONCE_BYTES);
		rand_bytes(enc_info->salt, FP_CONTEXT_SALT_BYTES);
		exit_trng();

		ret = derive_encryption_key(key, enc_info->salt);
		if (ret != EC_RES_SUCCESS) {
			CPRINTS("fgr%d: Failed to derive key", fgr);
			return EC_RES_UNAVAILABLE;
		}

		ret = aes_gcm_encrypt(key, SBP_ENC_KEY_LEN, fp_template[fgr],
				      fp_enc_buffer + sizeof(*enc_info),
				      sizeof(fp_template[0]),
				      enc_info->nonce, FP_CONTEXT_NONCE_BYTES,
				      enc_info->tag, FP_CONTEXT_TAG_BYTES);
		if (ret != EC_RES_SUCCESS) {
			CPRINTS("fgr%d: Failed to encrypt template", fgr);
			return EC_RES_UNAVAILABLE;
		}
		templ_dirty &= ~(1 << fgr);
	}
	memcpy(out, fp_enc_buffer + offset, size);
	args->response_size = size;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_FRAME, fp_command_frame, EC_VER_MASK(0));

static int fp_command_stats(struct host_cmd_handler_args *args)
{
	struct ec_response_fp_stats *r = args->response;

	r->capture_time_us = capture_time_us;
	r->matching_time_us = matching_time_us;
	r->overall_time_us = overall_time_us;
	r->overall_t0.lo = overall_t0.le.lo;
	r->overall_t0.hi = overall_t0.le.hi;
	r->timestamps_invalid = timestamps_invalid;
	r->template_matched = template_matched;

	args->response_size = sizeof(*r);
	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_STATS, fp_command_stats, EC_VER_MASK(0));

static int validate_template_format(
	struct ec_fp_template_encryption_metadata *enc_info)
{
	if (enc_info->struct_version != FP_TEMPLATE_FORMAT_VERSION) {
		CPRINTS("Invalid template format %d", enc_info->struct_version);
		return EC_RES_INVALID_PARAM;
	}
	return EC_RES_SUCCESS;
}

static int fp_command_template(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_template *params = args->params;
	uint32_t size = params->size & ~FP_TEMPLATE_COMMIT;
	int xfer_complete = params->size & FP_TEMPLATE_COMMIT;
	uint32_t offset = params->offset;
	uint32_t idx = templ_valid;
	uint8_t key[SBP_ENC_KEY_LEN];
	struct ec_fp_template_encryption_metadata *enc_info;
	int ret;

	/* Can we store one more template ? */
	if (idx >= FP_MAX_FINGER_COUNT)
		return EC_RES_OVERFLOW;

	if (args->params_size !=
	    size + offsetof(struct ec_params_fp_template, data))
		return EC_RES_INVALID_PARAM;
	ret = validate_fp_buffer_offset(sizeof(fp_enc_buffer), offset, size);
	if (ret != EC_SUCCESS)
		return EC_RES_INVALID_PARAM;

	memcpy(&fp_enc_buffer[offset], params->data, size);

	if (xfer_complete) {
		/*
		 * The complete encrypted template has been received, start
		 * decryption.
		 */
		fp_clear_finger_context(idx);
		/* The beginning of the buffer contains nonce/salt/tag. */
		enc_info = (void *)fp_enc_buffer;
		ret = validate_template_format(enc_info);
		if (ret != EC_RES_SUCCESS) {
			CPRINTS("fgr%d: Template format not supported", idx);
			return EC_RES_INVALID_PARAM;
		}
		ret = derive_encryption_key(key, enc_info->salt);
		if (ret != EC_RES_SUCCESS) {
			CPRINTS("fgr%d: Failed to derive key", idx);
			return EC_RES_UNAVAILABLE;
		}

		ret = aes_gcm_decrypt(key, SBP_ENC_KEY_LEN, fp_template[idx],
				      fp_enc_buffer + sizeof(*enc_info),
				      sizeof(fp_template[0]),
				      enc_info->nonce, FP_CONTEXT_NONCE_BYTES,
				      enc_info->tag, FP_CONTEXT_TAG_BYTES);
		if (ret != EC_RES_SUCCESS) {
			CPRINTS("fgr%d: Failed to decipher template", idx);
			/* Don't leave bad data in the template buffer */
			fp_clear_finger_context(idx);
			return EC_RES_UNAVAILABLE;
		}
		templ_valid++;
	}

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_TEMPLATE, fp_command_template, EC_VER_MASK(0));

static int fp_command_context(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_context *params = args->params;

	fp_clear_context();

	memcpy(user_id, params->userid, sizeof(user_id));

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_CONTEXT, fp_command_context, EC_VER_MASK(0));

static int fp_command_tpm_seed(struct host_cmd_handler_args *args)
{
	const struct ec_params_fp_seed *params = args->params;

	if (params->struct_version != FP_TEMPLATE_FORMAT_VERSION) {
		CPRINTS("Invalid seed format %d", params->struct_version);
		return EC_RES_INVALID_PARAM;
	}

	if (fp_tpm_seed_is_set) {
		CPRINTS("Seed has already been set.");
		return EC_RES_ACCESS_DENIED;
	}
	memcpy(tpm_seed, params->seed, sizeof(tpm_seed));
	fp_tpm_seed_is_set = 1;

	return EC_RES_SUCCESS;
}
DECLARE_HOST_COMMAND(EC_CMD_FP_SEED, fp_command_tpm_seed, EC_VER_MASK(0));

#ifdef CONFIG_CMD_FPSENSOR_DEBUG
/* --- Debug console commands --- */

/*
 * Send the current Fingerprint buffer to the host
 * it is formatted as an 8-bpp PGM ASCII file.
 *
 * In addition, it prepends a short Z-Modem download signature,
 * which triggers automatically your preferred viewer if you configure it
 * properly in "File transfer protocols" in the Minicom options menu.
 * (as triggered by Ctrl-A O)
 * +--------------------------------------------------------------------------+
 * |     Name             Program             Name U/D FullScr IO-Red. Multi  |
 * | A  zmodem     /usr/bin/sz -vv -b          Y    U    N       Y       Y    |
 *  [...]
 * | L  pgm        /usr/bin/display_pgm        N    D    N       Y       N    |
 * | M  Zmodem download string activates... L                                 |
 *
 * My /usr/bin/display_pgm looks like this:
 * #!/bin/sh
 * TMPF=$(mktemp)
 * ascii-xfr -rdv ${TMPF}
 * display ${TMPF}
 */
static void upload_pgm_image(uint8_t *frame)
{
	int x, y;
	uint8_t *ptr = frame;

	/* fake Z-modem ZRQINIT signature */
	ccprintf("#IGNORE for ZModem\r**\030B00");
	msleep(100); /* let the download program start */
	/* Print 8-bpp PGM ASCII header */
	ccprintf("P2\n%d %d\n255\n", FP_SENSOR_RES_X, FP_SENSOR_RES_Y);

	for (y = 0; y < FP_SENSOR_RES_Y; y++) {
		watchdog_reload();
		for (x = 0; x < FP_SENSOR_RES_X; x++, ptr++)
			ccprintf("%d ", *ptr);
		ccputs("\n");
		cflush();
	}

	ccprintf("\x04"); /* End Of Transmission */
}

static int fp_console_action(uint32_t mode)
{
	int tries = 200;
	ccprintf("Waiting for finger ...\n");
	sensor_mode = mode;
	task_set_event(TASK_ID_FPSENSOR, TASK_EVENT_UPDATE_CONFIG, 0);

	while (tries--) {
		if (!(sensor_mode & FP_MODE_ANY_CAPTURE)) {
			ccprintf("done (events:%x)\n", fp_events);
			return 0;
		}
		usleep(100 * MSEC);
	}
	return EC_ERROR_TIMEOUT;
}

int command_fpcapture(int argc, char **argv)
{
	int capture_type = FP_CAPTURE_SIMPLE_IMAGE;
	uint32_t mode;
	int rc;

	if (system_is_locked())
		return EC_RES_ACCESS_DENIED;

	if (argc >= 2) {
		char *e;

		capture_type = strtoi(argv[1], &e, 0);
		if (*e || capture_type < 0)
			return EC_ERROR_PARAM1;
	}
	mode = FP_MODE_CAPTURE | ((capture_type << FP_MODE_CAPTURE_TYPE_SHIFT)
				  & FP_MODE_CAPTURE_TYPE_MASK);

	rc = fp_console_action(mode);
	if (rc == EC_SUCCESS)
		upload_pgm_image(fp_buffer + FP_SENSOR_IMAGE_OFFSET);

	return rc;
}
DECLARE_CONSOLE_COMMAND(fpcapture, command_fpcapture, "", "");

int command_fpenroll(int argc, char **argv)
{
	int rc;
	int percent = 0;
	uint32_t event;
	static const char * const enroll_str[] = {"OK", "Low Quality",
						  "Immobile", "Low Coverage"};

	if (system_is_locked())
		return EC_RES_ACCESS_DENIED;

	do {
		int tries = 1000;

		rc = fp_console_action(FP_MODE_ENROLL_SESSION |
				       FP_MODE_ENROLL_IMAGE);
		if (rc != EC_SUCCESS)
			break;
		event = atomic_read_clear(&fp_events);
		percent = EC_MKBP_FP_ENROLL_PROGRESS(event);
		ccprintf("Enroll capture: %s (%d%%)\n",
			 enroll_str[EC_MKBP_FP_ERRCODE(event) & 3], percent);
		/* wait for finger release between captures */
		sensor_mode = FP_MODE_ENROLL_SESSION | FP_MODE_FINGER_UP;
		task_set_event(TASK_ID_FPSENSOR, TASK_EVENT_UPDATE_CONFIG, 0);
		while (tries-- && sensor_mode & FP_MODE_FINGER_UP)
			usleep(20 * MSEC);
	} while (percent < 100);
	sensor_mode = 0; /* reset FP_MODE_ENROLL_SESSION */
	task_set_event(TASK_ID_FPSENSOR, TASK_EVENT_UPDATE_CONFIG, 0);

	return rc;
}
DECLARE_CONSOLE_COMMAND(fpenroll, command_fpenroll, "", "");


int command_fpmatch(int argc, char **argv)
{
	int rc = fp_console_action(FP_MODE_MATCH);
	uint32_t event = atomic_read_clear(&fp_events);

	if (rc == EC_SUCCESS && event & EC_MKBP_FP_MATCH) {
		uint32_t errcode = EC_MKBP_FP_ERRCODE(event);

		ccprintf("Match: %s (%d)\n",
			 errcode & EC_MKBP_FP_ERR_MATCH_YES ? "YES" : "NO",
			 errcode);
	}

	return rc;
}
DECLARE_CONSOLE_COMMAND(fpmatch, command_fpmatch, "", "");

int command_fpclear(int argc, char **argv)
{
	fp_clear_context();
	return EC_SUCCESS;
}
DECLARE_CONSOLE_COMMAND(fpclear, command_fpclear, "", "");

#endif /* CONFIG_CMD_FPSENSOR_DEBUG */
