//-*-c++-*-

/*
 * Copyright 2003, 2004, 2005 PathScale, Inc.  All Rights Reserved.
 */

// ====================================================================
// ====================================================================
//
// Module: profile_interface.h
//
// ====================================================================
//
// Copyright (C) 2000, 2001 Silicon Graphics, Inc.  All Rights Reserved.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of version 2 of the GNU General Public License as
// published by the Free Software Foundation.
//
// This program is distributed in the hope that it would be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
//
// Further, this software is distributed without any warranty that it
// is free of the rightful claim of any third person regarding
// infringement  or the like.  Any license provided herein, whether
// implied or otherwise, applies only to this software file.  Patent
// licenses, if any, provided herein do not apply to combinations of
// this program with other software, or any other product whatsoever.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write the Free Software Foundation,
// Inc., 59 Temple Place - Suite 330, Boston MA 02111-1307, USA.
//
// Contact information:  Silicon Graphics, Inc., 1600 Amphitheatre Pky,
// Mountain View, CA 94043, or:
//
// http://www.sgi.com
//
// For further information regarding this notice, see:
//
// http://oss.sgi.com/projects/GenInfo/NoticeExplan
//
// ====================================================================
//
// Description:
//
// During instrumentation, calls to the following procedures are
// inserted into the WHIRL code.  When invoked, these procedures
// initialize, perform, and finalize frequency counts.
//
// ====================================================================
// ====================================================================


#ifndef profile_interface_INCLUDED
#define profile_interface_INCLUDED


// The calls to these routines are generated by the compiler during
// instrumentation. Since the names have to match the compiler
// generated names, we need extern C to prevent name mangling.

#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */
// Subtypes for the different _profile_type(..) calls.
 
typedef enum 
   { 
     IF_THEN_SUBTYPE=1,
     IF_THEN_ELSE_SUBTYPE=2,
     IF_UNK_SUBTYPE=3,
     TRUEBR_SUBTYPE=4,
     FALSEBR_SUBTYPE=5,
     CSELECT_SUBTYPE=6
   } BranchSubType;

typedef enum
  {   
    DO_LOOP_SUBTYPE=1,
    WHILE_DO_SUBTYPE=2,
    DO_WHILE_SUBTYPE=3

  } LoopSubType;

typedef enum
  {
    PICCALL_SUBTYPE=1,
    CALL_SUBTYPE=2,
    INTRINSIC_CALL_SUBTYPE=3,
    IO_SUBTYPE=4

  } CallSubType;

typedef enum
  {
    CAND_SUBTYPE=1,   
    CIOR_SUBTYPE=2

  } CircuitSubType;

typedef enum
  { 
    COMPGOTO_SUBTYPE=1,
    XGOTO_SUBTYPE=2
  } GotoSubType; 

// ====================================================================
struct profile_init_struct
{
  char *output_filename;
  INT32 phase_num;
  BOOL unique_name; 
};



struct profile_gen_struct
{
  void *pu_handle;
  char *file_name; 
  char *pu_name;
  INT32 id;
  INT32 linenum; 
  INT32 endline;
  INT32 subtype;
  INT32 taken; 
  char *callee;
  INT32 target;
  INT32 num_targets;
  void *called_fun_address;  
  void *data;
};


// One time initialization

extern void __profile_init(struct profile_init_struct *d); 

// PU level initializations

extern void __profile_invoke(struct profile_gen_struct *d);
extern void __profile_invoke_exit(struct profile_gen_struct *d);


// Profile routines for conditional branches
extern void __profile_branch(struct profile_gen_struct *d);
extern void __profile_branch_exit(struct profile_gen_struct *d);

// Profile routines for switches
extern void __profile_switch(struct profile_gen_struct *d);
extern void __profile_switch_exit(struct profile_gen_struct *d);


// Profile routines for compgotos

extern void __profile_compgoto(struct profile_gen_struct *d);


// Profile routines for loops

extern void __profile_loop(struct profile_gen_struct *d);
extern void __profile_loop_iter(struct profile_gen_struct *d);
extern void __profile_loop_exit(struct profile_gen_struct *d);


// Profile routines for short circuiting

extern void __profile_short_circuit(struct profile_gen_struct *d);


// Profile routines for calls

extern void __profile_call_entry(struct profile_gen_struct *d);
extern void __profile_call_exit(struct profile_gen_struct *d);

//Profile routines for icalls

  
#ifdef KEY
  // Profile routines for value
  extern void __profile_value_init(void *pu_handle, int num_values);
  extern void __profile_value(void * pu_handle, int inst_id, INT64 value );

  // Profile routines for value_fp_bin
  extern void __profile_value_fp_bin_init(void *pu_handle, int num_values);
  extern void __profile_value_fp_bin(void * pu_handle, int inst_id, 
				     double value_fp_0, double value_fp_1 );
#endif

extern void __profile_icall_entry(struct profile_gen_struct *d);
extern void __profile_icall_exit(struct profile_gen_struct *d);
// PU level cleanup 

extern void __profile_finish(void);


// ====================================================================


#ifdef __cplusplus
}
#endif /* __cplusplus */


#endif /* profile_interface_INCLUDED */
