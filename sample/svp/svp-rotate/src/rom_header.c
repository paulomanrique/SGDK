#include <genesis.h>
#include <svp/svp.h>

// The 'notes' field starts at ROM offset 0x1C8, which is exactly where the SVP
// boot code looks for its marker, flags and entry point. Without SVP_ROM_NOTES
// the DSP never leaves its internal ROM and this sample reports a failure.
__attribute__((externally_visible))
const ROMHeader rom_header = {
    "SEGA MEGA DRIVE ",
    "(C)SGDK 2026    ",
    "SVP ROTATE DEMO                                 ",
    "SVP ROTATE DEMO                                 ",
    "GM 00000000-00",
    0x000,
    "JD              ",
    0x00000000,
    0x000FFFFF,
    0xE0FF0000,
    0xE0FFFFFF,
    "RA",
    0xF820,
    0x00200000,
    0x0020FFFF,
    "            ",
    SVP_ROM_NOTES(0, SVP_ENTRY_POINT),
    "JUE             "
};
