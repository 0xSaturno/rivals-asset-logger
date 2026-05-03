#pragma once
#include <cstdint>

// --- Global Offsets ---
static constexpr uintptr_t OFFSET_GOBJECTS       = 0xECE19E0;
static constexpr uintptr_t OFFSET_GNAMES         = 0xEC29B80;
static constexpr uintptr_t OFFSET_GWORLD         = 0xEE8B4D0;

// --- UObject Internals ---
static constexpr uintptr_t OFF_UObject_Class     = 0x0010;
static constexpr uintptr_t OFF_UObject_FName     = 0x0018;
static constexpr uintptr_t OFF_UObject_Outer     = 0x0028;
