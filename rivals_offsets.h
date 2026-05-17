#pragma once
#include <cstdint>

extern "C" {
    bool __stdcall ue_offsets_start();
    bool __stdcall ue_offsets_ready();

    uint64_t __stdcall ue_offsets_gworld_storage();
    uint64_t __stdcall ue_offsets_gworld();
    uint64_t __stdcall ue_offsets_gnames();
    uint64_t __stdcall ue_offsets_gobjects_storage();
    uint64_t __stdcall ue_offsets_gobjects();
    uint64_t __stdcall ue_offsets_guobjectarray();
    uint64_t __stdcall ue_offsets_fname_append();
}

