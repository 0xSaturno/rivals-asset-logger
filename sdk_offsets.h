#pragma once
#include <cstdint>

// --- Global Offsets ---
static constexpr uintptr_t OFFSET_GOBJECTS       = 0xE8A10F0;
static constexpr uintptr_t OFFSET_GNAMES         = 0xE7E92C0;
static constexpr uintptr_t OFFSET_GWORLD         = 0xEA4AB10;

// --- UObject Internals (layout never changes) ---
static constexpr uintptr_t OFF_UObject_Class     = 0x0010;
static constexpr uintptr_t OFF_UObject_FName     = 0x0018;
static constexpr uintptr_t OFF_UObject_Outer     = 0x0028;
