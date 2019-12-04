/* The actual stack switching function.
 * It saves state, switches stack pointer, and restores state
 */
#pragma once

/* function signature to save/restore state in a stack
 * sp is the stack pointer, extra contains information
 * for the function
*/
typedef void* (*tealet_save_restore_t)(void *sp, void *extra);

/* raw task switching function. We use function pointers
 * for saving and restoring state (copying and restoring stack
 * context to the heap) to make assembler
 * implementations simpler. Then we don't need to have
 * a linker resolve external function calls.
 * These are always the same callbacks.
 *
 * an appropriate implementation is included b
 * platf_tealet/tealet_platformselect.h
 * A template implementation is available in
 * platf_tealet/switch_template.h
 */

void *tealet_slp_switch(tealet_save_restore_t save_state,
                        tealet_save_restore_t restore_state,
                        void *extra);

