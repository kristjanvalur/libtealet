/* The actual stack switching function.
 * It saves state, switches stack pointer, and restores state
 * Here an implementation is defined which uses inline assembly.
 */

#include "switch.h"

/* pick an implementation, basef on platform */
#define TEALET_SWITCH_IMPL
#include "platf_tealet/tealet_platformselect.h"
