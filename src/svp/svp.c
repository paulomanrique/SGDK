#include "config.h"
#include "types.h"

#include "svp/svp.h"

#include "sys.h"


// Cartridge area reserved for the SVP program. md.ld pins the '.svp_program'
// section at cartridge byte 0x800, which is DSP program word 0x400, and the
// build overwrites it with the output of ssp16asm. Defining it here means the
// reservation only happens for a project that actually links the SVP module.
// The default content is a 'bra always, 0x400' so that a ROM built without an
// assembled .svp file leaves the DSP spinning instead of running garbage.
__attribute__((externally_visible, used, section(".svp_program")))
const u8 SVP_program[SVP_PROGRAM_SIZE] = { 0x4C, 0x00, 0x04, 0x00 };


// SVP halt nesting level, so that a halt / resume pair can be nested
static u16 haltLevel = 0;


bool SVP_isPresent(void)
{
    // this is exactly what the SVP boot code tests before jumping into
    // cartridge code, so it tells whether the DSP is running at all
    return (rom_header.notes[0] == 'S') && (rom_header.notes[1] == 'V');
}


void SVP_halt(void)
{
    if (haltLevel++ == 0)
        *((vu16*) SVP_HALT_PORT) = SVP_HALT_STOP;
}

void SVP_resume(void)
{
    if (haltLevel && (--haltLevel == 0))
        *((vu16*) SVP_HALT_PORT) = SVP_HALT_RUN;
}


u16 SVP_getState(void)
{
    return *((vu16*) SVP_XST_STATE);
}


void SVP_sendCommand(u16 cmd)
{
    *((vu16*) SVP_XST) = cmd;
}

u16 SVP_getReply(void)
{
    return *((vu16*) SVP_XST);
}


bool SVP_waitReply(u16 cmd, u16 *reply, u16 timeout)
{
    u16 remaining = timeout;

    // drop a reply left over from a previous exchange, otherwise the very
    // first poll below would return it as if it answered this command
    SVP_getState();

    SVP_sendCommand(cmd);

    while(TRUE)
    {
        // reading the status register clears SVP_STATE_DSP_WROTE, so the reply
        // has to be picked up right away
        if (SVP_getState() & SVP_STATE_DSP_WROTE)
        {
            if (reply) *reply = SVP_getReply();
            return TRUE;
        }

        // timeout == 0 means wait forever
        if (timeout && (--remaining == 0)) return FALSE;
    }
}


void SVP_reset(void)
{
    // make sure the DSP is running whatever the previous halt state was
    haltLevel = 0;
    *((vu16*) SVP_HALT_PORT) = SVP_HALT_RUN;

    // drop any word the DSP left behind and clear SVP_STATE_DSP_WROTE
    SVP_getReply();
    SVP_getState();
}


vu16* SVP_getDRAM(void)
{
    return (vu16*) SVP_DRAM;
}


u16 SVP_readDRAMWord(u16 offset)
{
    u16 result;

    SVP_halt();
    result = ((vu16*) SVP_DRAM)[offset];
    SVP_resume();

    return result;
}

void SVP_writeDRAMWord(u16 offset, u16 value)
{
    SVP_halt();
    ((vu16*) SVP_DRAM)[offset] = value;
    SVP_resume();
}


void SVP_readDRAM(u16 offset, u16 *dst, u16 count)
{
    const vu16* src = ((vu16*) SVP_DRAM) + offset;
    u16 i = count;

    SVP_halt();
    while(i--) *dst++ = *src++;
    SVP_resume();
}

void SVP_writeDRAM(u16 offset, const u16 *src, u16 count)
{
    vu16* dst = ((vu16*) SVP_DRAM) + offset;
    u16 i = count;

    SVP_halt();
    while(i--) *dst++ = *src++;
    SVP_resume();
}
