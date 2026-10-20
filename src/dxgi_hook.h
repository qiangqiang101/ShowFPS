#pragma once

#include "fh2mod.h"
#include <windows.h>
#include <dxgi.h>
#include <dxgi1_2.h>

bool InstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod);
void UninstallDxgiHooks(const FH2ModApi* api, FH2Mod* mod);
