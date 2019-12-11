#pragma once

/* raw task switching function. We use a function pointer
 * for saving and restoring state (copying and restoring stack
 * context to the heap) to make assembler
 * implementations simpler. Then we don't need to have
 * a linker resolve external function calls.  An caller
 * must always provide the same value to both arguments, otherwise
 * the result is undefined (an implementation is not required
 * to save these arguments on the stack, thereby allowing different
 * ones to be used for saving stack, and then restoring for a new one)
 *
 * The implementation must simply:
 * 1) store all cpu state on the stack (callee-stored registers, etc)
 * 2) call the callback with the stack pointer (save old stack)
 * 3) replace the stack pointer with the returned value from cb.
 * 4) call the callback again with new stack pointer (restore new stack)
 * 5) Pop cpu state back from stack.
 * 6) return the result from the callback.
 *
 * The callback must itself decide if it is saving or restoring, perhaps
 * using flags in the 'context' structure.  This allows for a much simpler
 * implementation of the tealet_slp_switch() function which may
 * need to be hand coded in assembly.
 *
 * an appropriate implementation is included b
 * platf_tealet/tealet_platformselect.h
 * A template implementation is available in
 * platf_tealet/switch_template.h
 */

/* function signature to save/restore state in a stack.
 * 'context' contains information
 * for the function, 'stack_pointer' is the lower level
 * of the stack.  Returns new stack pointer.
 */
typedef void *(*tealet_save_restore_t)(void *context, void *stack_pointer);
/* The actual stack switching function.
 * It saves state, switches stack pointer, and restores state
 */
void *tealet_slp_switch(tealet_save_restore_t save_restore_cb, void *context);

