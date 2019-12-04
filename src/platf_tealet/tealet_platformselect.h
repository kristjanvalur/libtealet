/*
 * Include the proper files to define the tealet_slp_switch() function,
 * either as C source or assembler source
 */

#pragma once
/* define USE_SLP_FALLBACK here if you want to test the SLP_FALLBACK mechanism */
#define ENABLE_SLP_FALLBACK
/* #define USE_SLP_FALLBACK */

#if defined USE_SLP_FALLBACK && !defined ENABLE_SLP_FALLBACK
#define ENABLE_SLP_FALLBACK
#endif

#ifndef USE_SLP_FALLBACK
#if   defined(_M_IX86)
#include "switch_x86_msvc.h" /* MS Visual Studio on X86 */
#define TEALET_PLATFORM x86_msvc
#elif defined(_M_X64)
#include "switch_x64_msvc.h" /* MS Visual Studio on X64 */
#define TEALET_PLATFORM x64_msvc
#elif defined(__GNUC__) && defined(__amd64__)
#include "switch_x86_64_gcc.h" /* gcc on amd64 */
#define TEALET_PLATFORM x64_gcc
#elif defined(__GNUC__) && defined(__i386__)
#include "switch_x86_gcc.h" /* gcc on X86 */
#define TEALET_PLATFORM x86_gcc
#elif defined(__GNUC__) && defined(__arm__) && defined (__thumb__)
#include "switch_arm_thumb_gcc.h" /* gcc using arm thumb */
#define TEALET_PLATFORM arm_thumb
#elif defined(__GNUC__) && defined(__arm__)
#include "switch_arm_gcc.h" /* gcc using arm */
#define TEALET_PLATFORM arm
#else
#ifdef ENABLE_SLP_FALLBACK
#define USE_SLP_FALLBACK
#else
#error "Unsupported platform!"
#endif
#endif
#endif

/* hope this is standard C */
#if defined TEALET_SWITCH_IMPL && !defined __ASSEMBLER__
#ifdef USE_SLP_FALLBACK
#warning "fallback to stackless platform support. Switching is not thread-safe"
#include "slp_fallback.h"
#endif

#define STRING2(X) #X
#define STRING(X) STRING2(X)
#if defined TEALET_PLATFORM
#pragma message ("Platform: " STRING(TEALET_PLATFORM))
#elif defined SLP_PLATFORM
#pragma message ("Platform (slp): " STRING(SLP_PLATFORM))
#endif
#endif

