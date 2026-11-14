#pragma once

class QStyle;

// Windows 10's theme puts a checked menu item's tick or dot on a blue square
// (MENU_POPUPCHECKBACKGROUND), which windowsvista draws on Qt 5 and Qt 6 alike;
// Windows 11's theme leaves the square empty. The style returned draws such an
// item with the theme's tick or dot alone, everything else as base does. It
// takes base over. Elsewhere than on Windows base comes back as it is.
QStyle* menuCheckStyle(QStyle* base);
