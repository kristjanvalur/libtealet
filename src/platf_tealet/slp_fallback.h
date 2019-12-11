/* slp_fallback.h
 * Implements a fallback mechanism where the stacless python switching code
 * will be used.
 * Stackless python switching code uses global variables to communicate and
 * thus is not thread safe. We do not attempt to provide a lock for those
 * global variables here, it is up to the user in each case to ensure that
 *  switching only occurs on one thread at a time.
 */

#include <stdint.h>
#include <stddef.h>

/* staic vars to pass information around.  This is what makes this
 * non-threadsafe
 */
static tealet_save_restore_t fallback_save_restore_cb = NULL;
static void *fallback_context = NULL;
static void *fallback_newstack = NULL;

/* call the provided save_state method with its arguments.
 * compute a proper stack delta and store the result for later
 */
#define SLP_SAVE_STATE(stackref, stsizediff) do {\
	intptr_t stsizeb; \
	void *newstack; \
	stackref += STACK_MAGIC; \
	newstack = fallback_save_restore_cb(fallback_context, stackref); \
	fallback_newstack = newstack; /* store this for restore_state */ \
	/* compute the delta expected by old switching code */ \
	stsizediff = ((intptr_t*) newstack - (intptr_t*)stackref) * sizeof(intptr_t); \
} while (0)

/* call the restore_state using the stored data */
#define SLP_RESTORE_STATE() do { \
	fallback_newstack = fallback_save_restore_cb(fallback_context, stackref); \
} while(0)

/* include standard stackless python switching code. */
#define SLP_EVAL
/* Rename the slp_switch function that the slp headers will define */
#include "../platf_slp/slp_platformselect.h"

#if defined TEALET_SWITCH_IMPL && ! defined __ASSEMBLER__
/* This is a wrapper that takes care of setting the appropriate globals */
void *tealet_slp_switch(tealet_save_restore_t save_restore_cb,
                        void *context)
{
	/* need to store the restore information on the stack */
	void *result;
	fallback_save_restore_cb = save_restore_cb;
	fallback_context = context;

	slp_switch();
	result = fallback_newstack;
	
	/* clearing them again is prudent */
	fallback_save_restore_cb = NULL;
	fallback_context = fallback_newstack = NULL;
	return result;
}
#endif
