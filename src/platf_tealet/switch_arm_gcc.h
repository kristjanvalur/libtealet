/* Switch function for arm 32 as seen on RaspberryPi2
 * for ARM abi, see http://infocenter.arm.com/help/index.jsp?topic=/com.arm.doc.ihi0042f/index.html
 * and additional information for gcc inline arm assembly:
 * http://www.ethernut.de/en/documents/arm-inline-asm.html
 *
 */
#ifdef TEALET_SWITCH_IMPL
#if ! __ASSEMBLER__

#ifndef USE_ASSEMBLER
/* inline assembly does not by default produce code that is usable
 *  because depending
 * on optimization,the fp (r11) register may be used to restore the
 * stack pointer on function exit, thereby invalidating our changes
 * To arrive at reasonable assembler, follow some approach similar to:
 * cp switch_arm_gcc.h test.c
 * gcc -S -O -DTEALET_SWITCH_IMPL -DDEV test.c
 * mv test.s switch_arm_gcc.s
 * assembly code which can then be modified for actual use.  Simple optimized
 * version is better than no-optimized because the latter uses stack
 * variables for arguments.  And it turns out that this version actually
 * runs because it does not use fp for restoring the stack pointer
 * 
 * However, for now the optimizer can be instructed to omit frame
 * ponter, so we simply use that method. the -fomit-frame-pointer
 * option is applied with an __attribute__.
 * we must also enable optimization level "O" so that intermediates
 * aren't stored on the stack.
 */

#include <stdint.h>
#include <stddef.h>
#include "../switch.h"

/* these are the core registers that must be preserved. We save them because
 * we have no idea what happens after the switch, and the caller of this function
 * assumes that they are left in place when we return to him.
 */
#define NV_REGISTERS "v1", "v2","v3", "v4", "v5", "v6", "v7" /*"v8" is used as fp by gcc and cannot be used */
/* These are the floating point extension registers. Same applies, we must preserve them in
 * case the calling function was doing any floating point logic.  Note that we do not
 * preserve the FPSCR and VPR registers since they have complex rules about preservation.
 * it may be optionally disabled to store these floating point registers by not
 * defining them here.
 */
#define CP_REGISTERS "d8","d9","d10","d11","d12","d13","d14","d15"

__attribute__((optimize("O", "omit-frame-pointer")))
void *tealet_slp_switch(tealet_save_restore_t save_state,
                        tealet_save_restore_t restore_state,
                        void *extra)
{
	void *sp;
	__asm__ volatile ("" : : : NV_REGISTERS);
#ifdef CP_REGISTERS
	__asm__ volatile ("" : : : CP_REGISTERS);
#endif
	/* assembly to save non-volatile registers
         * those, according to abi, that functions must save/restore if they
         * intend to use them
	*/
	/* sp = get stack pointer from assembly code */
	__asm__ ("mov %[result], sp" : [result] "=r" (sp));
	/* store stack */
	sp = save_state(sp, extra);
	if ((intptr_t)sp & 1)
		return sp; /* error, or save only operation */
	/* set stack pointer from sp using assembly */
	__asm__ ("mov sp, %[result]" : : [result] "r" (sp));
	sp = restore_state(sp, extra);
	/* retstore register */
	return sp;
}
#endif
#else
/* assembler code here, if the above cannot be done in in-line assembly */
#endif
#endif
