#ifdef TEALET_SWITCH_IMPL
#if ! __ASSEMBLER__
#include <stdint.h>
#include "../switch.h"

/* This is the System V ABI for x86-64.
 * It is used on linux and other similar systems (other than
 * windows).
 * https://wiki.osdev.org/System_V_ABI
 * Note that we don't preserve the mmx mxcsr register or the
 * fp x87 cw (control word) as is strictly required by the ABI
 * since it requires more custom assembly.  switching between
 * floating point functions is therefore dangerous.
 */

#define PRESERVE "rbx","r12", "r13", "r14", "r15","rbp"

/* Need the optimization level to avoid storing 'stack_pointer'
 * on the stack which would cause the wrong value to be sent to 
 * the second callback call (it would be read from stack).
 * Also, avoid a frame being put in rbp, allowing us to push it.
 */
__attribute__((optimize("O", "omit-frame-pointer")))
void *tealet_slp_switch(tealet_save_restore_t save_restore_cb,
                        void *context)
{
	void *stack_pointer;
	/* assembly to save non-volatile registers */
	__asm__ volatile ("" : : : PRESERVE);
	/* sp = get stack pointer from assembly code */
	__asm__ ("movq %%rsp, %[result]" : [result] "=r" (stack_pointer));
	stack_pointer = save_restore_cb(context, stack_pointer);

	/* set stack pointer from sp using assembly */
	__asm__ ("movq %[result], %%rsp" :: [result] "r" (stack_pointer));

	/* read it back, so that the value isnt read from a stack var
	 * for the next call. For -O1, this is optimized away.
	 */
	__asm__ ("movq %%rsp, %[result]" : [result] "=r" (stack_pointer));
	stack_pointer = save_restore_cb(context, stack_pointer);
	/* restore non-volatile registers from stack */
	return stack_pointer;
}
#else
/* assembler code here, if the above cannot be done in in-line assembly */
#endif
#endif
