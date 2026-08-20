/* Project-specific runtime setup, run before main().
 *
 * ps3recomp's cellGame ships a placeholder title id ("BLES00000") and expects
 * the port to supply the real one. Taiko builds absolute content paths from it
 * -- e.g. /dev_hdd0/game/<TITLE_ID>/USRDIR/data/config/common/config.xml -- so
 * leaving the placeholder makes every one of those opens miss.
 *
 * TITLE_ID comes from the game's PARAM.SFO. Note it does NOT match the folder
 * name on disk ("SCEEX001 GREEN"), which is why the VFS root needs a tree with
 * a game/SCEEXE001 entry rather than pointing straight at the dump.
 */
#include <stdio.h>
#include <stdlib.h>

void cellGame_set_title_id(const char* title_id);
void cellGame_set_title(const char* title);

#define TAIKO_TITLE_ID "SCEEXE001"

static void taiko_set_default_environment(const char* name, const char* value)
{
    if (getenv(name)) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 0);
#endif
}

__attribute__((constructor))
static void taiko_init(void)
{
    /* Unbuffer first: the boot harness logs with printf, and a block-buffered
     * pipe eats the last 4 KB -- which is the part that says why it stopped. */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* The Wine launcher already supplies these, but the native executable is
     * commonly launched directly.  Keep explicit =0 overrides available for
     * silent/headless diagnosis. */
    taiko_set_default_environment("TAIKO_AUDIO_DECODE", "1");
    taiko_set_default_environment("TAIKO_AUDIO_SPU", "1");

    cellGame_set_title_id(TAIKO_TITLE_ID);
    cellGame_set_title("Taiko no Tatsujin(S111)");
}
