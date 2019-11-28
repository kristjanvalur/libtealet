#include <stdio.h>
#include "tealet_platformselect.h"
#define STR2(x) #x
#define STR1(x) STR2(x) 

int main(int argc, char *argv[])
{
#ifdef EXTERNAL_ASM
	char *msg = STR1(EXTERNAL_ASM);
	printf("platf_tealet/%s\n", msg);
#endif
	return 0;
}


