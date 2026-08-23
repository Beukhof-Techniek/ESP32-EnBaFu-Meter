/**
 * @file EnBaFu_SSD1306.cpp
 * 
 * EnBaFu extension of the Adafruit SSD1306 Class
 *
 * Contains functions to draw a splash screen, draw predefined icons and position
 * text including clearing any previous text on that position.
 *
 */

#include "EnBaFu_SSD1306.h"

/**
 * Instantiates a new EnBaFu_SSD1309 class, extending the Adafruit_SSD1306 Class
 * 
 * @param theWire the TwoWire object to use
 */
EnBaFu_SSD1306::EnBaFu_SSD1306(TwoWire *theWire) : Adafruit_SSD1306(OLEDDISPLAY_WIDTH, OLEDDISPLAY_HEIGHT, theWire, OLEDDISPLAY_RESET_PIN) {}

/**
 * Clears the display and draws the splash screen in white
 */
void EnBaFu_SSD1306::drawSplashScreen() {
  clearDisplay();
  drawBitmap(0, 0, bitmap_Logo_BeukhofTechniek, 128, 64, WHITE);
  display();
}

/**
 * Clears the display and draws the icons in white at the left side of the display
 */
void EnBaFu_SSD1306::drawIcons() {
  clearDisplay();
  drawBitmap(3,  0, bitmap_alternator_12x12, 12, 12, WHITE);
  drawBitmap(0, 13, bitmap_battery_24x12_nr1, 24, 12, WHITE);
  drawBitmap(0, 26, bitmap_battery_24x12_nr2, 24, 12, WHITE);
  drawBitmap(3, 39, bitmap_gasoline_pump_12x12, 12, 12, WHITE);
  drawBitmap(0, 52, bitmap_engine_18x12, 18, 12, WHITE);
  display();
}

/**
 * Clears the contents from the given position (x,y) to the right edge of the display
 * and prints the text at the given position.
 * 
 * The clearing is done by drawing a black rectangle with a height of 7 from (x,y)
 * to x = OLEDDISPLAY_WIDTH-x
 * 
 * @param x x-coordinate starting from 0 (left)
 * @param y y-coordinate starting from 0 (top)
 * @param text Text to display, note that the text does not exceed the available space (display width - x-coordinate)
 */
void EnBaFu_SSD1306::printAt(int16_t x, int16_t y, const String &text) {
  fillRect(x, y, OLEDDISPLAY_WIDTH-x, 7, BLACK);
  setCursor(x, y);
  print(text);
  display();
}