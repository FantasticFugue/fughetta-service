module;
#if defined(_WIN32)
#include <stdlib.h>
#elif __has_include(<unistd.h>) && __has_include(<sys/wait.h>)
#include <sys/wait.h>
#include <unistd.h>
#endif
export module boost.ut;
export import std;
#define BOOST_UT_CXX_MODULES 1
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Winclude-angled-in-module-purview"
#endif
#include <boost/ut.hpp>
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
