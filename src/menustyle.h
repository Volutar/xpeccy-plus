#pragma once

class QStyle;

// base (windowsvista, not null) without Windows 10's square behind a menu tick; takes base over
QStyle* menuCheckStyle(QStyle* base);

class QWidget;
// a menu window about to show: it and its drop shadow kept off screen until its first paint
void menuShowing(QWidget* w);
