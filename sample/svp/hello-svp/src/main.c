#include <genesis.h>
#include <svp/svp.h>

// protocol, kept in sync with src/hello.svp
#define SVP_CMD_TEST        0x1234
#define SVP_MAGIC           0xC0DE
#define SVP_MAGIC_OFFSET    0

// expected mailbox answer: the DSP sends back the complement of the command
#define SVP_REPLY_TEST      (SVP_CMD_TEST ^ 0xFFFF)

// generous enough to cover the DSP being scheduled only once per scanline
#define SVP_TIMEOUT         0xFFFF

#define TEXT_LEFT           2
#define VALUE_COLUMN        24


static void drawResult(u16 line, const char *label, u16 value, u16 expected)
{
    char str[8];

    VDP_drawText(label, TEXT_LEFT, line);

    intToHex(value, str, 4);
    VDP_drawText(str, VALUE_COLUMN, line);

    VDP_drawText((value == expected) ? "OK" : "KO", VALUE_COLUMN + 5, line);
}


int main(bool hardReset)
{
    u16 reply;
    u16 magic;
    bool present;
    bool answered;

    VDP_setBackgroundColor(0);
    VDP_drawText("SGDK - SVP (Sega Virtua Processor)", TEXT_LEFT, 2);

    // the SVP only runs if the ROM header carries the "SV" marker
    present = SVP_isPresent();
    VDP_drawText("SVP marker in header", TEXT_LEFT, 5);
    VDP_drawText(present ? "OK" : "MISSING", VALUE_COLUMN, 5);

    if (!present)
    {
        VDP_drawText("FAIL", TEXT_LEFT, 12);

        while(TRUE) SYS_doVBlankProcess();
    }

    // put the mailbox in a known state, then ask the DSP to do its job
    SVP_reset();

    reply = 0;
    answered = SVP_waitReply(SVP_CMD_TEST, &reply, SVP_TIMEOUT);

    // the DSP writes the magic value into DRAM before answering, so a valid
    // answer means the DRAM content is already there
    magic = SVP_readDRAMWord(SVP_MAGIC_OFFSET);

    drawResult(7, "mailbox reply", reply, SVP_REPLY_TEST);
    drawResult(8, "DRAM word at $300000", magic, SVP_MAGIC);

    if (answered && (reply == SVP_REPLY_TEST) && (magic == SVP_MAGIC))
        VDP_drawText("PASS", TEXT_LEFT, 12);
    else
        VDP_drawText("FAIL", TEXT_LEFT, 12);

    while(TRUE)
    {
        SYS_doVBlankProcess();
    }

    return 0;
}
