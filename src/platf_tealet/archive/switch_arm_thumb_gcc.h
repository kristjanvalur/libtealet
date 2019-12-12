
/* Use the generic support for an external assembly language slp_switch
 * function.
 */
#define EXTERNAL_ASM switch_arm_thumb_gas.s
#ifdef __ASSEMBLER__
#include "switch_arm_thumb_gas.s"
#endif

