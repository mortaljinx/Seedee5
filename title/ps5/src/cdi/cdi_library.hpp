/*
 * Seedee5 - what the Cover Wheel shows: the games, as the program (cdiwheel.cpp) builds them from the engine's
 * list (engine/cdie.h), each with its cover texture and colours. The design (kit/screen.cpp) reads them and asks
 * for things through the small mailboxes below; the program answers on the next frame. Everything here is made
 * and changed on the render thread only.
 *
 * prelude.h includes this file first, so the program (compiled inside a namespace of its own) and the design
 * (compiled outside it) share one copy.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "gfx/draw_list.hpp"

namespace cdi
{

struct Game
{
    std::string name;   // "Burn:Cycle (Europe)": the file's name, as the list shows it
    std::string title;  // "Burn:Cycle": the name before its first "(...)" tag
    std::string meta;   // "Europe  ·  NAS  ·  2 discs": the tags and where it is
    std::string path;   // the engine's path for it: what a game is recognised by across rescans
    std::string source; // "On the console", "NAS", "USB"
    std::string format; // "CHD", "CUE", "ISO", "7Z"
    char letter = '#';
    int discs = 1;
    bool here = true;   // can start at once (not still on the NAS or packed in a .7z)
    bool favorite = false;
    std::string resume;    // "11 Oct 22:40": when its automatic save was made (Continue), or "" for none
    std::string played;    // "3 h 20 min" in all, or ""
    std::string year, developer, publisher; // from the list of CD-i games (games_info.hpp), when it knows the game
    int engine = -1;       // the game's index in the engine's list (the wheel may show only some games)
    long playSecs = 0;     // time played, seconds
    long lastPlayed = 0;   // when it was last played (Unix time), 0 never
    bool onNas = false;
    std::uint32_t cover = 0; // a renderer texture (top row first), or 0 while there is none
    float aspect = 1.0f;     // the picture's height over its width
    // The open card's extras (only the open game has them; the program fills them in as they arrive):
    std::string about;       // a few sentences from the game's Wikipedia article, or ""
    std::string reviews;     // "AllGame 2/5  ·  Edge 6/10", or ""
    std::string credit;      // where the text is from (Wikipedia, CC BY-SA 4.0), shown under it
    int aboutState = 0;      // 0 still looking, 1 found, 2 nothing found (or no network)
    std::uint32_t backdrop = 0; // an in-game screenshot drawn behind the open card, or 0
    float backdropAspect = 0.75f;
    std::uint32_t titleLogo = 0; // the game's logo drawn in place of its title, or 0
    float titleLogoAspect = 0.25f;
    hui::gfx::Color dark{0.03f, 0.03f, 0.05f, 1.0f};
    hui::gfx::Color mid{0.16f, 0.18f, 0.26f, 1.0f};
    hui::gfx::Color accent{0.55f, 0.75f, 1.0f, 1.0f};
};

// The release (Settings shows it; the PS5 home screen shows param.json's, which make_cdi_title.py sets to the same)
#define SEEDEE5_VERSION "1.22"

struct Library
{
    std::uint32_t logo = 0; // the Seedee5 logo (a renderer texture), shown at the top left of the wheel
    std::vector<Game> games;
    int letters[27] = {}; // the first game of '#', 'A' ... 'Z'; a letter with no games repeats the next one
    unsigned serial = 0;  // goes up whenever the list itself changed
    std::string nas;      // one line about the NAS ("NAS: 2020 game file(s) found"), or ""
    std::string covers;   // one line about the cover downloads, or ""
    std::string keepNote; // after a replaced app folder (a store's update): what came back and where the rest is
    int nasState = 0;     // 0 none, 1 connecting, 2 connected, 3 failed
    std::string roomFor;  // where games go, for the empty screen
    std::string view;     // what the wheel shows: "" every game, else "FAVOURITES", "RECENTLY PLAYED"...
    std::string emptyText; // what to say when that shows nothing
};

inline Library &library()
{
    static Library instance;
    return instance;
}

// The design to the program: start this game (index), or -1
inline int &playRequest()
{
    static int value = -1;
    return value;
}

// The design to the program: start the game asked for with playRequest from its automatic save (Continue)
inline bool &continueRequest()
{
    static bool value = false;
    return value;
}

// The design to the program: these games' favourite marks were toggled (indexes)
inline std::vector<int> &favouriteToggles()
{
    static std::vector<int> value;
    return value;
}

// The design to the program: look for games again (Square)
inline bool &rescanRequest()
{
    static bool value = false;
    return value;
}

// The design to the program: the game in the middle of the wheel, so its neighbours' covers are read first
inline int &centre()
{
    static int value = 0;
    return value;
}

// The design to the program: the game whose card is open (its index), or -1 when none is. The program fetches
// that game's extras (about, reviews, screenshot, logo) and drops them when the card closes.
inline int &openCard()
{
    static int value = -1;
    return value;
}

// The program to the design: move the wheel to this game at once (after a rescan, or at start), or -1
inline int &jumpTo()
{
    static int value = -1;
    return value;
}

// The program to the design: the game it asked for is starting, or could not start. The design closes the
// details panel when the game ends (playing becomes false again).
inline bool &playing()
{
    static bool value = false;
    return value;
}

} // namespace cdi
