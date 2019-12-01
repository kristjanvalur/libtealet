/* The actual stack saving function, which just stores the stack,
 * this declared in an .asm file
 */
#define EXTERNAL_ASM switch_x64_msvc.asm
extern void *tealet_slp_switch(void *(*save_state)(void*, void*),
                        void *(*restore_state)(void*, void*),
                        void *extra);

