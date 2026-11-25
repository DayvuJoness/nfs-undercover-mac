// menu_lang.cpp - the settings menu in the game's language (2026-11-17).
//
// The menu is written in English. When the game itself is Russian (the profile registry's "Language" says
// "Russian"), every string the menu shows - page names, section names, row names, values, the help under the
// list, timing notes, statuses and the key prompts - goes through mods_menu_tr: an exact English -> Russian table.
// The kit's own strings are below; a mod adds its own from its manifest (label_ru, help_ru, choices_ru beside
// label, help and choices). Strings with numbers in them ("40 FPS", "3 changes not applied yet ...") are matched
// by pattern. A string with no translation stays English. Every other language keeps the English menu.
// RECOMP_MENU_LANG=ru or en overrides the game's language (tests, A/B).
#include "menu_lang.h"
#include "../platform/os.h"
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <regex>
#include <string>

extern "C" __attribute__((weak)) int recomp_registry_find_string(const char *, char *, int) {
    return 0;
}

namespace {
std::mutex g_mu;
std::map<std::string, std::string> &table() {
    static std::map<std::string, std::string> t;
    return t;
}
int g_lang = -1; // -1 not decided yet, 0 English, 1 Russian

const char *const kRussian[][2] = {
    // Pages and sections.
    {"Graphics", "Графика"},
    {"Mods", "Моды"},
    {"Controls", "Управление"},
    {"Display", "Экран"},
    {"Image quality", "Качество изображения"},
    {"Appearance", "Внешний вид"},
    {"Defaults", "Значения по умолчанию"},
    {"Mod actions", "Действия модов"},
    {"Saves", "Сохранения"},
    {"Nothing waits for Apply: the changes here took effect when you made them.",
     "Применять нечего: изменения здесь уже вступили в силу, когда вы их сделали."},
    {"restart", "перезапуск"},
    {"Extra Options", "Extra Options"},
    {"Save files", "Файлы сохранения"},
    {"Load a save to see its settings", "Загрузите сохранение, чтобы открыть его настройки"},
    {"save", "сохранение"},
    {"shared", "общая"},
    {"this save's own setting", "своя у этого сохранения"},
    {"shared by all saves", "общая для всех сохранений"},
    {"No save files", "Нет файлов сохранения"},
    {"Moves this save to the Trash (Finder can put it back). Delete a save that isn't loaded: if you are playing "
     "it, the game writes it again - choose another save or a new game first.",
     "Перемещает это сохранение в Корзину (из Finder его можно вернуть). Удаляйте сохранение, которое сейчас не "
     "загружено: если вы в нём играете, игра запишет его снова - сначала выберите другое сохранение или новую игру."},
    {"Visual restorations and enhancements", "Восстановление и улучшение графики"},
    {"Controller feedback", "Отдача контроллера"},
    {"Career and customization", "Карьера и тюнинг"},
    {"Unlocks", "Открытие контента"},
    {"Races and pursuits", "Гонки и погони"},
    {"Driving", "Вождение"},
    {"World and weather", "Мир и погода"},
    {"Cheats", "Читы"},
    {"Audio", "Звук"},
    {"Verified fixes and compatibility", "Проверенные исправления и совместимость"},
    {"Other", "Другое"},
    {"Diagnostics", "Диагностика"},
    {"On-screen controls", "Экранное управление"},
    {"Controller", "Джойстик"},
    // 2026-11-24 redesign: categories, subcategories, the marks under a row, the page buttons, short hints.
    {"Screen", "Экран"},
    {"Window and frame rate", "Окно и частота кадров"},
    {"Resolution and sharpness", "Разрешение и чёткость"},
    {"Screen layout", "Размещение на экране"},
    {"Image", "Изображение"},
    {"Detail and distance", "Детализация и дальность"},
    {"Light and colour", "Свет и цвет"},
    {"Camera effects", "Эффекты камеры"},
    {"HDR", "HDR"},
    {"Game mods", "Моды игры"},
    {"Races and police", "Гонки и полиция"},
    {"Cars", "Машины"},
    {"Game and money", "Игра и деньги"},
    {"Police and heat", "Полиция и розыск"},
    {"Driving, camera and keys", "Езда, камера и клавиши"},
    {"Game", "Игра"},
    {"Start-up and menus", "Запуск и меню"},
    {"Buttons and vibration", "Кнопки и вибрация"},
    {"Trackpad", "Тачпад"},
    {"Vibration", "Вибрация"},
    {"Applies immediately", "Действует немедленно"},
    {"Applies on Save", "После «Сохранить»"},
    {"From the next load", "Со следующей загрузки"},
    {"From the next race", "Со следующего заезда"},
    {"Needs a restart", "Требует перезапуска"},
    {"After the game's video setting", "После видеонастройки игры"},
    {"Saved", "Сохранено"},
    {"Waiting for restart", "Ожидание перезапуска"},
    {"Not saved", "Не сохранено"},
    {"Shared", "Общая"},
    {"Save file", "Сохранение"},
    {"Save", "Сохранить"},
    {"Saving...", "Сохраняю..."},
    {"Nothing to save: every change here was saved when you made it.",
     "Сохранять нечего: изменения здесь сохраняются сразу."},
    {"Defaults marked: Save uses them, Discard forgets them.",
     "Значения по умолчанию отмечены: «Сохранить» — применить, «Отменить» — забыть."},
    {"\xe2\x86\x91\xe2\x86\x93 choose   \xe2\x86\x90\xe2\x86\x92 change   Tab page   Esc close",
     "\xe2\x86\x91\xe2\x86\x93 выбор   \xe2\x86\x90\xe2\x86\x92 изменить   Tab страница   Esc закрыть"},
    {"Saves the changes marked \"Not saved\".\nEverything else is saved the moment you change it.",
     "Сохраняет изменения с пометкой «Не сохранено».\nВсё остальное сохраняется сразу, как только вы его меняете."},
    {"Forgets the changes marked \"Not saved\".", "Забывает изменения с пометкой «Не сохранено»."},
    {"Marks every setting of this page at its default value.\nSave uses them, Discard forgets them.",
     "Отмечает для всех пунктов этой страницы значения по умолчанию.\n«Сохранить» применяет их, «Отменить» — забывает."},
    {"Closes the menu (Esc or F10 too).\nChanges not saved yet are forgotten.",
     "Закрывает меню (также Esc или F10).\nНесохранённые изменения забываются."},
    {"How the game is shown:\nWindowed - in a window.\nBorderless - the whole screen, quick to switch to other apps.\n"
     "Full screen - the real full-screen mode.",
     "Как показывать игру:\nВ окне — в обычном окне.\nБез рамки — на весь экран, быстро переключается на другие "
     "программы.\nПолноэкранный — настоящий полноэкранный режим."},
    {"The most frames a second the game draws.\nScreen - your display's rate (120 on a MacBook Pro, 60 on other Macs).\n"
     "40 and 60 - less heat and power.\nMaximum - as many as it can (up to 960).\nOriginal - the game sets the pace.",
     "Сколько кадров в секунду игра рисует самое большее.\nЭкран — частота дисплея (120 на MacBook Pro, 60 на других "
     "Mac).\n40 и 60 — меньше нагрев и расход батареи.\nМаксимум — сколько успеет (до 960).\nКак в игре — темп задаёт "
     "сама игра."},
    {"Frame counters or a frame-time graph in a corner.\nF11 switches it too.",
     "Счётчики кадров или график времени кадра в углу.\nКлавиша F11 тоже переключает."},
    {"On - frames come evenly, one per screen refresh, none thrown away.\nOff - a little less delay, less even.\n"
     "No effect with the frame limit \"original\".",
     "Вкл. — кадры идут ровно, по одному на обновление экрана, без потерь.\nВыкл. — задержка чуть меньше, но менее "
     "плавно.\nПри ограничении «как в игре» не действует."},
    {"Sharper road, wall and ground textures seen at an angle.\nGame's own - as the game asks.\n"
     "Trilinear - without anisotropic filtering.\n4x-16x - the higher, the sharper.\nText, the HUD and movies are not "
     "affected.",
     "Чёткость дороги, стен и земли под углом.\nКак в игре — как просит игра.\nТрилинейная — без анизотропной "
     "фильтрации.\n4x–16x — чем больше, тем чётче.\nТекст, интерфейс и ролики не меняются."},
    {"Moves this save to the Trash (Finder can put it back).\nDon't delete the save you are playing: the game writes it "
     "again.",
     "Перемещает сохранение в Корзину (из Finder его можно вернуть).\nНе удаляйте сохранение, в котором играете: игра "
     "запишет его снова."},
    // Display rows and values.
    {"Display", "Экран"},
    {"windowed", "в окне"},
    {"borderless", "во весь экран без рамки"},
    {"fullscreen", "полноэкранный"},
    {"Frame limit", "Ограничение кадров"},
    {"original", "как в игре"},
    {"Performance overlay", "Счётчик производительности"},
    {"off", "выкл."},
    {"counters", "счётчики"},
    {"graph", "график"},
    {"World filtering", "Фильтрация текстур мира"},
    {"Game's own", "Как в игре"},
    {"Trilinear", "Трилинейная"},
    {"4x anisotropic", "Анизотропная 4x"},
    {"8x anisotropic", "Анизотропная 8x"},
    {"16x anisotropic", "Анизотропная 16x"},
    {"Vertical sync", "Вертикальная синхронизация"},
    {"On", "Вкл."},
    {"Off", "Выкл."},
    {"Resolution", "Разрешение"},
    {"Textures", "Текстуры"},
    {"HD pack", "HD-пакет"},
    {"UI scale", "Масштаб интерфейса"},
    {"auto", "авто"},
    // The Controls page's controller picture (2026-11-22).
    {"Controller connected: ", "Подключён джойстик: "},
    {"No controller connected", "Джойстик не подключён"},
    {"Select in menus", "Выбор в меню"},
    {"Back", "Назад"},
    {"Gas", "Газ"},
    {"Pause", "Пауза"},
    {"Steering", "Руль"},
    {"Menu items", "Пункты меню"},
    {"Set in the game", "Назначается в игре"},
    {"Other buttons: the game's Pause > Settings > Controls.", "Остальные кнопки: в игре Пауза > Настройки > Управление."},
    {"Brake, reverse", "Тормоз, задний ход"},
    {"Select in menus, nitro", "Выбор в меню, нитро"},
    {"Back, speedbreaker", "Назад, замедление"},
    {"Handbrake", "Ручник"},
    {"Look back", "Взгляд назад"},
    {"Helicopter: stick flies, right trigger up, left down.", "Вертолёт: стик - полёт, правый курок - вверх, левый - вниз."},
    // Actions.
    {"Apply", "Применить"},
    {"Discard", "Отменить"},
    {"Restore appearance defaults", "Вернуть внешний вид по умолчанию"},
    {"Restore this section's defaults", "Вернуть значения раздела по умолчанию"},
    {"Restore graphics defaults", "Вернуть графику по умолчанию"},
    {"Close", "Закрыть"},
    {"Tab or click to change page", "Tab или щелчок - другая страница"},
    {"Tab, or click a page name", "Tab или щелчок по названию страницы"},
    {"Display mode", "Режим экрана"},
    // Timing.
    {"Applies now", "Действует сразу"},
    {"Applies when you choose Apply", "Действует после «Применить»"},
    {"Applies at the next screen or load", "Действует со следующего экрана или загрузки"},
    {"Applies to the next eligible event", "Действует со следующего подходящего заезда"},
    {"New careers only", "Только для новой карьеры"},
    {"Applies after restarting the game", "Действует после перезапуска игры"},
    {"Applies when a detail level is chosen in the game's Video menu",
     "Действует после выбора уровня детализации в видеонастройках игры"},
    // Statuses.
    {"Applying...", "Применяю..."},
    {"Already at the defaults.", "Уже стоят значения по умолчанию."},
    {"Discarded the changes.", "Изменения отменены."},
    {"Type the number, then press Return or Apply.", "Наберите число, затем нажмите Enter или «Применить»."},
    // Help.
    {"Window, borderless full screen or full screen.",
     "Как показывать игру: в окне, во весь экран без рамки (быстро переключается на другие программы) или в "
     "настоящем полноэкранном режиме."},
    {"The most frames a second the game draws. \"Screen\" is your display's refresh rate (120 on a MacBook Pro with "
     "ProMotion, 60 on other Macs). 40 and 60 save power and heat. \"Maximum\" draws as many as it can (up to 960); "
     "the screen still shows at most its refresh rate. \"Original\" leaves the pacing to the game.",
     "Сколько кадров в секунду игра рисует самое большее. «Экран» - частота вашего дисплея (120 на MacBook Pro с "
     "ProMotion, 60 на остальных Mac). 40 и 60 экономят батарею и меньше греют Mac. «Максимум» - сколько игра "
     "успеет (до 960), но экран всё равно покажет не больше своей частоты. «Как в игре» - темп задаёт сама игра."},
    {"On: each new frame waits for the screen to be ready for it, so frames arrive evenly, one per refresh (or every "
     "second or third refresh with a lower frame limit), with no frames thrown away. Off: the game draws as fast as "
     "the frame limit lets it and the screen shows the newest frame at each refresh; a little less delay, less even. "
     "Has no effect with the frame limit \"original\".",
     "Вкл.: каждый новый кадр ждёт, пока экран будет готов его показать, поэтому кадры идут ровно, по одному на "
     "обновление экрана (или раз в два-три обновления при меньшем ограничении), и ни один не пропадает зря. Выкл.: "
     "игра рисует так быстро, как позволяет ограничение кадров, а экран показывает самый свежий кадр; задержка "
     "чуть меньше, но плавность хуже. При ограничении «как в игре» не действует."},
    {"Frame counters or a frame-time graph in a corner (F11 cycles it too).",
     "Счётчики кадров или график времени кадра в углу экрана. Клавиша F11 переключает его тоже."},
    {"Sharper road, wall and terrain textures at an angle. \"Game's own\" keeps the filtering the game asks for; 4x-16x "
     "apply that anisotropy to every world texture, Trilinear turns it off. Text, the HUD, movies, shadows and "
     "lighting are not affected.",
     "Чёткость дороги, стен и земли под углом. «Как в игре» оставляет фильтрацию, которую просит игра; 4x-16x "
     "включают такую анизотропную фильтрацию для всех текстур мира, «Трилинейная» её выключает. Текст, интерфейс, "
     "ролики, тени и освещение не меняются."},
    {"Commit the changes marked with a dot.", "Применить изменения, отмеченные точкой."},
    {"Forget the changes not yet applied.", "Отменить ещё не применённые изменения."},
    {"Marks the Appearance section's default values (the Definitive look) as changes; Apply commits them. Quality, "
     "anti-aliasing, filtering, resolution and the frame limit stay as they are.",
     "Отмечает значения раздела «Внешний вид» по умолчанию как изменения; «Применить» их включает. Качество, "
     "сглаживание, фильтрация, разрешение и ограничение кадров не меняются."},
    {"Marks the default values of World filtering, Quality, anti-aliasing and the Appearance section as changes; "
     "Apply commits them. The window mode, resolution and frame limit stay as they are.",
     "Отмечает значения по умолчанию для фильтрации, качества, сглаживания и раздела «Внешний вид» как изменения; "
     "«Применить» их включает. Режим окна, разрешение и ограничение кадров не меняются."},
    {"Keys: Up/Down choose    Left/Right change    Enter select    Tab next page    Esc or F10 close\n"
     "Mouse: point at a row, click it or its < > arrows, click a page name, wheel scrolls    Pad: L1/R1 pages",
     "Клавиши: вверх/вниз - выбор, влево/вправо - изменить, Tab - страница, Esc или F10 - закрыть\n"
     "Мышь: наведите на строку - подсказка внизу, щелчок или < > - изменить, колёсико - прокрутка"},
    // Controls rows.
    {"Controls", "Управление"},
    {"Edit controls", "Настроить управление"},
    {"Controls size", "Размер кнопок"},
    {"Controls opacity", "Прозрачность кнопок"},
    {"Button haptics", "Отклик кнопок"},
    {"Editor snapping", "Привязка в редакторе"},
    {"Pad with controller", "Кнопки при подключённом контроллере"},
    {"Small", "Маленький"},
    {"Medium", "Средний"},
    {"Large", "Большой"},
    {"hidden", "скрыты"},
    {"on", "вкл."},
};

void fill_builtin() {
    auto &t = table();
    if (!t.empty())
        return;
    for (const auto &p : kRussian)
        t.emplace(p[0], p[1]);
}

int decide() {
    if (const char *e = recomp_env("MENU_LANG"))
        return !strncmp(e, "ru", 2) ? 1 : 0;
    char lang[64] = {};
    if (!recomp_registry_find_string("Language", lang, sizeof lang))
        return 0;
    return strstr(lang, "Russian") || strstr(lang, "russian") || !strncmp(lang, "ru", 2) ? 1 : 0;
}

// "40 FPS", "screen (120 FPS)", "maximum (960 FPS)", "Applied 2 changes." ...
std::string by_pattern(const std::string &s) {
    std::smatch m;
    static const std::regex fps("^(\\d+) FPS$"), screen("^screen \\((\\d+) FPS\\)$"),
        maximum("^maximum \\((\\d+) FPS\\)$"), applied("^Applied (\\d+) changes?\\.$"),
        applied_of("^Applied (\\d+) of (\\d+) changes\\.$"), saved("^Saved (\\d+) changes?\\.$"),
        saved_of("^Saved (\\d+) of (\\d+) changes\\.$"), not_saved("^(\\d+) changes? not saved: Save or Discard\\.$"),
        restart_wait("^Restart the game to use (\\d+) changes? waiting for a restart\\.$"),
        pending("^(\\d+) changes? not applied yet: choose Apply or Discard \\(Esc discards\\)\\.$"),
        marked("^(.*) defaults marked; choose Apply to use them\\.$"),
        restore_group("^Marks the default values of \"(.*)\" as changes; Apply commits them, Discard forgets them\\. "
                      "Other sections stay as they are\\.$");
    static const std::regex save_del("^Delete save \"(.*)\"$"), save_again("^Press again to delete \"(.*)\"\\.?$"),
        save_moved("^Moved \"(.*)\" to the Trash\\.$"), save_failed("^Could not move \"(.*)\" to the Trash\\.$");
    static const std::regex restart_n("^Restart the game to use (\\d+) changes? marked \"restart\"\\.$");
    if (std::regex_match(s, m, saved))
        return "Сохранено изменений: " + m[1].str() + ".";
    if (std::regex_match(s, m, saved_of))
        return "Сохранено " + m[1].str() + " из " + m[2].str() + ".";
    if (std::regex_match(s, m, not_saved))
        return "Не сохранено изменений: " + m[1].str() + " — «Сохранить» или «Отменить».";
    if (std::regex_match(s, m, restart_wait))
        return "Перезапустите игру, чтобы применить изменения: " + m[1].str() + ".";
    if (std::regex_match(s, m, restart_n))
        return "Перезапустите игру, чтобы применить изменения с пометкой «перезапуск»: " + m[1].str() + ".";
    if (std::regex_match(s, m, save_del))
        return "Удалить сохранение «" + m[1].str() + "»";
    if (std::regex_match(s, m, save_again))
        return "Нажмите ещё раз, чтобы удалить «" + m[1].str() + "»";
    if (std::regex_match(s, m, save_moved))
        return "«" + m[1].str() + "» перемещено в Корзину.";
    if (std::regex_match(s, m, save_failed))
        return "Не удалось переместить «" + m[1].str() + "» в Корзину.";
    if (std::regex_match(s, m, fps))
        return m[1].str() + " к/с";
    if (std::regex_match(s, m, screen))
        return "экран (" + m[1].str() + " к/с)";
    if (std::regex_match(s, m, maximum))
        return "максимум (" + m[1].str() + " к/с)";
    if (std::regex_match(s, m, applied))
        return "Применено изменений: " + m[1].str() + ".";
    if (std::regex_match(s, m, applied_of))
        return "Применено " + m[1].str() + " из " + m[2].str() + ".";
    if (std::regex_match(s, m, pending))
        return "Не применено изменений: " + m[1].str() + ". Выберите «Применить» или «Отменить» (Esc отменяет).";
    if (std::regex_match(s, m, marked))
        return "Значения по умолчанию для «" + mods_menu_tr(m[1].str()) + "» отмечены; нажмите «Применить».";
    if (std::regex_match(s, m, restore_group))
        return "Отмечает значения раздела «" + mods_menu_tr(m[1].str()) +
               "» по умолчанию как изменения; «Применить» их включает, «Отменить» забывает. Другие разделы не меняются.";
    return s;
}
} // namespace

bool mods_menu_russian() {
    std::lock_guard lock(g_mu);
    if (g_lang < 0) {
        g_lang = decide();
        std::printf("mods: settings menu language: %s\n", g_lang ? "Russian (the game's)" : "English");
        std::fflush(stdout);
    }
    return g_lang == 1;
}

void mods_menu_tr_add(const std::string &en, const std::string &ru) {
    if (en.empty() || ru.empty())
        return;
    std::lock_guard lock(g_mu);
    fill_builtin();
    table()[en] = ru;
}

void mods_menu_tr_add_choices(const std::string &en, const std::string &ru) {
    size_t a = 0, b = 0;
    while (a <= en.size() && b <= ru.size()) {
        const size_t ea = en.find('|', a), rb = ru.find('|', b);
        mods_menu_tr_add(en.substr(a, ea == std::string::npos ? std::string::npos : ea - a),
                         ru.substr(b, rb == std::string::npos ? std::string::npos : rb - b));
        if (ea == std::string::npos || rb == std::string::npos)
            break;
        a = ea + 1, b = rb + 1;
    }
}

std::string mods_menu_tr(const std::string &en) {
    if (en.empty() || !mods_menu_russian())
        return en;
    {
        std::lock_guard lock(g_mu);
        fill_builtin();
        auto it = table().find(en);
        if (it != table().end())
            return it->second;
    }
    std::string out = by_pattern(en);
    if (out == en && std::getenv("RECOMP_MENU_TR_LOG")) {
        static std::mutex log_mu;
        static std::map<std::string, int> told;
        std::lock_guard lock(log_mu);
        if (!told[en]++) {
            std::printf("mods: settings menu: no Russian for \"%s\"\n", en.c_str());
            std::fflush(stdout);
        }
    }
    return out;
}
