
/* Use the generic support for an external assembly language slp_switch function. */
#define EXTERNAL_ASM switch_arm_thumb_gas.s

extern
void *tealet_slp_switch(void *(*save_state)(void*, void*),
                        void *(*restore_state)(void*, void*),
                        void *extra);
