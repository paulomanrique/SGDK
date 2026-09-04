/**
 *  \file svp/svp.h
 *  \brief SVP (Sega Virtua Processor) control
 *  \author Paulo Manrique
 *  \date 09/2026
 *
 * This unit provides 68000 side access to the SVP cartridge coprocessor:<br>
 * - detection of the SVP marker in the ROM header<br>
 * - halt / resume of the DSP<br>
 * - XST mailbox send / receive and PM0 status polling<br>
 * - safe access to the 128 KB DRAM shared between the 68000 and the DSP<br>
 *
 * The SVP is a Samsung SSP1601 DSP running at about 23 MHz inside the
 * cartridge. It is not a C target (six level hardware call stack holding
 * return addresses only, no data stack, 512 words of internal RAM), so its
 * program is written in SSP16xx assembly in a <i>.svp</i> file, assembled by
 * the external \c ssp16asm tool and injected into the ROM image by the build.
 * This mirrors how SGDK handles the Z80 with <i>.s80</i> files.
 *
 * The DSP only starts if the Mega Drive ROM header carries the SVP marker.
 * Declare it in your own <i>src/rom_header.c</i> using #SVP_ROM_NOTES in the
 * \c notes field of the #ROMHeader structure (that field starts at ROM offset
 * 0x1C8, which is exactly where the marker belongs).
 *
 * \see SVP_ROM_NOTES
 * \see sample/svp/hello-svp
 */

#ifndef _SVP_H_
#define _SVP_H_


/**
 *  \brief
 *      SVP mailbox register (XST as seen by the DSP).
 *
 * A 68000 write sets bit 1 of the DSP visible PM0 status register, a 68000
 * read returns the last word written by the DSP.
 */
#define SVP_XST                     0xA15000
/**
 *  \brief
 *      Mirror of #SVP_XST.
 */
#define SVP_XST_MIRROR              0xA15002
/**
 *  \brief
 *      SVP status register (PM0 as seen by the 68000).
 *
 * Reading this register clears #SVP_STATE_DSP_WROTE.
 */
#define SVP_XST_STATE               0xA15004
/**
 *  \brief
 *      SVP halt register.
 *
 * Write #SVP_HALT_STOP to freeze the DSP, #SVP_HALT_RUN to let it run again.
 * Real software halts the SVP before reading #SVP_DRAM with the DMA engine.
 *
 * \note Neither Genesis Plus GX nor ares implement this register: Genesis
 * Plus GX decodes only 0xA15000, 0xA15002 and 0xA15004 and discards writes
 * anywhere else in the block. Using the guard is still correct, it is simply
 * a no-op under those emulators.
 */
#define SVP_HALT_PORT               0xA15006

/**
 *  \brief
 *      Value to write to #SVP_HALT_PORT to freeze the DSP.
 */
#define SVP_HALT_STOP               0x000A
/**
 *  \brief
 *      Value to write to #SVP_HALT_PORT to resume the DSP.
 */
#define SVP_HALT_RUN                0x0000

/**
 *  \brief
 *      #SVP_XST_STATE bit set when the DSP wrote a word to the mailbox.
 *
 * Cleared by reading #SVP_XST_STATE.
 */
#define SVP_STATE_DSP_WROTE         0x0001
/**
 *  \brief
 *      #SVP_XST_STATE bit set when the 68000 wrote a word to the mailbox.
 *
 * Cleared when the DSP reads the mailbox.
 */
#define SVP_STATE_M68K_WROTE        0x0002

/**
 *  \brief
 *      Start address of the DRAM shared with the DSP.
 */
#define SVP_DRAM                    0x300000
/**
 *  \brief
 *      Size in bytes of the DRAM shared with the DSP.
 */
#define SVP_DRAM_LEN                0x20000
/**
 *  \brief
 *      Size in word of the DRAM shared with the DSP.
 */
#define SVP_DRAM_LEN_W              (SVP_DRAM_LEN / 2)

/**
 *  \brief
 *      Cartridge byte offset of the first word of SVP program space
 *      reachable from the cartridge (DSP program word address 0x0400).
 *
 * The build injects the assembled <i>.svp</i> code here.
 */
#define SVP_PROGRAM_OFFSET          0x800
/**
 *  \brief
 *      Default DSP entry point, as a program space word address.
 */
#define SVP_ENTRY_POINT             0x0400

/**
 *  \brief
 *      Size in bytes of the cartridge area reserved for the SVP program.
 *
 * The linker keeps cartridge bytes 0x800 to 0x800 + #SVP_PROGRAM_SIZE free of
 * 68000 code (see the \c .svp_program section in <i>md.ld</i>) and the build
 * injects the assembled <i>.svp</i> file there. A project needing more room
 * rebuilds the library with <code>EXTRA_FLAGS=-DSVP_PROGRAM_SIZE=...</code>,
 * after a <code>make -f makelib.gen clean</code> (a command line variable does
 * not make the existing object out of date). The build reads the resulting
 * area back out of the linked ELF, so there is nothing to keep in sync on the
 * project side.
 *
 * \note The reservation only exists in projects that link the SVP module in,
 * so it costs nothing to a project that does not use the SVP.
 */
#ifndef SVP_PROGRAM_SIZE
#define SVP_PROGRAM_SIZE            0x800
#endif

/**
 *  \brief
 *      Build the content of the #ROMHeader \c notes field so that the SVP
 *      boots.
 *  \param flags
 *      Flags word stored at ROM offset 0x1CA. Only 0 or 1 are accepted by the
 *      SVP boot code (its low 10 bits are masked and compared), bits 11 to 13
 *      end up in the XST state register. Use 0 unless you know better.
 *  \param entry
 *      DSP entry point as a program space <b>word</b> address, normally
 *      #SVP_ENTRY_POINT. It must match the \c org directive of your
 *      <i>.svp</i> source.
 *
 * The \c notes field of #ROMHeader starts at ROM offset 0x1C8, which is where
 * the SVP boot code looks for its parameters:<br>
 * - 0x1C8: the ASCII marker "SV", without it the DSP loops forever<br>
 * - 0x1CA: the flags word<br>
 * - 0x1CC: a word read and discarded by the boot code<br>
 * - 0x1CE: the entry point<br>
 *
 * The last three are binary words and not printable text, so this macro
 * expands to a braced initializer and not to a string literal. Remaining
 * bytes of the field are zero filled by the compiler.
 *
 * Usage, in your own <i>src/rom_header.c</i>:
 * \code
 * #include <genesis.h>
 * #include <svp/svp.h>
 *
 * __attribute__((externally_visible))
 * const ROMHeader rom_header = {
 *     "SEGA MEGA DRIVE ",
 *     ...
 *     SVP_ROM_NOTES(0, SVP_ENTRY_POINT),
 *     "JUE             "
 * };
 * \endcode
 */
#define SVP_ROM_NOTES(flags, entry)             \
{                                               \
    'S', 'V',                                   \
    (char) (((flags) >> 8) & 0xFF),             \
    (char) (((flags) >> 0) & 0xFF),             \
    (char) 0x20, (char) 0x00,                   \
    (char) (((entry) >> 8) & 0xFF),             \
    (char) (((entry) >> 0) & 0xFF)              \
}

/**
 *  \brief
 *      Shorthand for <code>SVP_ROM_NOTES(0, SVP_ENTRY_POINT)</code>.
 */
#define SVP_ROM_NOTES_DEFAULT       SVP_ROM_NOTES(0, SVP_ENTRY_POINT)


/**
 *  \brief
 *      The cartridge area reserved for the SVP program.
 *
 * Pinned at cartridge byte 0x800 by <i>md.ld</i> and overwritten by the build
 * with the output of \c ssp16asm. Referencing it from C is only useful to
 * check what actually got injected, the DSP reads it on its own.
 */
extern const u8 SVP_program[SVP_PROGRAM_SIZE];

/**
 *  \brief
 *      Return TRUE if the ROM header carries the SVP marker.
 *
 * This tests the very condition the SVP boot code and Genesis Plus GX test, so
 * a FALSE result means the DSP is certainly not running. A TRUE result means
 * the ROM asks for the SVP, not that the chip is physically there: the SVP
 * registers cannot be probed safely, a cartridge without an SVP leaves the
 * 68000 hanging on the first access to 0xA150xx.
 */
bool SVP_isPresent(void);

/**
 *  \brief
 *      Freeze the DSP.
 *
 * Nested calls are counted, so a #SVP_halt() / #SVP_resume() pair can be
 * safely nested inside another one.
 *
 * \warning The counter is not protected against re-entrancy: do not call this
 * from an interrupt handler that can fire between another #SVP_halt() and its
 * matching #SVP_resume(), or the DSP can keep running inside what looks like a
 * protected section. Use #SVP_getDRAM() and your own guard if you need that.
 *
 *  \see SVP_resume()
 */
void SVP_halt(void);
/**
 *  \brief
 *      Let the DSP run again.
 *
 *  \see SVP_halt()
 */
void SVP_resume(void);

/**
 *  \brief
 *      Read and clear the SVP status register.
 *  \return
 *      The raw #SVP_XST_STATE value, a combination of #SVP_STATE_DSP_WROTE and
 *      #SVP_STATE_M68K_WROTE.
 *
 * \warning Reading the status register clears #SVP_STATE_DSP_WROTE, so the
 * result has to be kept if it is tested more than once.
 */
u16 SVP_getState(void);

/**
 *  \brief
 *      Send a command word to the DSP through the mailbox.
 *  \param cmd
 *      The word to send. It stays readable by the DSP until the DSP reads it.
 */
void SVP_sendCommand(u16 cmd);
/**
 *  \brief
 *      Read the last word written to the mailbox by the DSP.
 *
 * This does not tell whether the word is fresh, use #SVP_getState() or
 * #SVP_waitReply() for that.
 */
u16 SVP_getReply(void);

/**
 *  \brief
 *      Send a command to the DSP and wait for its reply.
 *  \param cmd
 *      The command word to send.
 *  \param reply
 *      Pointer receiving the reply word, only written on success. Can be NULL.
 *  \param timeout
 *      Maximum number of poll iterations before giving up, 0 means wait
 *      forever.
 *  \return
 *      TRUE if the DSP replied, FALSE on timeout.
 *
 * Any reply still pending from a previous exchange is dropped before the
 * command is sent, so the word returned always belongs to \p cmd.
 */
bool SVP_waitReply(u16 cmd, u16 *reply, u16 timeout);

/**
 *  \brief
 *      DRAM word the samples use as their "work finished" flag.
 *
 * Far past any data they produce, so the flag never collides with it.
 *
 *  \see SVP_waitDRAMReply()
 */
#define SVP_STATUS_WORD             0x7FF0

/**
 *  \brief
 *      Send a command to the DSP and wait for it to raise a flag in DRAM.
 *  \param cmd
 *      The command word to send through the mailbox.
 *  \param offset
 *      Word offset in DRAM the DSP writes when it is done, normally
 *      #SVP_STATUS_WORD.
 *  \param expected
 *      The value the DSP writes there.
 *  \param timeout
 *      Maximum number of poll iterations, 0 means wait forever.
 *  \return
 *      TRUE if the DSP raised the flag, FALSE on timeout.
 *
 * Prefer this over #SVP_waitReply() for anything that has to run on more than
 * one emulator. The 68000 to DSP half of the mailbox works everywhere, but the
 * DSP to 68000 half does not: Kega Fusion executes SVP code and emulates DRAM
 * correctly, yet reads of #SVP_XST always return 0xFFFF. Virtua Racing never
 * reads the status register either, it synchronises through DRAM, so that path
 * is simply untested territory in some emulators.
 *
 * The flag is cleared before the command is sent, so a value left over from a
 * previous exchange cannot be mistaken for a fresh one.
 */
bool SVP_waitDRAMReply(u16 cmd, u16 offset, u16 expected, u16 timeout);

/**
 *  \brief
 *      Reset the mailbox to a known state.
 *
 * Drains any word the DSP left in the mailbox, clears the status register and
 * makes sure the DSP is running. Call it once before starting to talk to the
 * DSP.
 */
void SVP_reset(void);

/**
 *  \brief
 *      Return a pointer to the DRAM shared with the DSP.
 *
 * The DSP is free to write DRAM at any time, so the caller is responsible for
 * bracketing its own accesses with #SVP_halt() and #SVP_resume(). The
 * SVP_readDRAM* and SVP_writeDRAM* methods do that on their own.
 */
vu16* SVP_getDRAM(void);

/**
 *  \brief
 *      Read a single word from the DRAM shared with the DSP.
 *  \param offset
 *      Word offset in DRAM, from 0 to #SVP_DRAM_LEN_W - 1.
 *
 * The DSP is halted for the duration of the access.
 */
u16 SVP_readDRAMWord(u16 offset);
/**
 *  \brief
 *      Write a single word to the DRAM shared with the DSP.
 *  \param offset
 *      Word offset in DRAM, from 0 to #SVP_DRAM_LEN_W - 1.
 *  \param value
 *      Word to write.
 *
 * The DSP is halted for the duration of the access.
 */
void SVP_writeDRAMWord(u16 offset, u16 value);

/**
 *  \brief
 *      Copy words out of the DRAM shared with the DSP.
 *  \param offset
 *      Source word offset in DRAM.
 *  \param dst
 *      Destination buffer in 68000 memory.
 *  \param count
 *      Number of words to copy.
 *
 * The DSP is halted for the duration of the transfer.
 */
void SVP_readDRAM(u16 offset, u16 *dst, u16 count);
/**
 *  \brief
 *      Copy words into the DRAM shared with the DSP.
 *  \param offset
 *      Destination word offset in DRAM.
 *  \param src
 *      Source buffer in 68000 memory.
 *  \param count
 *      Number of words to copy.
 *
 * The DSP is halted for the duration of the transfer.
 */
void SVP_writeDRAM(u16 offset, const u16 *src, u16 count);


#endif // _SVP_H_
