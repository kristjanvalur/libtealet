/* gnu C AMD64 switching code, done with in-line assembly in a .c file */
#ifdef TEALET_SWITCH_IMPL
#ifndef __ASSEMBLER__

void *tealet_slp_switch(void *(*save_restore)(void*, void*),
                        void *context)
{
  void *result;
  __asm__ volatile (
     "pushq %%rbp\n"
     "pushq %%rbx\n"       /* push the registers that may contain  */
     "pushq %%rsi\n"       /* some value that is meant to be saved */
     "pushq %%rdi\n"
     "pushq %%rcx\n"
     "pushq %%rdx\n"
     "pushq %%r8\n"
     "pushq %%r9\n"
     "pushq %%r10\n"
     "pushq %%r11\n"
     "pushq %%r12\n"
     "pushq %%r13\n"
     "pushq %%r14\n"
     "pushq %%r15\n"

     "movq %%rax, %%r12\n" /* save 'restore_state' for later */
     "movq %%rcx, %%r13\n" /* save 'extra' for later         */

     "movq %%rcx, %%rdi\n" /* arg 1: extra                       */
     "movq %%rsp, %%rsi\n" /* arg 2: current (old) stack pointer */
     "call *%%rax\n"       /* call save_state()                  */

     "movq %%rax, %%rsp\n"     /* change the stack pointer */

     /* From now on, the stack pointer is modified, but the content of the
        stack is not restored yet.  It contains only garbage here. */

     "movq %%r13, %%rdi\n" /* arg 1: extra                       */
     "movq %%rax, %%rsi\n" /* arg 2: current (new) stack pointer */
     "call *%%r12\n"       /* call restore_state()               */

     /* The stack's content is now restored. */

     "0:\n"
     "popq %%r15\n"
     "popq %%r14\n"
     "popq %%r13\n"
     "popq %%r12\n"
     "popq %%r11\n"
     "popq %%r10\n"
     "popq %%r9\n"
     "popq %%r8\n"
     "popq %%rdx\n"
     "popq %%rcx\n"
     "popq %%rdi\n"
     "popq %%rsi\n"
     "popq %%rbx\n"
     "popq %%rbp\n"

     : "=a"(result)              /* output variables */
     : "a"(save_restore),       /* input variables  */
       "c"(context)
     );
  return result;
}
#endif /* !__ASSEMBLER__*/
#endif /* TEALET_SWITCH_IMPL */
