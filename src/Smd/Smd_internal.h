#ifndef SMD_INTERNAL_H
#define SMD_INTERNAL_H

#include <ddraw.h>

void SmdLog(const char *format, ...);
void SmdLogOpen(void);
void SmdHookPrimary(IDirectDrawSurface7 *surface);
void SmdTriggerWindowFallback(const char *reason);
int SmdPatchVtblSlot(void **slot, void *hook, void **original);
void SmdBatchFlushQuads(void);

void SmdControllerInit(void);
void SmdControllerPoll(void);
int SmdControllerIsConnected(unsigned id);

extern const int g_LogAll;
extern const int g_StripAllowModeX;
extern const int g_RetryCoopLevel;
extern const int g_RetryDisplayMode;
extern const int g_LowLatency;
extern const int g_UseDwmFlush;

#endif
