#include <stdio.h>
#include "tealet_platformselect.h"
#define STR2(x) #x
#define STR1(x) STR2(x) 

int main(int argc, char *argv[])
{
	if (argc > 1) {
#if defined EXTERNAL_ASM
		char *masm = " (asm " STR1(EXTERNAL_ASM) ")";
#else
		char *masm = "";
#endif
#if defined TEALET_PLATFORM
		char *msg = STR1(TEALET_PLATFORM);
		printf("Platform: %s%s\n", msg, masm);
#elif defined SLP_PLATFORM
		char *msg = STR1(TEALET_PLATFORM);
		printf("Platform: %s (non-threadsafe)%s\n", msg, masm);
#else
		printf("Platform: Unknown\n");
#endif
	} else {
#ifdef EXTERNAL_ASM
		char *msg = STR1(EXTERNAL_ASM);
		printf("src/platf_tealet/%s\n", msg);
#endif
	}
	return 0;
}


