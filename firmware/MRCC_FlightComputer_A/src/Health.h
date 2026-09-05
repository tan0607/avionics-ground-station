#pragma once
#include <Arduino.h>

// =====================================================
// HEALTH - reset diagnosis and automatic recovery
//
// THE RULE FOR THIS PROJECT:
// nothing ever halts. A subsystem that fails is marked
// down, skipped by everything that uses it, and retried
// quietly in the background. If it comes back, it is
// put straight back into service.
// =====================================================

void reportResetReason();
void serviceHealth();
void printStatus();
