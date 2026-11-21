/*
 * Arm64EC support
 *
 * Copyright 2023 Jacek Caban for CodeWeavers
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#ifdef __arm64ec__

#include "windef.h"
#include "winnt.h"
#include "wine/asm.h"

__ASM_GLOBAL_FUNC( __icall_helper_arm64ec,
                   "stp fp, lr, [sp, #-0x10]!\n\t"
                   ".seh_save_fplr_x 16\n\t"
                   "mov fp, sp\n\t"
                   ".seh_set_fp\n\t"
                   ".seh_endprologue\n\t"
                   "adrp x16, __os_arm64x_dispatch_icall\n\t"
                   "ldr x16, [x16, #:lo12:__os_arm64x_dispatch_icall]\n\t"
                   "blr x16\n\t"
                   ".seh_startepilogue\n\t"
                   "ldp fp, lr, [sp], #0x10\n\t"
                   ".seh_save_fplr_x 0x10\n\t"
                   ".seh_endepilogue\n\t"
		   "br x11" )

/*
 * HACK, bug 25841: Provide an exit thunk for memcpy/memmove/memset to work around
 * https://github.com/llvm/llvm-project/issues/101355
 */
asm( "\t.def $iexit_thunk$cdecl$i8$i8i8i8; .scl 2; .type 32; .endef\n"
     "\t.section .wowthk$aa,\"xr\",discard,$iexit_thunk$cdecl$i8$i8i8i8\n"
     "\t.globl  $iexit_thunk$cdecl$i8$i8i8i8\n"
     "\t.p2align 2\n"
     "$iexit_thunk$cdecl$i8$i8i8i8:\n"
     "\t.seh_proc $iexit_thunk$cdecl$i8$i8i8i8\n"
     "\tsub     sp, sp, #48\n"
     "\t.seh_stackalloc 48\n"
     "\tstp     x29, x30, [sp, #32]\n"
     "\t.seh_save_fplr 32\n"
     "\tadd     x29, sp, #32\n"
     "\t.seh_add_fp 32\n"
     "\t.seh_endprologue\n"
     "\tadrp    x8, __os_arm64x_dispatch_call_no_redirect\n"
     "\tldr     x16, [x8, :lo12:__os_arm64x_dispatch_call_no_redirect]\n"
     "\tblr     x16\n"
     "\tmov     x0, x8\n"
     "\t.seh_startepilogue\n"
     "\tldp     x29, x30, [sp, #32]\n"
     "\t.seh_save_fplr  32\n"
     "\tadd     sp, sp, #48\n"
     "\t.seh_stackalloc 48\n"
     "\t.seh_endepilogue\n"
     "\tret\n"
     "\t.seh_endproc\n"

     "\t.section .hybmp$x,\"yi\"\n"
     "\t.symidx memcpy\n"
     "\t.symidx $iexit_thunk$cdecl$i8$i8i8i8\n"
     "\t.word 4\n"
     "\t.symidx memmove\n"
     "\t.symidx $iexit_thunk$cdecl$i8$i8i8i8\n"
     "\t.word 4\n"
     "\t.symidx memset\n"
     "\t.symidx $iexit_thunk$cdecl$i8$i8i8i8\n"
     "\t.word 4\n" );

#endif /* __arm64ec__ */
