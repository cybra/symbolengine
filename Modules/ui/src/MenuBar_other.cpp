#include <symbolengine/ui/MenuBar.h>

// Windows has no menu bar of the system's that a program shares; a program draws its menus in its own window.
namespace symbolengine::ui
{

struct MenuBar::Impl
{
};

MenuBar::MenuBar(std::vector<MenuItem>, std::vector<Menu>, std::function<void()>) : impl(std::make_unique<Impl>()) {}

MenuBar::~MenuBar() = default;

void MenuBar::changed() {}

}
