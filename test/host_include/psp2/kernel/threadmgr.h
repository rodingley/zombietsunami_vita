/* stand-in for the Vita SDK header, for the host tests in test/ */
#pragma once
#include <stdint.h>
typedef int SceUID;
typedef unsigned int SceSize;
typedef unsigned int SceUInt;
typedef struct { char d[32]; } SceKernelLwMutexWork;
int sceKernelCreateLwMutex(void *w, const char *n, unsigned a, int c, const void *o);
int sceKernelLockLwMutex(void *w, int c, unsigned *t);
int sceKernelUnlockLwMutex(void *w, int c);
int sceKernelCreateThread(const char *n, int (*e)(SceSize, void *), int p, unsigned s, unsigned a, int m, const void *o);
int sceKernelStartThread(int t, unsigned l, void *a);
int sceKernelExitDeleteThread(int s);
int sceKernelDelayThread(unsigned us);
SceUID sceKernelGetThreadId(void);
