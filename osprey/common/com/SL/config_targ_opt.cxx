/********************************************************************\
|*                                                                  *|   
|*  Copyright (c) 2006 by SimpLight Nanoelectronics.                *|
|*  All rights reserved                                             *|
|*                                                                  *|
|*  This program is free software; you can redistribute it and/or   *|
|*  modify it under the terms of the GNU General Public License as  *|
|*  published by the Free Software Foundation; either version 2,    *|
|*  or (at your option) any later version.                          *|
|*                                                                  *|
\********************************************************************/

/* ====================================================================
 * ====================================================================
 *
 * $Source: /depot/CVSROOT/javi/src/sw/cmplr/common/com/MIPS/config_targ_opt.cxx,v $
 *
 * Revision history:
 *  26-Feb-97 - Original Version, extracted from config.c.
 *
 * Description:
 *
 * Configure the -TARG group (included in config.c).
 * See config_targ_opt.h for usage conventions.
 * See config_targ.* for more general target configuration support.
 *
 * NOTE:  There is an approximate distinction between -TARG option
 * group flags and their configuration (in this file), and more generic
 * target configuration (in config_targ.c).  Note that the related
 * header file config_targ.h is included in config.h, and hence in most
 * source files, whereas config_targ_opt.h is only included directly, so
 * putting new -TARG option-related variables in here is to be
 * preferred to putting them in config_targ.[hc].
 *
 * ====================================================================
 * ====================================================================
 */

/* This file is included in config.c, so it doesn't need its own set of
 * standard includes -- only the following:
 */
#include "config_targ_opt.h"

/* ====================================================================
 * List of global variables that are set by the -TARG option group
 * ====================================================================
 */

/* General target control: */
char *ABI_Name = NULL;		/* -TARG:abi=xxx */
char *ISA_Name = NULL;		/* -TARG:isa=xxx */
char *Processor_Name = NULL;	/* -TARG:processor=xxx */
static char * Platform_Name = NULL;
INT16 Target_FPRs = 0;		/* -TARG:fp_regs=nn */
BOOL Pure_ABI = FALSE;		/* Avoid non-ABI constructs? */

/* Fault handling: */
BOOL Force_FP_Precise_Mode = FALSE;	/* Force precise FP traps? */
BOOL Force_Memory_Dismiss = FALSE;	/* Force mem fault dismissal? */
BOOL Force_Page_Zero = FALSE;		/* Force mapping page zero? */
BOOL Force_SMM = FALSE;			/* Force sequential memory? */
char *FP_Excp_Max = NULL;		/* Max FP trap enables */
char *FP_Excp_Min = NULL;		/* Min FP trap enables */
BOOL Flush_To_Zero = FALSE;		/* suppress fp underflow */

/* Miscellaneous target instruction features: */
#if defined(TARG_SL)
BOOL Madd_Allowed = FALSE;		/* Generate madd instructions? */
#else
BOOL Madd_Allowed = TRUE;		/* Generate madd instructions? */
#endif
BOOL Force_Jalr = FALSE;	/* Force calls via address in register */
static BOOL Slow_CVTDL_Set = FALSE;

BOOL Itanium_a0_step = FALSE;
BOOL SYNC_Allowed = TRUE;
BOOL Slow_CVTDL = FALSE;
char *Endian_Name = NULL;

/* Target machine specification options.  This group defines the target
 * ABI, ISA, processor, and FPR count.  It should also be the home for
 * options like specifying processor revisions.
 */
static OPTION_DESC Options_TARG[] = {
  { OVK_NAME,	OV_VISIBLE,	FALSE, "abi",		"ab",
    0, 0, 0, &ABI_Name,		NULL,
    "Specify the ABI to follow" },
  { OVK_NAME,	OV_VISIBLE,	FALSE, "isa",		"is",
    0, 0, 0, &ISA_Name,		NULL,
    "Specify the instruction set architecture to use" },
  { OVK_SELF,	OV_SHY,		FALSE, "mips1",	NULL,
    0, 0, 0, &ISA_Name,		NULL,
    "Use the MIPS-I instruction set architecture" },
  { OVK_SELF,	OV_SHY,		FALSE, "mips2",	NULL,
    0, 0, 0, &ISA_Name,		NULL,
    "Use the MIPS-II instruction set architecture" },
  { OVK_SELF,	OV_SHY,		FALSE, "mips3",	NULL,
    0, 0, 0, &ISA_Name,		NULL,
    "Use the MIPS-III instruction set architecture" },
  { OVK_SELF,	OV_SHY,		FALSE, "mips4",	NULL,
    0, 0, 0, &ISA_Name,		NULL,
    "Use the MIPS-IV instruction set architecture" },
  { OVK_NAME,	OV_VISIBLE,	FALSE, "platform",	"pl",
    0, 0, 0, &Platform_Name,	NULL,
    "Specify the target platform" },
  { OVK_NAME,	OV_VISIBLE,	FALSE, "processor",	"pr",
    0, 0, 0, &Processor_Name,	NULL,
    "Specify the target microprocessor" },

  /* Miscellaneous features: */
  { OVK_BOOL,	OV_VISIBLE,	FALSE, "dismiss_mem_faults",	"dis",
    0, 0, 0,	&Force_Memory_Dismiss, NULL,
    "Force kernel to ignore memory faults (SIGSEGV/SIGBUS)" },
  { OVK_NAME,	OV_VISIBLE,	FALSE, "exc_max",	"exc_ma",
    0, 0, 0,	&FP_Excp_Max,	NULL,
    "Specify the only floating point exceptions which may be trapped" },
  { OVK_NAME,	OV_VISIBLE,	FALSE, "exc_min",	"exc_mi",
    0, 0, 0,	&FP_Excp_Min,	NULL,
    "Specify any floating point exceptions which must be trapped" },
  { OVK_BOOL,	OV_SHY,		FALSE, "force_jalr",	"",
    0, 0, 0,	&Force_Jalr,	NULL,
    "Force use of JALR instruction for all subprogram calls" },
  { OVK_BOOL,	OV_VISIBLE,	FALSE, "flush_to_zero",	"",
    0, 0, 0,	&Flush_To_Zero, NULL,
    "Suppress floating point underflow exceptions" },
  { OVK_BOOL,	OV_VISIBLE,	FALSE, "fp_precise",	"fp_p",
    0, 0, 0,	&Force_FP_Precise_Mode, NULL,
    "Force the processor into precise floating point mode" },
  { OVK_INT32,	OV_INTERNAL,	FALSE, "fp_regs",	"fp_r",
    32, 16, 32, &Target_FPRs,	NULL,
    "Specify number of FP registers to use (16 or 32)" },
  { OVK_BOOL,	OV_VISIBLE,	FALSE, "madd",		"",
    0, 0, 0,	&Madd_Allowed,	NULL,
    "Specify whether to generate MADD instructions" },
  { OVK_BOOL,	OV_SHY,		FALSE, "page_zero",	"",
    0, 0, 0,	&Force_Page_Zero, NULL,
    "Force the kernel to map page zero into address space" },
  { OVK_BOOL,	OV_INTERNAL,	FALSE, "slow_cvtdl", "",
    0, 0, 0,	&Slow_CVTDL,	&Slow_CVTDL_Set,
    "" },
  { OVK_BOOL,	OV_SHY,		FALSE, "seq_memory",	"seq",
    0, 0, 0,	&Force_SMM,	NULL,
    "Force the processor into sequential memory mode" },
  { OVK_BOOL,	OV_VISIBLE,	FALSE, "sync", "",
    0, 0, 0,	&SYNC_Allowed,	NULL,
    "Specify whether to generate SYNC instructions" },
  { OVK_BOOL,	OV_INTERNAL,	FALSE, "pure",	"pu",
    0, 0, 0,	&Pure_ABI,	NULL,
    "Generate pure ABI-compliant code" },
  { OVK_NAME,	OV_VISIBLE,	FALSE,	"endian", "end", 
    0, 0, 0, 	&Endian_Name, 	NULL, 
    "Endianness of target processor" },

  { OVK_BOOL,   OV_INTERNAL,    FALSE, "ma0_step", "",
    0, 0, 0,    &Itanium_a0_step, NULL,
    "" },

  /* Unimplemented options: */
  /* Obsolete options: */

  { OVK_COUNT }		/* List terminator -- must be last */
};


/* ====================================================================
 *
 * Configure_Source_TARG
 *
 * Same as Configure_Source except here we handle target specifics.
 *
 * ====================================================================
 */
static void
Configure_Source_TARG ( char *filename )
  /**  NOTE: filename CAN BE NULL */
{
}
