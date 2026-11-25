# The F10 settings of core.nfsu: tools/dev/gen_nfsu_settings.py writes them into mods/core/nfsu/mod.toml.
# group = the F10 subcategory (kit/mods/settings_menu.cpp kSubs); "|" in a hint = a new line in its card.
add("pause_in_menu", "bool", True, "live", "career",
    "Settings menu pauses the game", "Меню настроек ставит игру на паузу",
    "On — the game stops while F10 is open (race clock, cops and traffic wait).|Off — the game keeps running behind the menu.",
    "Вкл. — пока открыто F10, игра стоит (время заезда, полиция и трафик ждут).|Выкл. — игра идёт дальше за меню.")
add("ea_nation_signin", "bool", False, "live", "career",
    "EA Nation sign-in after loading a save", "Вход в EA Nation после загрузки сохранения",
    "EA Nation closed long ago: the game only tried to connect and then waited for Enter.|Off — skip that sign-in after a save loads.|On — as in the game.",
    "Сервис EA Nation давно закрыт: игра лишь пыталась подключиться и ждала Enter.|Выкл. — пропускать этот вход после загрузки сохранения.|Вкл. — как в игре.")
add("startup_logos", "bool", True, "restart", "career",
    "Logo movie at start-up", "Ролик с логотипами при запуске",
    "The EA / Black Box / Need for Speed logo movie (about 9 s) when the game starts.|Off — the title screen comes 9 s sooner.",
    "Ролик с логотипами EA / Black Box / Need for Speed (около 9 с) при запуске.|Выкл. — титульный экран на 9 с раньше.")
add("hdr_output", "bool", False, "restart", "graphics.hdr",
    "HDR", "HDR",
    "On an HDR screen (MacBook Pro, HDR monitor) the brightest spots of a race — sun, lights, glints — shine brighter than white.|Menus, the HUD and text stay as without HDR.",
    "На HDR-экране (MacBook Pro, HDR-монитор) самые яркие места заезда — солнце, огни, блики — светят ярче белого.|Меню, приборы и надписи — как без HDR.")
add("hdr_peak", "int", 15, "live", "graphics.hdr",
    "HDR: brightest light (%)", "HDR: самый яркий свет (%)",
    "How bright the brightest white gets with HDR on.|15 — about three times plain white on a MacBook Pro.|Higher — more dazzling, lower — calmer.",
    "Насколько ярким становится самый яркий белый при HDR.|15 — примерно втрое ярче обычного белого на MacBook Pro.|Больше — ослепительнее, меньше — спокойнее.",
    None, None, 5, 60)
add("hdr_world", "int", 130, "live", "graphics.hdr",
    "HDR: brightness of the world (%)", "HDR: яркость мира (%)",
    "With HDR on, how bright the world of a race is shown, so colours keep up with the lifted lights.|100 — as without HDR.",
    "При HDR — насколько ярче показывается мир заезда, чтобы краски не терялись рядом с огнями.|100 — как без HDR.",
    None, None, 100, 200)
add("upscaling", "bool", True, "live", "graphics.resolution",
    "Sharper upscaling (MetalFX)", "Чёткое увеличение (MetalFX)",
    "When the game draws fewer pixels than the screen has:|On — Apple MetalFX enlarges it with sharp edges and text.|Off — a plain, softer stretch.",
    "Когда игра рисует меньше пикселей, чем у экрана:|Вкл. — MetalFX от Apple увеличивает с чёткими краями и надписями.|Выкл. — обычное, более мягкое растягивание.")
add("sharpening", "int", 0, "live", "graphics.resolution",
    "Picture sharpening", "Резкость картинки",
    "Crisper edges and fine detail; smooth areas like the sky stay as they are.|Off — exactly as the game draws.|Light — barely noticeable.|Medium, Strong — clearly crisper.",
    "Чётче края и мелкие детали; ровные места вроде неба не меняются.|Выкл. — ровно как рисует игра.|Лёгкая — почти незаметна.|Средняя, Сильная — заметно чётче.",
    "Off (as in the game)|Light|Medium|Strong", "Выкл. (как в игре)|Лёгкая|Средняя|Сильная", 0, 3)
add("motion_blur", "int", 0, "live", "graphics.effects",
    "Speed blur at the screen edges", "Размытие краёв на скорости",
    "At speed the game blurs the sides of the screen; the centre stays sharp.|Smooth — one even smear.|As in the game — stepped copies.|Lighter — smooth and 40% weaker.|Off — no speed blur.",
    "На скорости игра размывает края экрана; центр остаётся чётким.|Плавное — ровный мазок.|Как в игре — ступенчатые копии.|Слабее — плавное и на 40% слабее.|Выкл. — без размытия.",
    "Smooth|As in the game (stepped)|Lighter|Off", "Плавное|Как в игре (ступеньками)|Слабее|Выкл.", 0, 3)
add("colour_filter", "int", 0, "restart", "graphics.light",
    "Colour filter", "Цветовой фильтр",
    "The game tints the whole picture yellow-orange.|As in the game — the full tint.|Softer — half of it.|Neutral — true colours, a little darker.",
    "Игра окрашивает картинку в жёлто-оранжевый.|Как в игре — полный оттенок.|Мягче — вдвое слабее.|Нейтральный — настоящие цвета, чуть темнее.",
    "As in the game (yellow-orange)|Softer|Neutral (no tint)", "Как в игре (жёлто-оранжевый)|Мягче|Нейтральный (без оттенка)", 0, 2)
add("render_scale", "int", 2, "restart", "graphics.resolution",
    "Render resolution", "Разрешение отрисовки",
    "How many pixels the game draws:|Screen — the Mac's full Retina resolution (heaviest).|1.5x — smoother edges, about twice the work.|Game resolution — as set in the game.|Half the screen — a quarter of the pixels by MetalFX, much cooler.",
    "Сколько пикселей рисует игра:|Экран — полное Retina-разрешение Mac (тяжелее всего).|1,5x — ровнее края, примерно вдвое больше работы.|Разрешение игры — как выбрано в игре.|Половина экрана — вчетверо меньше пикселей через MetalFX, намного прохладнее.",
    "Screen (heaviest)|1.5x (smoother)|Game resolution|Half the screen (cooler)", "Экран (тяжелее всего)|1,5x (ровнее)|Разрешение игры|Половина экрана (прохладнее)", 0, 3)
add("menu_fps", "int", 0, "live", "graphics.display",
    "Menu frame rate", "Частота кадров в меню",
    "Frames a second in menus, loading screens and the pause menu.|45 — as on the original PC.|30 — saves the most power.|60, Not limited — smoother.|Driving is not affected.",
    "Кадров в секунду в меню, загрузках и паузе.|45 — как на оригинальном ПК.|30 — больше всего экономит батарею.|60, Без ограничения — плавнее.|На езду не влияет.",
    "45 (as on the original PC)|30|60|Not limited", "45 (как на ПК)|30|60|Без ограничения", 0, 3)
add("depth_of_field", "int", 1, "live", "graphics.effects",
    "Depth of field", "Глубина резкости",
    "The camera blurs what is nearer and farther than its focus (most when it circles a standing car).|As in the game — full blur.|Weaker — half.|Off — everything sharp.",
    "Камера размывает то, что ближе и дальше фокуса (сильнее всего, когда облетает стоящую машину).|Как в игре — полное размытие.|Слабее — вполовину.|Выкл. — всё чёткое.",
    "As in the game|Weaker|Off", "Как в игре|Слабее|Выкл.", 0, 2)
add("notch_area", "bool", True, "restart", "graphics.layout",
    "Picture beside the notch", "Картинка рядом с «чёлкой»",
    "Full screen on a MacBook with a notch:|On — the picture fills the strips beside the notch too (pick the full screen size in the game, e.g. 3024x1964).|Off — below the notch, a black strip at the top.",
    "Полный экран на MacBook с «чёлкой»:|Вкл. — картинка занимает и полосы по бокам «чёлки» (выберите в игре полный размер экрана, например 3024x1964).|Выкл. — ниже «чёлки», с чёрной полосой сверху.")
add("hud_fill", "bool", True, "live", "graphics.layout",
    "Menus fill the screen's height", "Меню на всю высоту экрана",
    "On a screen narrower than 16:9 (MacBook 16:10):|On — menus, the map and loading screens fill the height, no bands.|Off — the 16:9 layout with bands above and below.",
    "На экране уже 16:9 (MacBook 16:10):|Вкл. — меню, карта и загрузки на всю высоту, без полос.|Выкл. — макет 16:9 с полосами сверху и снизу.")
add("shadow_softness", "int", 2, "restart", "graphics.detail",
    "Shadow softness", "Мягкость теней",
    "Soft shadow edges, as in real light: sharp at the object, softer farther away.|Off — the game's hard, stepped shadows.|Light, Medium, Strong — how soft.",
    "Мягкие края теней, как при настоящем свете: чёткие у предмета, мягче вдали.|Выкл. — жёсткие ступенчатые тени игры.|Слабо, Средне, Сильно — насколько мягкие.",
    "Off (as in the game)|Light|Medium|Strong", "Выкл. (как в игре)|Слабо|Средне|Сильно", 0, 3)
add("shadow_reflection_detail", "int", 0, "restart", "graphics.detail",
    "Shadows and reflections", "Тени и отражения",
    "How finely shadows and road and car reflections are drawn.|Quality — twice the game's size, the sharpest.|Balanced — shadows 1.5x, less heat.|Smoothness — as in the game, the most frames.",
    "Насколько подробно рисуются тени и отражения на дороге и машинах.|Качество — вдвое крупнее, чем в игре, самые чёткие.|Баланс — тени в 1,5 раза, меньше нагрев.|Плавность — как в игре, больше всего кадров.",
    "Quality|Balanced|Smoothness", "Качество|Баланс|Плавность", 0, 2)
add("smoke_resolution", "int", 0, "restart", "graphics.detail",
    "Smoke and dust resolution", "Разрешение дыма и пыли",
    "Resolution of smoke, dust and tyre smoke.|Game's quarter — as originally, the coolest.|Half — a good balance.|Full — the sharpest, the graphics chip works much harder.",
    "Разрешение дыма, пыли и дыма от шин.|Четверть, как в игре — как было, меньше всего нагрев.|Половина — хороший баланс.|Полное — чётче всего, но видеочип работает намного больше.",
    "Game's quarter|Half|Full", "Четверть, как в игре|Половина|Полное", 0, 2)
add("draw_distance", "int", 2, "live", "graphics.detail",
    "Draw distance", "Дальность прорисовки",
    "How far shadows and small scenery are drawn.|As in the game — shadows end at about 170 m.|Farther — shadows to 320 m, all scenery layers.|Much farther — shadows to 500 m.",
    "Насколько далеко рисуются тени и мелкие предметы.|Как в игре — тени кончаются примерно в 170 м.|Дальше — тени до 320 м, все слои декораций.|Гораздо дальше — тени до 500 м.",
    "As in the game|Farther|Much farther", "Как в игре|Дальше|Гораздо дальше", 0, 2)
add("bloom", "int", 1, "live", "graphics.light",
    "Glow round bright things", "Свечение вокруг ярких мест",
    "The soft glow over bright things: headlights, street lights, the sky over trees.|As in the game — full glow.|Weaker — half.|Off — crisp lights, no halo.",
    "Мягкое свечение вокруг ярких мест: фары, фонари, небо над деревьями.|Как в игре — полное.|Слабее — вполовину.|Выкл. — чёткие огни без ореола.",
    "As in the game|Weaker|Off", "Как в игре|Слабее|Выкл.", 0, 2)
add("gloss", "int", 1, "live", "graphics.light",
    "Gloss of roads and buildings", "Блеск дорог и зданий",
    "How shiny roads, buildings and the ground are (car paint is always as in the game).|As in the game — the original glare, a dry road may look wet.|Softer — toned down.|Matte — toned down further.",
    "Насколько блестят дороги, здания и земля (краска машин всегда как в игре).|Как в игре — исходные блики, сухая дорога может выглядеть мокрой.|Мягче — блики приглушены.|Матово — ещё слабее.",
    "As in the game|Softer|Matte", "Как в игре|Мягче|Матово", 0, 2)
add("mod_redux", "bool", True, "live", "visual",
    "Need for Speed Undercover Redux", "Need for Speed Undercover Redux",
    "The Redux mod: new textures of roads, ground and buildings, and its game parameters (cameras, glow, physics).|Its InGameA.bun is read at the next load.",
    "Мод Redux: новые текстуры дорог, земли и зданий и его параметры игры (камеры, свечение, физика).|Его InGameA.bun читается при следующей загрузке.")
add("mod_prostreet_sky", "bool", True, "live", "visual",
    "ProStreet sky", "Небо из ProStreet",
    "The ProStreet Sky mod: the sky and clouds of Need for Speed ProStreet.",
    "Мод ProStreet Sky: небо и облака из Need for Speed ProStreet.")
add("mod_hc_hud", "bool", True, "live", "visual",
    "High-contrast HUD", "Контрастный HUD",
    "The High Contrast HUD mod: a clearer tachometer, map and race icon.|Shows from the next scene load.",
    "Мод High Contrast HUD: более чёткие тахометр, карта и значок гонки.|Видно со следующей загрузки сцены.")
add("mod_revamped_pursuits", "bool", True, "live", "gameplay",
    "Revamped Pursuits", "Revamped Pursuits",
    "The Revamped Pursuits mod: police cars, their numbers, support and tactics at each heat level reworked.|A pursuit already going keeps part of its start.",
    "Мод Revamped Pursuits: полицейские машины, их число, поддержка и тактика на каждом уровне розыска переработаны.|Уже идущая погоня сохраняет часть начальных значений.")
add("mod_eo_collectors", "bool", True, "live", "extraoptions.game",
    "Collector's Edition", "Коллекционное издание",
    "The game counts as the Collector's Edition, with its extra cars and events.",
    "Игра считается коллекционным изданием, с его дополнительными машинами и событиями.")
add("mod_eo_no_tap_reverse", "bool", True, "live", "extraoptions.driving",
    "No reverse on a double brake tap", "Без заднего хода по двойному тормозу",
    "On — a double tap of the brake no longer engages reverse.|Hold the brake to reverse.",
    "Вкл. — двойное нажатие тормоза не включает задний ход.|Для заднего хода держите тормоз.")
add("mod_eo_unlock_all", "bool", False, "live", "extraoptions.game",
    "Everything unlocked", "Всё разблокировано",
    "Every car, part and event open; the money is kept at the amount below.|Off — your own progress comes back.|Key: 6.",
    "Открыты все машины, детали и события; деньги держатся на сумме ниже.|Выкл. — возвращается ваш прогресс.|Клавиша: 6.")
add("mod_eo_cash", "int", 10000000, "live", "extraoptions.game",
    "Career money", "Деньги в карьере",
    "The money you have now.|Click the field (or Return), type the amount, then Apply or Return.|With Everything unlocked the money is topped back up to it.",
    "Сколько денег у вас сейчас.|Щёлкните поле (или Enter), наберите сумму, затем «Применить» или Enter.|При «Всё разблокировано» деньги пополняются до неё.",
    None, None, 0, 999999999, step=1000000, unit="$")
add("mod_eo_autodrive", "bool", False, "live", "extraoptions.driving",
    "Autopilot", "Автопилот",
    "In free roam the game drives the car itself, like its demo driver.|Off when the game starts.|Key: 7.",
    "В свободной езде игра ведёт машину сама, как демо-водитель.|При запуске выключен.|Клавиша: 7.")
add("mod_eo_heat_hack", "bool", False, "live", "extraoptions.police",
    "Choose the heat level", "Выбор уровня розыска",
    "On — your heat level is the one chosen below.|Off — the game's own.|Key: 8.",
    "Вкл. — уровень розыска — тот, что выбран ниже.|Выкл. — как в игре.|Клавиша: 8.")
add("mod_eo_heat_level", "int", 1, "live", "extraoptions.police",
    "Heat level", "Уровень розыска",
    "The heat level used while Choose the heat level is on.|1 — the calmest police, 5 — the toughest.|Key: 9.",
    "Уровень розыска, пока включён «Выбор уровня розыска».|1 — самая спокойная полиция, 5 — самая жёсткая.|Клавиша: 9.",
    None, None, 1, 5)
add("mod_eo_cops", "bool", True, "live", "extraoptions.police",
    "Police in the city", "Полиция в городе",
    "Off — no police cars and no pursuits.|On — as in the game.|Key: 0.",
    "Выкл. — нет полицейских машин и погонь.|Вкл. — как в игре.|Клавиша: 0.")
add("mod_eo_lights", "bool", False, "live", "extraoptions.police",
    "Emergency lights on your car", "Мигалки на вашей машине",
    "Flashing emergency lights on the car you drive.|Off when the game starts.|Key: O (a police car's own lights: H).",
    "Проблесковые огни на машине, которой вы управляете.|При запуске выключено.|Клавиша: O (мигалки служебной машины — H).")
add("mod_eo_freeze_camera", "bool", False, "live", "extraoptions.driving",
    "Freeze the camera", "Заморозить камеру",
    "The camera stays put while the car drives on — for screenshots.|Off when the game starts.|Key: -.",
    "Камера стоит на месте, а машина едет дальше — для снимков.|При запуске выключено.|Клавиша: -.")
add("mod_eo_hotkeys", "bool", True, "live", "extraoptions.driving",
    "Keys for these options", "Клавиши для этих функций",
    "While you drive these keys switch the options above:|6 everything unlocked, 7 autopilot|8 choose the heat level, 9 next level|0 police, O lights, - frozen camera",
    "Во время езды эти клавиши переключают функции выше:|6 всё разблокировано, 7 автопилот|8 выбор уровня розыска, 9 следующий уровень|0 полиция, O мигалки, - заморозка камеры")
add("mod_police_career", "bool", False, "live", "gameplay",
    "Police Career", "Полицейская карьера",
    "The Police Career mod: play as a police officer in free roam (Police mode in the pause menu).|Own garage and shop, patrols, pursuits, the Interceptor event.|Off ends police mode.",
    "Мод Police Career: игра за полицейского в свободной езде («Режим полицейского» в меню паузы).|Свои гараж и магазин, патрули, погони, событие «Перехват».|Выключение завершает режим полицейского.")
add("mod_addon_cars", "bool", False, "live", "cars",
    "Add-on cars", "Машины-аддоны",
    "15 extra cars in the car lot: Koenigsegg, Ferrari, Lamborghini, Aston Martin, Mercedes-AMG, Alfa Romeo, Honda, Subaru, Toyota, Lexus.|Two Ladas in a Russia section.|Off hides them from the car lot and garages.",
    "15 новых машин у автодилера: Koenigsegg, Ferrari, Lamborghini, Aston Martin, Mercedes-AMG, Alfa Romeo, Honda, Subaru, Toyota, Lexus.|Две Лады — в разделе «Россия».|Выкл. прячет их из автосалона и гаражей.")
add("mod_hard_drivers", "int", 0, "live", "gameplay",
    "Racer difficulty (Hard Drivers)", "Сложность соперников (Hard Drivers)",
    "The Hard Drivers mod: racers with more skill and speed.|Off — the game's racers.|Easy — a little harder.|Normal — between.|Hard — the mod's level: they stay on your tail.",
    "Мод Hard Drivers: соперники опытнее и быстрее.|Выкл. — соперники игры.|Лёгкая — чуть сложнее.|Средняя — между ними.|Сложная — уровень мода: держатся у вас на хвосте.",
    "Off|Easy|Normal|Hard", "Выкл.|Лёгкая|Средняя|Сложная", 0, 3)
add("mod_porsche_pack", "bool", False, "live", "cars",
    "Porsche Pack cars", "Автомобили Porsche Pack",
    "26 Porsches of the Porsche Pack (959, 918, many 911s, Boxsters) in the car lot.|Off hides them from the car lot and garages.",
    "26 машин Porsche Pack (959, 918, разные 911, Boxster) у автодилера.|Выкл. прячет их из автосалона и гаражей.")
add("mod_photoreal", "bool", True, "live", "visual",
    "Photorealistic look", "Фотореалистичный вид",
    "The Photorealistic mod's look: bluer highlights, brighter midtones, fuller colours, a warm glow.|Only the 3D scene; the HUD and menus keep their colours.",
    "Вид мода Photorealistic: голубее света, ярче средние тона, насыщеннее краски, тёплое свечение.|Только 3D-сцена; HUD и меню не меняются.")
add("trackpad_haptics", "int", 2, "live", "controls.trackpad",
    "Trackpad: crashes, smashes and gear changes", "Тачпад: удары, разрушения и передачи",
    "The Force Touch trackpad knocks for crashes, smashes, gear changes, the race end and menu moves.|Best felt with a finger on it.|Light — weaker.",
    "Тачпад Force Touch щёлкает при ударах, разрушениях, переключении передач, в конце гонки и в меню.|Лучше всего чувствуется пальцем.|Слабо — слабее.",
    "Off|Light|Strong", "Выкл.|Слабо|Сильно", 0, 2)
add("engine_vibration", "int", 2, "live", "controls.trackpad",
    "Trackpad: the engine accelerating", "Тачпад: двигатель при разгоне",
    "While you accelerate the trackpad beats with the engine: soft at low revs, faster up the revs, a buzz at the limiter.|Light — about half as strong.",
    "Пока вы разгоняетесь, тачпад пульсирует с мотором: мягко на низких оборотах, чаще с ростом, «бррр» на отсечке.|Слабо — примерно вдвое слабее.",
    "Off|Light|Strong", "Выкл.|Слабо|Сильно", 0, 2)
add("pad_haptics", "int", 2, "live", "controls.pad",
    "Controller: crashes, smashes and gear changes", "Джойстик: удары, разрушения и передачи",
    "Controller rumble for crashes, smashes, gear changes, the race end and menu moves.|While a controller is connected the trackpad stays quiet.|Light — weaker.",
    "Вибрация джойстика при ударах, разрушениях, переключении передач, в конце гонки и в меню.|Пока джойстик подключён, тачпад молчит.|Слабо — слабее.",
    "Off|Light|Strong", "Выкл.|Слабо|Сильно", 0, 2)
add("pad_layout", "int", 1, "live", "controls.pad",
    "Controller layout for driving", "Раскладка джойстика за рулём",
    "Console — RT gas, LT brake and reverse, A nitro, B speedbreaker, X handbrake, Y look back, RB camera, LB reset.|As in the game — the PC game's bindings (no nitro, no triggers).",
    "Консольная — RT газ, LT тормоз и задний ход, A нитро, B замедление, X ручник, Y взгляд назад, RB камера, LB вернуть на дорогу.|Как в игре — кнопки PC-версии (без нитро и курков).",
    "As in the game|Console", "Как в игре|Консольная", 0, 1)
add("pad_engine", "int", 2, "live", "controls.pad",
    "Controller: the engine accelerating", "Джойстик: двигатель при разгоне",
    "While you accelerate the controller rumbles with the engine: soft at low revs, harder up the revs, a buzz at the limiter.|Light — about half as strong.",
    "Пока вы разгоняетесь, джойстик вибрирует с мотором: мягко на низких оборотах, сильнее с ростом, «бррр» на отсечке.|Слабо — примерно вдвое слабее.",
    "Off|Light|Strong", "Выкл.|Слабо|Сильно", 0, 2)
