
#
#  (C) 2008-2009 Advanced Micro Devices, Inc. All Rights Reserved.
#
#  This file is part of libacml_mv.
#
#  libacml_mv is free software; you can redistribute it and/or
#  modify it under the terms of the GNU Lesser General Public
#  License as published by the Free Software Foundation; either
#  version 2.1 of the License, or (at your option) any later version.
#
#  libacml_mv is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
#  Lesser General Public License for more details.
#
#  You should have received a copy of the GNU Lesser General Public
#  License along with libacml_mv.  If not, see
#  <http://www.gnu.org/licenses/>.
#
#


# fmod.S
#
# An implementation of the fabs libm function.
#
# Prototype:
#
#     double fmod(double x,double y);
#

#
#   Algorithm:
#

#include "fn_macros.h"
#define fname FN_PROTOTYPE(fmod)
#define fname_special _fmod_special


# local variable storage offsets
.equ    temp_x, 0x0
.equ    temp_y, 0x10
.equ    stack_size,  0x28

#ifdef __ELF__
.section .note.GNU-stack,"",@progbits
#endif

.text
.align 16
.p2align 4,,15
.globl fname
.type fname,@function
fname:
    mov .L__exp_mask_64(%rip), %r10
    #move the input to GP registers
    movd %xmm0,%r8
    movd %xmm1,%r9
    movapd %xmm0,%xmm4
    movapd %xmm1,%xmm5
    movapd .L__Nan_64(%rip),%xmm6
    and %r10,%r8
    and %r10,%r9
    ror $52, %r8
    ror $52, %r9
    #ifeither of the exponents is zero we do the fmod calculation in x87 mode
    test %r8, %r8
    jz  .L__LargeExpDiffComputation
    mov %r9,%r10
    test %r9, %r9
    jz  .L__LargeExpDiffComputation
    sub %r9,%r8 
    cmp $52,%r8
    jge .L__LargeExpDiffComputation
    pand %xmm6,%xmm4
    pand %xmm6,%xmm5
    comisd %xmm5,%xmm4
    jp  .L__InputIsNaN # if either of xmm1 or xmm0 is a NaN then 
                       # parity flag is set
    jz  .L__Input_Is_Equal
    jbe .L__ReturnImmediate
    cmp $0x7FF,%r8
    jz  .L__Dividend_Is_Infinity

    #calculation without using the x87 FPU
.L__DirectComputation:
    movapd %xmm4,%xmm2
    movapd %xmm5,%xmm3
    divsd %xmm3,%xmm2
    cvttsd2siq %xmm2,%r8
    cvtsi2sdq %r8,%xmm2

    #multiplication in QUAD Precision
    #Since the below commented multiplication resulted in an error
    #we had to implement a quad precision multiplication.
    #LOGIC behind Quad Precision Multiplication
    #x = hx + tx   by setting x's last 27 bits to null
    #y = hy + ty   similar to x
    movapd .L__27bit_andingmask_64(%rip),%xmm4 
    #movddup %xmm5,%xmm5 #[x,x]
    #movddup %xmm2,%xmm2 #[y,y]

    movapd %xmm5,%xmm1 # x
    movapd %xmm2,%xmm6 # y
    movapd %xmm2,%xmm7 # 
    mulsd  %xmm5,%xmm7 # xmm7 = z = x*y
    andpd  %xmm4,%xmm1
    andpd  %xmm4,%xmm2
    subsd  %xmm1,%xmm5 # xmm1 = hx   xmm5 = tx
    subsd  %xmm2,%xmm6 # xmm2 = hy   xmm6 = ty

    movapd %xmm1,%xmm4 # copy hx
    mulsd  %xmm2,%xmm4 # xmm4 = hx*hy
    subsd  %xmm7,%xmm4 # xmm4 = (hx*hy - z)
    mulsd  %xmm6,%xmm1 # xmm1 = hx * ty
    addsd  %xmm1,%xmm4 # xmm4 = ((hx * hy - *z) + hx * ty)
    mulsd  %xmm5,%xmm2 # xmm2 = tx * hy
    addsd  %xmm2,%xmm4 # xmm4 = (((hx * hy - *z) + hx * ty) + tx * hy)
    mulsd  %xmm5,%xmm6 # xmm6 = tx * ty
    addsd  %xmm4,%xmm6 # xmm6 = (((hx * hy - *z) + hx * ty) + tx * hy) + tx * ty;
    #xmm6 and xmm7 contain the quad precision result
    #v = dx - c;
    #dx = v + (((dx - v) - c) - cc);
    movapd %xmm0,%xmm1 # copy the input number
    pand   .L__Nan_64(%rip),%xmm1
    movapd %xmm1,%xmm2 # xmm2 = dx = xmm1
    subsd  %xmm7,%xmm1 # v = dx - c
    subsd  %xmm1,%xmm2 # (dx - v)
    subsd  %xmm7,%xmm2 # ((dx - v) - c)
    subsd  %xmm6,%xmm2 # (((dx - v) - c) - cc)
    addsd  %xmm1,%xmm2 # xmm2 = dx = v + (((dx - v) - c) - cc) 
                       # xmm3 = w
    comisd .L__Zero_64(%rip),%xmm2
    jae .L__positive
    addsd  %xmm3,%xmm2
.L__positive:    
#  return x < 0.0? -dx : dx;
.L__Finish:
    comisd .L__Zero_64(%rip), %xmm0 
    ja  .L__Not_Negative_Number1

.L__Negative_Number1:
    movapd .L__Zero_64(%rip),%xmm0
    subsd  %xmm2,%xmm0
    ret 
.L__Not_Negative_Number1:
    movapd %xmm2,%xmm0 
    ret 

    #calculation using the x87 FPU
    #For numbers whose exponent of either of the divisor,
    #or dividends are 0. Or for numbers whose exponential 
    #diff is grater than 52
.align 16
.L__LargeExpDiffComputation:
    sub $stack_size, %rsp
    movsd %xmm0, temp_x(%rsp)
    movsd %xmm1, temp_y(%rsp)
    ffree %st(0)
    ffree %st(1)
    fldl  temp_y(%rsp)
    fldl  temp_x(%rsp)
    fnclex
.align 32
.L__repeat:    
    fprem #Calculate remainder by dividing st(0) with st(1)
          #fprem operation sets x87 condition codes, 
          #it will set the C2 code to 1 if a partial remainder is calculated
    fnstsw %ax 
    and $0x0400,%ax # Stores Floating-Point Store Status Word into the accumulator
                    # we need to check only the C2 bit of the Condition codes
    cmp $0x0400,%ax # Checks whether the bit 10(C2) is set or not 
                    # IF its set then a partial remainder was calculated
    jz .L__repeat   
    #store the result from the FPU stack to memory
    fstpl   temp_x(%rsp) 
    fstpl   temp_y(%rsp) 
    movsd   temp_x(%rsp), %xmm0 
    add $stack_size, %rsp
    ret 

    #IF both the inputs are equal
.L__Input_Is_Equal:
    cmp $0x7FF,%r8
    jz .L__Dividend_Is_Infinity
    cmp $0x7FF,%r9
    jz .L__InputIsNaN
    movsd %xmm0,%xmm1
    pand .L__sign_mask_64(%rip),%xmm1
    movsd .L__Zero_64(%rip),%xmm0
    por  %xmm1,%xmm0
    ret

.L__InputIsNaN:
    por .L__QNaN_mask_64(%rip),%xmm0
    por .L__exp_mask_64(%rip),%xmm0
.L__Dividend_Is_Infinity:
    ret

#Case when x < y
.L__ReturnImmediate:
    ret
    


.align 32    
.L__sign_mask_64:          .quad 0x8000000000000000
                           .quad 0x0
.L__exp_mask_64:           .quad 0x7FF0000000000000
                           .quad 0x0
.L__27bit_andingmask_64:   .quad 0xfffffffff8000000
                           .quad 0
.L__2p52_mask_64:          .quad 0x4330000000000000 
                           .quad 0
.L__Zero_64:               .quad 0x0 
                           .quad 0
.L__QNaN_mask_64:          .quad 0x0008000000000000 
                           .quad 0
.L__Nan_64:                .quad 0x7FFFFFFFFFFFFFFF
                           .quad 0

