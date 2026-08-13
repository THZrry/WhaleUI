/* whaleui core: build-variant metadata (step 1: environment bootstrap). */
#include "whaleui.h"

extern "C" const char* whaleui_variant(void)
{
#if defined(WHALEUI_BUILD_FULL)
    return "full";
#elif defined(WHALEUI_BUILD_LITE)
    return "lite";
#elif defined(WHALEUI_BUILD_MINIMAL)
    return "minimal";
#else
    return "unknown";
#endif
}

extern "C" const char* whaleui_version(void)
{
    return WHALEUI_VERSION;
}
