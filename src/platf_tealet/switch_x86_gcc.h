/* gcc implementationfor X86 (32 bit), inline assembly */

/* Follow System V i386 abi, including 16 byte stack alignment 
 * https://wiki.osdev.org/System_V_ABI#i386
 * eax, ecx, edx are scratch regs, 
 * ebp, ebx, esi, edi are callee-preserved
 * We have compiler construct a frame and push ebp
 * an then we fix an aligned stack pointer and just store
 * the arguments in proper places.
 * The function calls need to be assembler coded because a compiler
 * generated call will adjust stack pointer, but the restore
 * opcode will be placed _after_ we ourselves then modify esp,
 * ruining everything.
 * So, we use C to set up frame, pass arguments in and out,
 * and preserve registers. But we ourselves assemble the 
 * calls and stack pointer changes 
 */

# define PRESERVED "ebx", "esi", "edi"

#ifdef TEALET_SWITCH_IMPL
#if !__ASSEMBLER__
#include "../switch.h"
__attribute__((optimize("O1", "no-omit-frame-pointer")))
void *tealet_slp_switch(tealet_save_restore_t save_restore_cb,
                        void *context)
{
void *result;
  /* push registers, set up stack pointer on boundary */
    __asm__("" ::: PRESERVED);
    __asm__(

      /* adjust stack pointer to be 16 bit aligned with 
       * room for call args
       * since the call instruction, 5 four bytes have
       * been pushed (ip, bp, bx, si, di), need extra 12 bytes
       */
    "subl $12, %%esp\n"
    "movl %[cb], %%esi\n"  /* save 'save_restore_cb' for later */
    "movl %[ctx], %%edi\n" /* save 'context' for later         */

    /* first call */
    "movl %%esp, 4(%%esp)\n"  /* arg 1 */
    "movl %%edi, 0(%%esp)\n"  /* arg 0 */
    "call *%%esi\n"

    /* restore esp */
    "movl %%eax, %%esp\n"
     
    /* second call */
    "movl %%eax, 4(%%esp)\n"
    "movl %%edi, 0(%%esp)\n"
    "call *%%esi\n"

    "movl %%eax, %[result]\n"

    "addl $12, %%esp\n"
    : [result] "=r" (result)              /* output variables */
    : [cb] "r" (save_restore_cb),       /* input variables  */
      [ctx] "r" (context)
    );
    return result;
}
#endif
#endif /* TEALET_SWITCH_IMPL */
