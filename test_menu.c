#include <windows.h>
#include <stdio.h>

typedef struct {
    int stage;
    int isBoss;
    const char *label;
} MenuItem;

static const MenuItem g_MenuItems[12] = {
    { 1, 0, "STAGE 1 : PRISM SYSTEM" },
    { 1, 1, "STAGE 1 [BOSS] : R-01 CIRRUS" },
    { 2, 0, "STAGE 2 : MEMORIES" },
    { 2, 1, "STAGE 2 [BOSS] : R-02 STRATUS" },
    { 3, 0, "STAGE 3 : DEEP ZONE" },
    { 3, 1, "STAGE 3 [BOSS] : R-03 CUMULUS" },
    { 4, 0, "STAGE 4 : COUNTERMEASURE" },
    { 4, 1, "STAGE 4 [BOSS] : R-04 ALTOSTRATUS" },
    { 5, 0, "STAGE 5 : CORE PRISM" },
    { 5, 1, "STAGE 5 [BOSS] : R-05 NIMBOSTRATUS" },
    { 6, 0, "STAGE 6 : EXTRA STAGE" },
    { 6, 1, "STAGE 6 [BOSS] : R-06 CUMULONIMBUS" },
};

int main() {
    printf("Total menu items: %d\n", (int)(sizeof(g_MenuItems)/sizeof(g_MenuItems[0])));
    for (int i = 0; i < 12; i++) {
        printf("%02d: Stage %d (Boss=%d): %s\n", i, g_MenuItems[i].stage, g_MenuItems[i].isBoss, g_MenuItems[i].label);
    }
    return 0;
}
