// menu_lang.h - the settings menu in the game's language (menu_lang.cpp).
#pragma once
#include <string>

// True when the game is Russian (or RECOMP_MENU_LANG=ru): the menu then shows its Russian strings.
bool mods_menu_russian();
// The text to show for an English menu string: its Russian when the game is Russian and one is known, else as is.
std::string mods_menu_tr(const std::string &en);
// A translation from a manifest (label_ru, help_ru) and, choice by choice, choices_ru.
void mods_menu_tr_add(const std::string &en, const std::string &ru);
void mods_menu_tr_add_choices(const std::string &en, const std::string &ru);
