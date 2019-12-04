/* The actual stack switching function.
 * It saves state, switches stack pointer, and restores state
 * Here, an implementation is defined which a separate assembler file
 */

/* pick an implementation, basef on platform */
#define TEALET_SWITCH_IMPL
#include "platf_tealet/tealet_platformselect.h"
