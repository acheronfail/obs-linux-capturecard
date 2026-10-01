#pragma once
class QObject;
class QWidget;

// Returns nullptr when the OBS widget layout isn't compatible.
QObject *capture_toolbar_attach(QWidget *window);
