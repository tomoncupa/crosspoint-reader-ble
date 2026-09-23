#include "BootActivity.h"

#include <GfxRenderer.h>
#include <I18n.h>

#include "fontIds.h"
#include "images/TomLogo240.h"

void BootActivity::onEnter() {
  Activity::onEnter();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(TomLogo240, (pageWidth - 240) / 2, (pageHeight - 240) / 2 - 30, 240, 240);
  renderer.drawCenteredText(UI_12_FONT_ID, pageHeight / 2 + 115, "Tom Custom Firmware .1", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 145, tr(STR_BOOTING));
  renderer.displayBuffer();
}
