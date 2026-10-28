#pragma once

#include "fh2mod.h"
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>

bool InstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod);
// Call from the worker thread now and then: hooks the vtable of any new swap
// chain the Present hooks have seen (hooking from inside Present isn't safe).
void DxgiHookService(const FH2ModApi* api, FH2Mod* mod);
// Bugtrap: logs hook state while frames have stopped arriving. With retry set,
// also re-enables the hooks in case something disabled them.
void DxgiHookDiagnose(const FH2ModApi* api, FH2Mod* mod, bool retry);
void UninstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod);
