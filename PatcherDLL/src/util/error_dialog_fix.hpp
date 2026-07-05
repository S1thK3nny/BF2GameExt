#pragma once

#include <stdint.h>

// RedWarning::DialogBoxMessage fix — port of PrismaticFlower's upstream aefa406.
// Retail builds lack the dialog template resource, so fatal-error dialogs never
// show.  Redirects the DialogBoxParamA call to a template inside BF2GameExt.dll.
// No-op on modtools (whose exe still has the resource).

extern bool g_errorDialogFixEnabled;

// Must be called inside the install window while exe sections are RW
// (writes bytes into the game's .text).
void error_dialog_fix_install(uintptr_t exe_base);
