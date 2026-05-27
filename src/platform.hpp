// Copyright Text: 2026 Stepan Zorin <stz.hom@gmail.com>

#pragma once

#if defined _WIN32 || defined __CYGWIN__
    #define STZ_INTERN_PLATFORM_WINDOWS
#elif defined __unix__ || defined __unix || (defined __APPLE__ && defined __MACH__)
    #define STZ_INTERN_PLATFORM_UNIX
    #ifdef __linux__
        #define STZ_INTERN_PLATFORM_LINUX
    #endif
#endif