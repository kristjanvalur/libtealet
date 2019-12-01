/* The actual stack switching function.
 * It saves state, switches stack pointer, and restores state
 */
#pragma once

/* function signature to save/restore state in a stack */
typedef void* (*tealet_save_restore_t)(void*, void*);

void *tealet_slp_switch(tealet_save_restore_t save_state,
                        tealet_save_restore_t restore_state,
                        void *extra);

