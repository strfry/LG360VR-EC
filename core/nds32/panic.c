/* Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "common.h"
#include "console.h"
#include "cpu.h"
#include "panic.h"
#include "printf.h"
#include "system.h"
#include "task.h"
#include "timer.h"
#include "util.h"

/* General purpose register (r6) for saving software panic reason */
#define SOFT_PANIC_GPR_REASON 6
/* General purpose register (r7) for saving software panic information */
#define SOFT_PANIC_GPR_INFO   7

/* Panic data goes at the end of RAM. */
static struct panic_data * const pdata_ptr = PANIC_DATA_PTR;

#ifdef CONFIG_DEBUG_EXCEPTIONS
/**
 * bit[4] @ ITYPE, Indicates if an exception is caused by an instruction fetch
 * or a data memory access for the following exceptions:
 * -TLB fill
 * -TLB VLPT miss
 * -TLB read protection
 * -TLB write protection
 * -TLB non-executable page
 * -TLB page modified
 * -TLB Access bit
 * -PTE not present (all)
 * -Reserved PTE Attribute
 * -Alignment check
 * -Branch target alignment
 * -Machine error
 * -Precise bus error
 * -Imprecise bus error
 * -Nonexistent local memory address
 * -MPZIU Control
 * -Cache locking error
 * -TLB locking error
 * -TLB multiple hit
 * -Parity/ECC error
 * All other exceptions not in the abovetable should have the INST field of
 * the ITYPE register set to 0.
 */
static const char * const itype_inst[2] = {
	"a data memory access",
	"an instruction fetch access",
};

/**
 * bit[3-0] @ ITYPE, general exception type information.
 */
static const char * const itype_exc_type[16] = {
	"Alignment check",
	"Reserved instruction",
	"Trap",
	"Arithmetic",
	"Precise bus error",
	"Imprecise bus error",
	"Coprocessor",
	"Privileged instruction",

	"Reserved value",
	"Nonexistent local memory address",
	"MPZIU Control",
	NULL,
	NULL,
	NULL,
	NULL,
	NULL,
};
#endif /* CONFIG_DEBUG_EXCEPTIONS */

#ifdef CONFIG_SOFTWARE_PANIC
/* Software panic reasons */
static const char * const panic_sw_reasons[8] = {
	"PANIC_SW_DIV_ZERO",
	"PANIC_SW_STACK_OVERFLOW",
	"PANIC_SW_PD_CRASH",
	"PANIC_SW_ASSERT",
	"PANIC_SW_WATCHDOG",
	NULL,
	NULL,
	NULL,
};

void software_panic(uint32_t reason, uint32_t info)
{
	asm volatile ("mov55  $r6, %0" : : "r"(reason));
	asm volatile ("mov55  $r7, %0" : : "r"(info));
	if (in_interrupt_context())
		asm("j excep_handler");
	else
		asm("break 0");
	__builtin_unreachable();
}

void panic_set_reason(uint32_t reason, uint32_t info, uint8_t exception)
{
	uint32_t *regs = pdata_ptr->nds_n8.regs;
	uint32_t warning_ipc;

	/* Setup panic data structure */
	if (reason != PANIC_SW_WATCHDOG) {
		memset(pdata_ptr, 0, sizeof(*pdata_ptr));
	} else {
		warning_ipc = pdata_ptr->nds_n8.ipc;
		memset(pdata_ptr, 0, sizeof(*pdata_ptr));
		pdata_ptr->nds_n8.ipc = warning_ipc;
	}
	pdata_ptr->magic = PANIC_DATA_MAGIC;
	pdata_ptr->struct_size = sizeof(*pdata_ptr);
	pdata_ptr->struct_version = 2;
	pdata_ptr->arch = PANIC_ARCH_NDS32_N8;

	/* Log panic cause */
	pdata_ptr->nds_n8.itype = exception;
	regs[SOFT_PANIC_GPR_REASON] = reason;
	regs[SOFT_PANIC_GPR_INFO] = info;
}

void panic_get_reason(uint32_t *reason, uint32_t *info, uint8_t *exception)
{
	uint32_t *regs = pdata_ptr->nds_n8.regs;

	if (pdata_ptr->magic == PANIC_DATA_MAGIC &&
	    pdata_ptr->struct_version == 2) {
		*exception = pdata_ptr->nds_n8.itype;
		*reason = regs[SOFT_PANIC_GPR_REASON];
		*info = regs[SOFT_PANIC_GPR_INFO];
	} else {
		*exception = *reason = *info = 0;
	}
}
#endif /* CONFIG_SOFTWARE_PANIC */

static void print_panic_information(uint32_t *regs, uint32_t itype,
					uint32_t ipc, uint32_t ipsw)
{
	panic_printf("=== EXCEP: ITYPE=%x ===\n", itype);
	panic_printf("R0  %08x R1  %08x R2  %08x R3  %08x\n",
		     regs[0], regs[1], regs[2], regs[3]);
	panic_printf("R4  %08x R5  %08x R6  %08x R7  %08x\n",
		     regs[4], regs[5], regs[6], regs[7]);
	panic_printf("R8  %08x R9  %08x R10 %08x R15 %08x\n",
		     regs[8], regs[9], regs[10], regs[11]);
	panic_printf("FP  %08x GP  %08x LP  %08x SP  %08x\n",
		     regs[12], regs[13], regs[14], regs[15]);
	panic_printf("IPC %08x IPSW   %05x\n", ipc, ipsw);
	if ((ipsw & PSW_INTL_MASK) == (2 << PSW_INTL_SHIFT)) {
		/* 2nd level exception */
		uint32_t oipc;

		asm volatile("mfsr %0, $OIPC" : "=r"(oipc));
		panic_printf("OIPC %08x\n", oipc);
	}

#ifdef CONFIG_DEBUG_EXCEPTIONS
	panic_printf("SWID of ITYPE: %x\n", ((itype >> 16) & 0x7fff));
	if ((regs[SOFT_PANIC_GPR_REASON] & 0xfffffff0) == PANIC_SW_BASE) {
#ifdef CONFIG_SOFTWARE_PANIC
		panic_printf("Software panic reason %s\n",
			panic_sw_reasons[(regs[SOFT_PANIC_GPR_REASON] & 0x7)]);
		panic_printf("Software panic info 0x%x\n",
			regs[SOFT_PANIC_GPR_INFO]);
#endif
	} else {
		panic_printf("Exception type: General exception [%s]\n",
			itype_exc_type[(itype & 0xf)]);
		panic_printf("Exception is caused by %s\n",
			itype_inst[(itype & (1 << 4))]);
	}
#endif
}

void report_panic(uint32_t *regs, uint32_t itype)
{
	int i;
	struct panic_data *pdata = pdata_ptr;

	pdata->magic = PANIC_DATA_MAGIC;
	pdata->struct_size = sizeof(*pdata);
	pdata->struct_version = 2;
	pdata->arch = PANIC_ARCH_NDS32_N8;
	pdata->flags = 0;
	pdata->reserved = 0;

	pdata->nds_n8.itype = itype;
	for (i = 0; i < 16; i++)
		pdata->nds_n8.regs[i] = regs[i];
	pdata->nds_n8.ipc = regs[16];
	pdata->nds_n8.ipsw = regs[17];

	print_panic_information(regs, itype, regs[16], regs[17]);
	panic_reboot();
}

void panic_data_print(const struct panic_data *pdata)
{
	uint32_t itype, *regs, ipc, ipsw;
	itype = pdata->nds_n8.itype;
	regs = (uint32_t *)pdata->nds_n8.regs;
	ipc = pdata->nds_n8.ipc;
	ipsw = pdata->nds_n8.ipsw;

	print_panic_information(regs, itype, ipc, ipsw);
}
