// Host stub for "lcd/ui.h": DrawChannelLevels + ShowSystemMessage
// are no-ops on the host (levels rendering is not under test).
#ifndef ENGSTUB_LCD_UI_H
#define ENGSTUB_LCD_UI_H
#include <circle/types.h>
class CLCD;
class CUserInterface {
public:
  static void DrawChannelLevels(CLCD &, u8, float *, float *, int, bool) {}
  void ShowSystemMessage(const char *) {}
};
#endif
