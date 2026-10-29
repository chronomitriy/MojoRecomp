#pragma once

#include <cstdint>

// Opt-in aid (MOJORECOMP_HFR_DISCOVERY=1) for locating Crash of the Titans'
// 1/60 s scheduler floor on a machine that has the game. The unlocked frame
// rates stay at 60 Hz until MojoRecompHfrSchedulerFloor is installed at that
// instruction, and its address can only be read from the original code.
//
// On the first observation the runtime scans the guest code copied into guest
// memory for loads of 1/60 and 60.0. Each observed hook site then logs its
// guest call chain and the candidate loads inside those functions, including a
// ready-to-paste [[midasm_hook]] block for every 1/60 load. Output goes to
// runtime.log with the "[hfr-discovery]" prefix. Nothing in guest state is
// changed.

bool MojoRecompHfrDiscoveryEnabled();
void MojoRecompHfrDiscoveryObserve(const char* site, uint32_t siteAddress);
