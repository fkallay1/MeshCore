// =====================================================================
// FwId.cpp — definícia zapečeného FW identity traileru.
//
// image_size a sha256 sú placeholdery (0) — vyplní ich POST-build skript
// test_nrf-ota/gen_fw_trailer.py vo firmware.hex podľa magicu. build_number
// je compile-time z build_info.h (generuje pre:gen_build_info.py).
//
// __attribute__((used)) zabráni odstráneniu pri --gc-sections; sekcia
// .rodata.fwid spadá pod *(.rodata*) v nrf52_common.ld → flash, bez zmeny ld.
// =====================================================================
#ifdef WITH_LORA_FOTA
#include "FwId.h"
#include "build_info.h"   // FW_BUILD_NUMBER (-I test_nrf-ota)

__attribute__((used, section(".rodata.fwid")))
const FwIdTrailer fw_id_trailer = {
    { 'F','K','F','W','I','D','0','1' },  // magic
    0,                                     // image_size — post-build
    FW_BUILD_NUMBER,                       // build_number — compile-time
    { 0 },                                 // sha256 — post-build
};
#endif // WITH_LORA_FOTA
