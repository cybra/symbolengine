#include "Guard.h"

#if defined(_MSC_VER)
    #ifndef WIN32_LEAN_AND_MEAN
        #define WIN32_LEAN_AND_MEAN
    #endif
    #ifndef NOMINMAX
        #define NOMINMAX
    #endif
    #include <windows.h>

    #include <malloc.h>
#endif

namespace symbolengine
{

#if defined(_MSC_VER)

namespace
{

// Every structured exception but a C++ exception's, which goes on to the caller's catch.
int filter(unsigned long code, std::uint32_t& fault)
{
    constexpr unsigned long cppException = 0xE06D7363;

    if (code == cppException)
        return EXCEPTION_CONTINUE_SEARCH;

    fault = static_cast<std::uint32_t>(code);
    return EXCEPTION_EXECUTE_HANDLER;
}

}

bool guarded(void (*body)(void*), void* context, std::uint32_t& fault)
{
    __try
    {
        body(context);
    }
    __except (filter(GetExceptionCode(), fault))
    {
        // The stack's guard page, used up by an overflow, is put back so a later one is caught too.
        if (fault == static_cast<std::uint32_t>(EXCEPTION_STACK_OVERFLOW))
            _resetstkoflw();

        return false;
    }

    return true;
}

#else

bool guarded(void (*body)(void*), void* context, std::uint32_t&)
{
    body(context);
    return true;
}

#endif

}
