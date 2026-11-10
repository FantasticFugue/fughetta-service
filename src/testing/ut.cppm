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

namespace {
    struct stream_initialization {
        stream_initialization() {
            static std::ios_base::Init streams;
        }
    };

    // Base classes initialize in order, before the reporter reads cout.
    struct initialized_reporter : stream_initialization, boost::ut::reporter_junit<> {};
}

export template <>
inline auto boost::ut::cfg<boost::ut::override> = boost::ut::runner<initialized_reporter>{};
