// Shared sim-side declarations (implemented by the sim, used by stubs).
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

// Renamed firmware entry point (firmware main.cpp built with -Dmain=...).
int firmware_main();

// Called by the GLB_SW_System_Reset stub: the firmware wants a reboot.
void sim_request_reset();
extern std::atomic<bool> g_reset_requested;

uint64_t sim_uptime_ms();

// Watchdog feed introspection.
uint64_t sim_wdg_feeds();
uint64_t sim_wdg_last_ms();
