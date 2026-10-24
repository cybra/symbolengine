#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace symbolengine::ui
{

/** A key that chooses a menu item from anywhere in the program: a letter, a digit or a punctuation key with Command,
    and Shift or Option as well. */
struct Shortcut
{
    char key = 0;   // as typed without Shift, such as 's' or ','
    bool shift = false;
    bool option = false;
};

/** An item in a menu: a choice, a submenu when it has items of its own, or a line between groups when it has no title. */
struct MenuItem
{
    std::string title{};
    std::optional<Shortcut> shortcut{};
    bool enabled = true;
    bool ticked = false;
    std::function<void()> chosen{};
    std::vector<MenuItem> submenu{};
};

/** A menu in the menu bar: its title, and its items, which it asks for as it opens and whenever it is told they have
    changed. */
struct Menu
{
    std::string title{};
    std::function<std::vector<MenuItem>()> items{};
};

/** The program's menus in the Mac's menu bar. The app menu, named after the program, holds the items given first, then
    Services, Hide, Hide Others, Show All, and Quit, which calls quit; the menus given follow it. On Windows a program
    draws its menus in its own window, and this shows nothing. One at a time, made and used on the message thread. */
class MenuBar
{
public:
    MenuBar(std::vector<MenuItem> appItems, std::vector<Menu> menus, std::function<void()> quit);
    ~MenuBar();

    MenuBar(const MenuBar&) = delete;
    MenuBar& operator=(const MenuBar&) = delete;

    /** Asks every menu for its items again, as after what they show has changed. While a menu is open, it waits until
        the menus close. */
    void changed();

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
