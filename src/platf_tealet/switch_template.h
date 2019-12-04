
/* Template function for the switch
 * Requires in-line assembly to save/restore registers from stack
 * and to get and restore the stack pointer.
 * for example use, see switch_arm_gcc.h
 */
#ifdef TEALET_SWITCH_IMPL
#if ! __ASSEMBLER__
#include <stdint.h>
#include "../switch.h"
void *tealet_slp_switch(tealet_save_restore_t save_state,
                        tealet_save_restore_t restore_state,
                        void *extra)
{
	void *sp;
	/* assembly to save non-volatile registers
         * those, according to abi, that functions must save/restore if they
         * intend to use them
	*/
	/* sp = get stack pointer from assembly code */
	sp = save_state(sp, extra);
	if ((intptr_t)sp & 1)
		return sp; /* error, or save only operation */
	/* set stack pointer from sp using assembly */
	sp = restore_state(sp, extra);
	/* restore non-volatile registers from stack */
	return sp;
}
#else
/* assembler code here, if the above cannot be done in in-line assembly */
#endif
#endif
