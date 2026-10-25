// A second instance of a program, for the app tests: started with an identifier and arguments, it hands them to the
// instance running under that identifier and exits with 0, or with 1 when none is running.
#include <symbolengine/app/Application.h>

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;

    symbolengine::Application app({.name = "SymbolEngine second instance", .identifier = argv[1]}, argc, argv);
    return app.sendToRunningInstance() ? 0 : 1;
}
