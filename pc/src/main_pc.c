#ifdef PC_BUILD
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <stdio.h>

#include "sgdk_pc.h"

#undef main
extern int sgdk_game_main(bool hardReset);

#ifdef _WIN32
#include <windows.h>
/* The executable is linked as a Windows (GUI) subsystem app so that
 * double-clicking it opens only the game, with no console window. That
 * would also swallow the output of the command-line modes, so when the
 * game is started from a terminal WITH arguments we re-attach to the
 * parent console and restore stdout/stderr. */
static void attach_parent_console(int argc)
{
    if (argc <= 1)
        return;                      /* double-clicked: game only, no console */
    /* If the caller redirected our output (a pipe or a file), that handle is
     * already inherited and valid -- reopening CONOUT$ would throw it away
     * and break `pitfall.exe --frames 60 | ...`. Only attach when there is
     * no stdout at all, which is the plain "run it from a terminal" case. */
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (out != NULL && out != INVALID_HANDLE_VALUE)
        return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS))
        return;
    FILE *f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
}
#else
static void attach_parent_console(int argc) { (void)argc; }
#endif

int main(int argc, char **argv)
{
    attach_parent_console(argc);
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (!SGDK_PC_init(argc, argv)) {
        SDL_Quit();
        return 1;
    }
    return sgdk_game_main(TRUE);
}
#endif
