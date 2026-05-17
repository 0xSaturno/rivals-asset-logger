#pragma once
#include <cstdint>

// // --- Global Offsets ---
// static constexpr uintptr_t OFFSET_GOBJECTS       = 0xF1284F0;
// static constexpr uintptr_t OFFSET_GNAMES         = 0xF035B88;
// static constexpr uintptr_t OFFSET_GWORLD         = 0xF2D3B68;
// static constexpr uintptr_t OFFSET_FNAME_APPEND   = 0xF3DF50;
// static constexpr uintptr_t OFFSET_FNAME_POOL     = 0xF0706C0;

// --- UObject Internals ---
static constexpr uintptr_t OFF_UObject_Class     = 0x0010;
static constexpr uintptr_t OFF_UObject_FName     = 0x0018;
static constexpr uintptr_t OFF_UObject_Outer     = 0x0028;
