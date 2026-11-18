// Host stub for "lcd/lcd.h": UpdateLCD only needs Height().
#ifndef ENGSTUB_LCD_LCD_H
#define ENGSTUB_LCD_LCD_H
#include <circle/types.h>
class CLCD {
public:
  u8 Height() const { return 64; }
};
#endif
