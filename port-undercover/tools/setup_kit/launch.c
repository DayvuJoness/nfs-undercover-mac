// launch.c - the packaged app's executable (Contents/MacOS/Launch).
//
// Finder starts this; it sets up the places a Mac game keeps its things and
// then runs the real game binary beside it (UndercoverRecomp):
//
//   ~/Library/Application Support/NFS Undercover/        profile (saves, settings)
//       <the game's own save folder>                    the game's own save files
//       registry.json                                    the game's options
//       mod-settings.json                                the port's options
//       Logs/<date>-<time>.log, Logs/latest.log          one log per launch (10 kept)
//       ShaderWarm/                                      Metal shaders to precompile
//
// Defaults come from Contents/Resources/Defaults the first time only, so a
// player's settings are never overwritten by an update. The game itself is
// Contents/Resources/Game/nfs.exe, built from the player's own copy.
//
// Any RECOMP_* variable already set in the environment wins, so Terminal
// runs can still override the profile, the log level and so on.
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <spawn.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define PROFILE_NAME "NFS Undercover"
#define GAME_BINARY "UndercoverRecomp"
#define LOGS_KEPT 10

static void mkdirs(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *p = buf + 1; *p; ++p)
        if (*p == '/') {
            *p = 0;
            mkdir(buf, 0755);
            *p = '/';
        }
    mkdir(buf, 0755);
}

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void copy_if_missing(const char *src, const char *dst) {
    if (exists(dst) || !exists(src))
        return;
    FILE *in = fopen(src, "rb"), *out = in ? fopen(dst, "wb") : NULL;
    if (in && out) {
        char buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, in)) > 0)
            fwrite(buf, 1, n, out);
    } else {
        fprintf(stderr, "launch: could not copy %s to %s\n", src, dst);
    }
    if (out)
        fclose(out);
    if (in)
        fclose(in);
}

static void set_default(const char *name, const char *value) {
    if (!getenv(name))
        setenv(name, value, 1);
}

// The Windows GEOID (the "Table of Geographical Locations") of the Mac's region, as text for RECOMP_GEOID.
static const char *mac_region_geoid(void) {
    static const struct {
        const char *iso, *geoid;
    } table[] = {{"RU", "203"}, {"US", "244"}, {"GB", "242"}, {"DE", "94"},  {"FR", "84"},  {"IT", "118"},
                 {"ES", "217"}, {"SE", "221"}, {"DK", "61"},  {"NO", "177"}, {"FI", "77"},  {"NL", "176"},
                 {"PL", "191"}, {"CZ", "75"},  {"HU", "109"}, {"UA", "241"}, {"BY", "29"},  {"KZ", "137"},
                 {"PT", "193"}, {"BR", "32"},  {"MX", "166"}, {"CA", "39"},  {"AU", "12"},  {"JP", "122"},
                 {"AT", "14"},  {"CH", "223"}, {"BE", "21"},  {"IE", "68"},  {"TR", "235"}};
    char cc[8] = "";
    CFLocaleRef locale = CFLocaleCopyCurrent();
    if (locale) {
        CFStringRef code = (CFStringRef)CFLocaleGetValue(locale, kCFLocaleCountryCode);
        if (code)
            CFStringGetCString(code, cc, sizeof cc, kCFStringEncodingASCII);
        CFRelease(locale);
    }
    for (size_t i = 0; i < sizeof table / sizeof *table; ++i)
        if (strcmp(cc, table[i].iso) == 0)
            return table[i].geoid;
    return "244";
}

static int by_name(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

// Keep the newest `keep` files whose names start with `prefix` ("" = a digit, the dated logs) and end with `suffix`
// (their names sort by time).
static void prune_files(const char *dir, const char *prefix, const char *suffix, int keep) {
    DIR *d = opendir(dir);
    if (!d)
        return;
    char *names[1024];
    int n = 0;
    struct dirent *e;
    const size_t sl = strlen(suffix), pl = strlen(prefix);
    while ((e = readdir(d)) && n < 1024) {
        size_t len = strlen(e->d_name);
        if (len > sl + pl && !strcmp(e->d_name + len - sl, suffix) &&
            (pl ? !strncmp(e->d_name, prefix, pl) : (e->d_name[0] >= '0' && e->d_name[0] <= '9')))
            names[n++] = strdup(e->d_name);
    }
    closedir(d);
    qsort(names, n, sizeof *names, by_name);
    for (int i = 0; i < n; ++i) {
        if (i < n - keep) {
            char p[PATH_MAX];
            snprintf(p, sizeof p, "%s/%s", dir, names[i]);
            unlink(p);
        }
        free(names[i]);
    }
}

static void prune_logs(const char *dir) {
    prune_files(dir, "", ".log", LOGS_KEPT);
    prune_files(dir, "", ".journal.txt", LOGS_KEPT);
    prune_files(dir, "hang-", ".txt", 10);
    prune_files(dir, "dip-", ".txt", 30);
}

// A whole-number value from mod-settings.json ("key": n), or `fallback`.
static long mod_setting(const char *profile, const char *key, long fallback) {
    char path[PATH_MAX], want[128], buf[65536];
    snprintf(path, sizeof path, "%s/mod-settings.json", profile);
    FILE *f = fopen(path, "rb");
    if (!f)
        return fallback;
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    snprintf(want, sizeof want, "\"%s\":", key);
    const char *at = strstr(buf, want);
    return at ? strtol(at + strlen(want), NULL, 10) : fallback;
}

int main(int argc, char **argv) {
    (void)argc;
    char self[PATH_MAX], real[PATH_MAX];
    uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size) != 0 || !realpath(self, real)) {
        fprintf(stderr, "launch: cannot find my own path\n");
        return 1;
    }
    char macos[PATH_MAX], resources[PATH_MAX], game[PATH_MAX], binary[PATH_MAX];
    snprintf(macos, sizeof macos, "%s", dirname(real));
    snprintf(resources, sizeof resources, "%s/../Resources", macos);
    if (realpath(resources, real))
        snprintf(resources, sizeof resources, "%s", real);
    snprintf(game, sizeof game, "%s/Game", resources);
    snprintf(binary, sizeof binary, "%s/" GAME_BINARY, macos);

    const char *home = getenv("HOME");
    if (!home || !*home) {
        fprintf(stderr, "launch: HOME is not set\n");
        return 1;
    }
    char profile[PATH_MAX], logs[PATH_MAX], path[PATH_MAX], def[PATH_MAX];
    const char *env_profile = getenv("RECOMP_PROFILE_DIR");
    if (env_profile && *env_profile)
        snprintf(profile, sizeof profile, "%s", env_profile);
    else
        snprintf(profile, sizeof profile, "%s/Library/Application Support/" PROFILE_NAME, home);
    snprintf(logs, sizeof logs, "%s/Logs", profile);
    mkdirs(logs);

    static const char *const defaults[] = {"registry.json", "mod-settings.json"};
    for (size_t i = 0; i < sizeof defaults / sizeof *defaults; ++i) {
        snprintf(def, sizeof def, "%s/Defaults/%s", resources, defaults[i]);
        snprintf(path, sizeof path, "%s/%s", profile, defaults[i]);
        copy_if_missing(def, path);
    }

    set_default("RECOMP_PROFILE_DIR", profile);
    // The F10 menu pauses the game at its main-loop step (FUN_006ae4c0) on the main thread, which services the
    // window, so menu keys keep arriving while it waits (kit hooks.cpp; parking the render thread at Present froze
    // the menu: no keys reached it).
    set_default("RECOMP_MENU_PAUSE_AT", "6ae4c0");
    // An Xbox / PlayStation pad: the game finds pads through DirectInput and drives one whose device path has "IG_"
    // through XInput (kit dinput.cpp DIPROP_GUIDANDPATH; 2026-11-22, the person's Xbox pad did nothing in game).
    set_default("RECOMP_DINPUT_XINPUT_PATH", "1");
    // The F10 menu's Saves page (kit settings_menu.cpp): the game keeps each save in a folder here.
    snprintf(path, sizeof path, "%s/Documents/NFS Undercover", profile);
    set_default("RECOMP_SAVES_DIR", path);
    // Each save has its own F10 settings (kit settings.cpp, 2026-11-24): a save without them starts from these.
    snprintf(path, sizeof path, "%s/Defaults/mod-settings.json", resources);
    set_default("RECOMP_SETTINGS_DEFAULTS", path);
    snprintf(path, sizeof path, "%s/registry.json", profile);
    set_default("RECOMP_REGISTRY", path);
    snprintf(path, sizeof path, "%s/nfs.exe", game);
    set_default("RECOMP_EXE", path);
    set_default("RECOMP_LOG", "1");
    // The player's country for KERNEL32!GetUserGeoID (kit/runtime/kernel32.cpp): the game picks miles or kilometres
    // by it for a new profile. From the Mac's region; the United States (244) when it is not one listed here.
    set_default("RECOMP_GEOID", mac_region_geoid());
    // Cop speech: the game asks for the English set (copspeechdat_en.big) whatever its Language; an install that
    // carries another language's set only (a Russian one: copspeechdat_ru.big) gets that set instead
    // (kit/runtime/kernel32.cpp file_fallback; 2026-11-14).
    set_default("RECOMP_FILE_FALLBACK", "SOUND/SPEECH|copspeech|copspeechdat_%s.big|en|ru,fr,de,it,es,sv,da,cs,hu,nl,pl,fi");
    // Render resolution (core.nfsu/render_scale): 0 Screen follows the screen (the
    // renderer's default: 3D drawn at the Retina size whatever the game resolution),
    // 1 draws at 1.5x the game resolution, 2 (the default) at the game resolution.
    // The renderer reads the scale once, at start-up.
    switch (mod_setting(profile, "core.nfsu/render_scale", 2)) {
    case 1:
        set_default("RECOMP_D3D9_SCALE", "1.5");
        break;
    case 2:
        set_default("RECOMP_D3D9_SCALE", "1");
        break;
    case 3: // half the screen, cooler (2026-11-18): screen-sized targets at 0.5, enlarged by MetalFX
        set_default("RECOMP_D3D9_SCALE", "0.5");
        break;
    default:
        break;
    }
    // The game's sound mixer on its own core (core.nfsu/mixer_free, default on since 2026-11-15): the runtime lets the
    // guest thread that names itself "RWAudioCore Dac" run beside the main thread instead of taking turns with it. Taking
    // turns, it held the baton 11-18 s of every 30 s in play; on its own core gameplay ran about 1.4x faster, no faults.
    // Renamed from mixer_own_core when the default turned on, so a profile that saved the old default (0) gets the new one.
    // The render thread too (core.nfsu/render_free, default on since 2026-11-15): the main thread and "RenderThread"
    // took turns on one core (60% + 40% of it at 120 fps); its Direct3D calls run under the runtime's Direct3D lock
    // and the hand-over points carry memory barriers (game.toml [translate] memory_fences).
    {
        // Always on since 2026-11-17 (the rows left the F10 menu: nobody turns them off to lose frames); a value an
        // older profile saved is ignored.
        const int mixer = 1, render = 1;
        if (mixer && render)
            set_default("RECOMP_FREE_THREADS", "RWAudioCore Dac,RenderThread");
        else if (mixer)
            set_default("RECOMP_FREE_THREADS", "RWAudioCore Dac");
        else if (render)
            set_default("RECOMP_FREE_THREADS", "RenderThread");
    }
    // Soft shadows (core.nfsu/soft_shadows, default on; 2026-11-17): the game's shadow comparisons become soft
    // ones over a disc of shadow-map reads, wider the farther the surface is behind what shades it (kit
    // RECOMP_D3D9_SOFT_SHADOW_SAMPLER / _SOFT_SHADOW), with the 3072x1024 shadow cascades at 1.5x; the 256x512
    // reflection maps at 2x. Smoke and dust (core.nfsu/smoke_resolution: 0 the game's quarter of the screen, 1 half
    // - the default, 2 full): the quarter-screen smoke/dust (and blur) buffers 2x or 4x (RECOMP_D3D9_TARGET_SCALE).
    {
        // 2026-11-17 (heat): the shadow map back at the game's own size - the soft edges hide its steps - and smoke
        // at the game's quarter by default; core.nfsu/shadow_softness: 0 the game's hard shadows, 1 light, 2 medium
        // (the default), 3 strong.
        // 2026-11-22 (core.nfsu/shadow_reflection_detail: 0 quality - the default, as before -, 1 balanced, 2
        // smoothness): the reflection maps at 2x / 1x / 1x and the enlarged shadow map at 2x / 1.5x / 1x.
        const int detail = mod_setting(profile, "core.nfsu/shadow_reflection_detail", 0);
        char rules[96] = "";
        if (detail <= 0)
            strcat(rules, "256x512:2");
        static const char *const softness[4] = {NULL, "300,3", "600,5", "1200,8"};
        const int soft = mod_setting(profile, "core.nfsu/shadow_softness", 2);
        if (soft >= 1 && soft <= 3) {
            set_default("RECOMP_D3D9_SOFT_SHADOW_SAMPLER", "SHADOWMAP_SAMPLER");
            set_default("RECOMP_D3D9_SOFT_SHADOW", softness[soft]);
        }
        // 2026-11-19: with the shadows drawn farther (core.nfsu/draw_distance) the cascades cover more ground, so the
        // 3072x1024 shadow map is drawn at twice the size.
        if (mod_setting(profile, "core.nfsu/draw_distance", 2) > 0 && detail < 2)
            strcat(rules, detail == 1 ? ",3072x1024:1.5" : ",3072x1024:2");
        const int smoke = mod_setting(profile, "core.nfsu/smoke_resolution", 0);
        if (smoke == 1 || smoke == 2)
            strcat(rules, smoke == 2 ? ",1/4:4" : ",1/4:2");
        if (rules[0])
            set_default("RECOMP_D3D9_TARGET_SCALE", rules[0] == ',' ? rules + 1 : rules);
    }
    // Gloss of roads and buildings (core.nfsu/gloss: 0 as in the game, 1 softer - the default, 2 matte; live): the
    // world shaders' specular colour, by name from their constant tables (kit RECOMP_D3D9_PS_CONST_SCALE at the matte
    // level; the kit reads the setting while running). 2026-11-18: car paint is no longer touched - its shaders (the
    // ones with cvVinylScales) carry cvSpecularMin/Range and cvEnvmapRange, and scaling those made the traffic cars
    // look like plastic; no world shader has them.
    // Glow round bright things (core.nfsu/bloom: 0 as in the game, 1 weaker - the default, 2 off; live, 2026-11-18):
    // the final composite's cvVisualTreatmentParams.x scales its bloom image (the lights' halos).
    // 2026-11-19 (pics/19, a mirror-like wet strip along the road's edge): the road shader's own environment
    // reflection, which grows toward grazing angles - cvReflectionParams.y, its added reflection, follows gloss too.
    set_default("RECOMP_D3D9_PS_CONST_SCALE",
                "cvWorldSpecularColour:0.35,cvReflectionParams.y:0.3,cvVisualTreatmentParams.x:0@bloom");
    // Headlights and tunnel lamps as lights, the tunnel darkening and the deeper baked shade (2026-11-18/19) were
    // taken out on the person's word (2026-11-19): only the game's sun, as before. The kit features stay, unset here
    // (RECOMP_D3D9_CAR_LIGHTS, RECOMP_D3D9_SKY_PROBE, "@roof" rules, all-vertex.patch).
    set_default("RECOMP_D3D9_PS_CONST_SETTING", "gloss");
    // The start-up logo movie (core.nfsu/startup_logos, default ON - the person wants the logos; renamed from startup_movie, whose saved default "off" is ignored; only when turned
    // off): MOVIES\EA_Bumper_*.vp6 is not there for the game (kit RECOMP_FILE_HIDE), which then goes straight on
    // to its warning and title screens, ~9 s sooner. Checked: no other file has that name; story videos are storey_*.
    if (!mod_setting(profile, "core.nfsu/startup_logos", 1))
        set_default("RECOMP_FILE_HIDE", "EA_Bumper_");
    // HDR (core.nfsu/hdr_output, default on, 2026-11-18): the frame's final composite marks the 3D scene, so the
    // presenter lifts its highlights above white and leaves the HUD, menus and text as they are (kit d3d9_metal.mm).
    if (mod_setting(profile, "core.nfsu/hdr_output", 0))
        set_default("RECOMP_D3D9_HDR_SCENE", "1");
    // The picture beside the notch (core.nfsu/notch_area, default on): full screen covers the whole display instead of
    // macOS's full screen below the notch (kit platform_ui_desktop.cpp).
    if (mod_setting(profile, "core.nfsu/notch_area", 1))
        set_default("RECOMP_MAC_FULLSCREEN_NOTCH", "1");
    // The speed blur (core.nfsu/motion_blur): unless it is "as in the game" (1), the game's blur composite is weighted
    // by distance from the centre (mods/core/nfsu/shaders/*.patch, kit RECOMP_D3D9_SHADER_PATCH_DIR, 2026-11-17), so
    // the centre keeps the full-resolution picture instead of a half-size copy at speed.
    {   // always (2026-11-17): the setting's levels apply while the game runs (core.nfsu)
        snprintf(path, sizeof path, "%s/mods/core/nfsu/shaders", resources);
        set_default("RECOMP_D3D9_SHADER_PATCH_DIR", path);
    }
    // The game mods in F10's Mods tab (2026-11-19; files in Resources/ModData, put there by package.py from the
    // person's mods folder). Each is switched live by its core.nfsu setting:
    // - TexMod packs (kit host/texmod.cpp): Redux's textures, the ProStreet sky;
    // - files the game reads instead of its own (kit RECOMP_FILE_OVERLAY, next time the game opens them): Redux's
    //   GLOBAL\InGameA.bun, the high-contrast HUD's GLOBAL\HUDTEXRACE.BIN;
    // - the Photorealistic grade in the scene composites' patches (c30.x, kit RECOMP_D3D9_PS_SETTING_REG).
    {
        char spec[8192];
        snprintf(spec, sizeof spec,
                 "mod_redux=%s/ModData/redux/NFSUCRedux.tpf|mod_prostreet_sky=%s/ModData/sky/japan3.tpf|"
                 "mod_prostreet_sky=%s/ModData/sky/skyfix.tpf!F3640B31",
                 resources, resources, resources);
        set_default("RECOMP_D3D9_TEXMOD", spec);
        snprintf(spec, sizeof spec,
                 "mod_redux=GLOBAL\\InGameA.bun=%s/ModData/redux/InGameA.bun|"
                 "mod_hc_hud=GLOBAL\\HUDTEXRACE.BIN=%s/ModData/hchud/HUDTEXRACE.BIN|"
                 // the add-on cars and the Porsche Pack (2026-11-24): their files are read always - every car is in
                 // the game from the start, and each save's switches (core.nfsu addoncars.inc) only show or hide
                 // them, so turning them on or off needs no restart (an empty setting = always)
                 "=LANGUAGES\\=%s/ModData/addoncars/languages|"
                 "=GLOBAL\\GLOBALB.LZC=%s/ModData/addoncars/GLOBALB_ALL.BUN|"
                 "=FRONTEND\\LOGOTEXTURES.BUN=%s/ModData/addoncars/replace/FRONTEND/LOGOTEXTURES.BUN|"
                 // the Honda's vinyls (2026-11-20): vector vinyls and their thumbnails
                 "=CARS\\VINYLS\\VINYLS.BIN=%s/ModData/addoncars/replace/CARS/VINYLS/VINYLS.BIN|"
                 "=FRONTEND\\VINYLTHUMBNAILS.BUN=%s/ModData/addoncars/replace/FRONTEND/VINYLTHUMBNAILS.BUN",
                 resources, resources, resources, resources, resources, resources, resources);
        set_default("RECOMP_FILE_OVERLAY", spec);
        // - add-on cars (core.nfsu addoncars.inc; tools/addon_cars.py made the files): car lists and names
        snprintf(spec, sizeof spec, "%s/ModData/addoncars/addoncars.txt", resources);
        set_default("RECOMP_NFSU_ADDON_CARS", spec);
        set_default("RECOMP_D3D9_PS_SETTING_REG", "823d0a95d7df282e:30:mod_photoreal,7f36120e05dc47e4:30:mod_photoreal");
        // - VltEd scripts applied to the game's live attribute database (core.nfsu attrib.inc): Redux's parameters
        //   (UCPO), Revamped Pursuits (its Cop Perf variant, the one that leaves the racers' cars as they are), Hard
        //   Drivers (one script per difficulty: "<setting>#<value>" is on at that value)
        snprintf(spec, sizeof spec,
                 "mod_redux=%s/ModData/redux/UCPO.nfsms|mod_revamped_pursuits=%s/ModData/pursuits/RevampedPursuits.nfsms|"
                 "mod_hard_drivers#1=%s/ModData/harddrivers/Easy.nfsms|"
                 "mod_hard_drivers#2=%s/ModData/harddrivers/Normal.nfsms|mod_hard_drivers#3=%s/ModData/harddrivers/Hard.nfsms",
                 resources, resources, resources, resources, resources);
        set_default("RECOMP_NFSU_VLT", spec);
        // - Police Career's own log (it writes scripts\\PoliceCareer.log beside itself): into the profile's Logs
        snprintf(spec, sizeof spec, "scripts\\PoliceCareer.log=%s/PoliceCareer.log", logs);
        set_default("RECOMP_FILE_WRITE_REDIRECT", spec);
        // - Police Career's two SecuROM jump-slot hooks (cells seeded in game.toml): FUN_00571b80 behind `push ebx`
        //   (cell pointer 0x01375840), FUN_0051df10 (cell pointer 0x01375854)
        set_default("RECOMP_SLOT_HOOKS", "571b80:1375840:571b80:1,51df10:1375854:51df10:0");
    }
    // Metal shaders this Mac has compiled before, recompiled in the background
    // at start-up: a cold shader cache stalled the first frames after the EA
    // splash on a first launch of NFS Carbon (2026-10-10).
    snprintf(path, sizeof path, "%s/ShaderWarm", profile);
    mkdirs(path);
    set_default("RECOMP_D3D9_SHADER_WARM_DIR", path);

    // One log per launch, plus Logs/latest.log pointing at it.
    char stamp[32], log[PATH_MAX], latest[PATH_MAX];
    time_t now = time(NULL);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", localtime(&now));
    snprintf(log, sizeof log, "%s/%s.log", logs, stamp);
    // The play journal (2026-11-21): Logs/<stamp>.journal.txt - keys, pad, window, the game's actions, a line of
    // performance a second, the renderer's cost, the system's load, dips and hangs, and how the game closed (kit
    // recomp_journal; the watcher below adds the system lines). RECOMP_JOURNAL=0 turns it off.
    char journal[PATH_MAX];
    snprintf(journal, sizeof journal, "%s/%s.journal.txt", logs, stamp);
    if (!getenv("RECOMP_JOURNAL")) {
        setenv("RECOMP_JOURNAL", journal, 1);
        FILE *jf = fopen(journal, "w");
        if (jf) {
            char when[64];
            strftime(when, sizeof when, "%Y-%m-%d %H:%M:%S", localtime(&now));
            fprintf(jf, "Need for Speed Undercover - play journal, started %s (log %s.log)\n", when, stamp);
            fprintf(jf, "Lines: key/pad/mouse = your input; game = the game's own actions and text; main = the game's "
                        "main loop each second; perf = frames each second; render = the renderer's cost per frame; "
                        "system = CPU/GPU/memory every 5 s; dip/hang = a slow stretch or a stop, with a stack sample "
                        "file; closed = how and when the game ended.\n");
            fclose(jf);
        }
    } else if (!strcmp(getenv("RECOMP_JOURNAL"), "0"))
        unsetenv("RECOMP_JOURNAL");
    const char *jpath = getenv("RECOMP_JOURNAL") ? getenv("RECOMP_JOURNAL") : "";
    int fd = open(log, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        dup2(fd, STDOUT_FILENO);
        dup2(fd, STDERR_FILENO);
        close(fd);
        snprintf(latest, sizeof latest, "%s/latest.log", logs);
        unlink(latest);
        symlink(log, latest);
    }
    prune_logs(logs);

    // The watcher beside the game (2026-11-21; this process becomes the game: same pid). A shell loop, every 5 s:
    // - "system" journal lines: the game's CPU %, the GPU's busy %, memory, the macOS thermal warning if any;
    // - hangs: no new d3d9 "frame pacing" line (printed once a minute from Present) for 130 s while the game lives
    //   -> a note in log and journal + a 3 s stack sample of every thread into Logs/hang-<time>.txt (twice at most);
    // - dips: the journal's "perf" lines under 85% of the frame target for 3 checks in a row while driving -> a 1 s
    //   stack sample into Logs/dip-<time>.txt (2 minutes apart, 10 at most), noted in the journal;
    // - the end: when the game's process is gone, how it ended (its own "closed" line or not = force-quit or crash).
    // RECOMP_NO_HANG_WATCH=1 turns the watcher off.
    if (!getenv("RECOMP_NO_HANG_WATCH")) {
        static const char *script =
            "P=$1; L=$2; D=$3; J=$4; n=0; seen=0; last=$(date +%s); dips=0; low=0; lastdip=0\n"
            "jr() { [ -n \"$J\" ] && echo \"$(date +%H:%M:%S).000 $*\" >> \"$J\"; }\n"
            "while kill -0 $P 2>/dev/null; do\n"
            "  now=$(date +%s)\n"
            "  c=$(grep -c 'frame pacing' \"$L\" 2>/dev/null); c=${c:-0}\n"
            "  if [ \"$c\" != \"$seen\" ]; then seen=$c; last=$now; fi\n"
            "  if [ \"$c\" -gt 0 ] && [ $((now-last)) -gt 130 ] && [ $n -lt 2 ]; then\n"
            "    n=$((n+1)); f=\"$D/hang-$(date +%Y%m%d-%H%M%S).txt\"\n"
            "    echo \"launch watchdog: no new frames for $((now-last)) s - stack samples in $f\" >> \"$L\"\n"
            "    jr \"hang: no new frames for $((now-last)) s - stack samples in $f\"\n"
            "    /usr/bin/sample $P 3 1 -file \"$f\" >/dev/null 2>&1; last=$(date +%s)\n"
            "  fi\n"
            "  if [ -n \"$J\" ]; then\n"
            "    cpu=$(ps -o %cpu= -o rss= -p $P 2>/dev/null)\n"
            "    gpu=$(ioreg -r -c IOAccelerator -d 1 2>/dev/null | grep -o '\"Device Utilization %\"=[0-9]*' | head -1 | cut -d= -f2)\n"
            "    th=$(pmset -g therm 2>/dev/null | grep -i 'level\\|limit' | grep -v 'No ' | tr -s ' ' | tr '\\n' ';')\n"
            "    set -- $cpu\n"
            "    jr \"system: game CPU ${1:-?}% (100% = one core), GPU busy ${gpu:-?}%, memory $(( ${2:-0} / 1024 )) MB${th:+, thermal: $th}\"\n"
            "    set -- \"$P\" \"$L\" \"$D\" \"$J\"\n"
            "    pl=$(grep '^[0-9:.]* perf:' \"$J\" | tail -1)\n"
            "    ml=$(grep '^[0-9:.]* main:' \"$J\" | tail -1)\n"
            "    g=$(echo \"$pl\" | sed -n 's/.*game \\([0-9]*\\)\\..* fps, shown.*/\\1/p')\n"
            "    lim=$(echo \"$pl\" | sed -n 's/.*limit \\([0-9]*\\).*/\\1/p'); hz=$(echo \"$pl\" | sed -n 's/.*screen \\([0-9]*\\) Hz.*/\\1/p')\n"
            "    t=${lim:-60}; [ \"$t\" -le 0 ] && t=60; [ -n \"$hz\" ] && [ \"$hz\" -gt 0 ] && [ \"$hz\" -lt \"$t\" ] && t=$hz\n"
            "    case \"$ml\" in *driving*) drv=1;; *) drv=0;; esac\n"
            "    if [ -n \"$g\" ] && [ $drv = 1 ] && [ $((g*100)) -lt $((t*85)) ]; then low=$((low+1)); else low=0; fi\n"
            "    if [ $low -ge 3 ] && [ $dips -lt 10 ] && [ $((now-lastdip)) -gt 120 ]; then\n"
            "      dips=$((dips+1)); lastdip=$now; f=\"$D/dip-$(date +%Y%m%d-%H%M%S).txt\"\n"
            "      jr \"dip: about $g fps for 15 s while driving (target $t) - stack sample in $f\"\n"
            "      /usr/bin/sample $P 1 1 -file \"$f\" >/dev/null 2>&1; low=0\n"
            "    fi\n"
            "  fi\n"
            "  sleep 5\n"
            "done\n"
            "if [ -n \"$J\" ]; then\n"
            "  if grep -q '^[0-9:.]* closed:' \"$J\"; then jr \"closed: the game's process is gone\";\n"
            "  else jr \"closed: the game's process ended WITHOUT closing itself (force-quit, crash or hang)\"; fi\n"
            "fi\n";
        char pid[16];
        snprintf(pid, sizeof pid, "%d", (int)getpid());
        char *const wargv[] = {"/bin/sh", "-c", (char *)script, "nfsu-hang-watch", pid, log, logs, (char *)jpath, NULL};
        pid_t w;
        posix_spawnattr_t attr;
        posix_spawnattr_init(&attr);
        posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
        posix_spawnattr_setpgroup(&attr, 0);
        extern char **environ;
        posix_spawn(&w, "/bin/sh", NULL, &attr, wargv, environ);
        posix_spawnattr_destroy(&attr);
    }

    if (chdir(game) != 0) {
        fprintf(stderr, "launch: cannot enter %s: %s\n", game, strerror(errno));
        return 1;
    }
    argv[0] = binary;
    execv(binary, argv);
    fprintf(stderr, "launch: cannot run %s: %s\n", binary, strerror(errno));
    return 1;
}
