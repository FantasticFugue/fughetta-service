export module fields;
import std;

namespace meta::detail {
    struct any_member {
        template <class T> operator T() const;
    };

    template <class T, std::size_t... I>
    consteval bool initializes(std::index_sequence<I...>) {
        return requires { T{(static_cast<void>(I), any_member{})...}; };
    }

    template <class T, std::size_t First, std::size_t Last> consteval auto arity() -> std::size_t {
        if constexpr (First == Last)
            return First;
        else {
            constexpr auto middle = First + (Last - First + 1) / 2;
            if constexpr (initializes<T>(std::make_index_sequence<middle>{}))
                return arity<T, middle, Last>();
            else
                return arity<T, First, middle - 1>();
        }
    }

    // This object supplies symbolic member addresses to Clang at compile time.
    template <class T> inline T field_sample;
    template <class Pointer> struct member_address { Pointer pointer; };

    template <auto Address> consteval auto spelling() -> std::string_view {
        constexpr std::string_view signature = __PRETTY_FUNCTION__;
        constexpr auto end = signature.rfind('}');
        constexpr auto begin = signature.rfind('.', end);
        static_assert(begin != std::string_view::npos && end != std::string_view::npos);
        return signature.substr(begin + 1, end - begin - 1);
    }
}

export namespace meta {
    template <class T> constexpr auto members(T& object) {
        using type = std::remove_cv_t<T>;
        static_assert(std::is_aggregate_v<type> && !std::is_array_v<type>, "wire records must be aggregates");
        constexpr auto size = detail::arity<type, 0, 48>();
        static_assert(!detail::initializes<type>(std::make_index_sequence<49>{}), "wire record has too many members");
        if constexpr (size == 0)
            return std::tuple{};
        else if constexpr (size == 1) {
            auto& [m0] = object;
            return std::tie(m0);
        }
        else if constexpr (size == 2) {
            auto& [m0, m1] = object;
            return std::tie(m0, m1);
        }
        else if constexpr (size == 3) {
            auto& [m0, m1, m2] = object;
            return std::tie(m0, m1, m2);
        }
        else if constexpr (size == 4) {
            auto& [m0, m1, m2, m3] = object;
            return std::tie(m0, m1, m2, m3);
        }
        else if constexpr (size == 5) {
            auto& [m0, m1, m2, m3, m4] = object;
            return std::tie(m0, m1, m2, m3, m4);
        }
        else if constexpr (size == 6) {
            auto& [m0, m1, m2, m3, m4, m5] = object;
            return std::tie(m0, m1, m2, m3, m4, m5);
        }
        else if constexpr (size == 7) {
            auto& [m0, m1, m2, m3, m4, m5, m6] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6);
        }
        else if constexpr (size == 8) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7);
        }
        else if constexpr (size == 9) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8);
        }
        else if constexpr (size == 10) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9);
        }
        else if constexpr (size == 11) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10);
        }
        else if constexpr (size == 12) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11);
        }
        else if constexpr (size == 13) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12);
        }
        else if constexpr (size == 14) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13);
        }
        else if constexpr (size == 15) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14);
        }
        else if constexpr (size == 16) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15);
        }
        else if constexpr (size == 17) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16);
        }
        else if constexpr (size == 18) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17);
        }
        else if constexpr (size == 19) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18);
        }
        else if constexpr (size == 20) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19);
        }
        else if constexpr (size == 21) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20);
        }
        else if constexpr (size == 22) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21);
        }
        else if constexpr (size == 23) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22);
        }
        else if constexpr (size == 24) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23);
        }
        else if constexpr (size == 25) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24);
        }
        else if constexpr (size == 26) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25);
        }
        else if constexpr (size == 27) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26);
        }
        else if constexpr (size == 28) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27);
        }
        else if constexpr (size == 29) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28);
        }
        else if constexpr (size == 30) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29);
        }
        else if constexpr (size == 31) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30);
        }
        else if constexpr (size == 32) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31);
        }
        else if constexpr (size == 33) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32);
        }
        else if constexpr (size == 34) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33);
        }
        else if constexpr (size == 35) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34);
        }
        else if constexpr (size == 36) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35);
        }
        else if constexpr (size == 37) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36);
        }
        else if constexpr (size == 38) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37);
        }
        else if constexpr (size == 39) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38);
        }
        else if constexpr (size == 40) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39);
        }
        else if constexpr (size == 41) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40);
        }
        else if constexpr (size == 42) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41);
        }
        else if constexpr (size == 43) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42);
        }
        else if constexpr (size == 44) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43);
        }
        else if constexpr (size == 45) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44);
        }
        else if constexpr (size == 46) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44, m45] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44, m45);
        }
        else if constexpr (size == 47) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44, m45, m46] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44, m45, m46);
        }
        else if constexpr (size == 48) {
            auto& [m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44, m45, m46, m47] = object;
            return std::tie(m0, m1, m2, m3, m4, m5, m6, m7, m8, m9, m10, m11, m12, m13,
                m14, m15, m16, m17, m18, m19, m20, m21, m22, m23, m24, m25, m26, m27,
                m28, m29, m30, m31, m32, m33, m34, m35, m36, m37, m38, m39, m40, m41,
                m42, m43, m44, m45, m46, m47);
        }
    }

    template <class T, std::size_t I> consteval auto member_name() -> std::string_view {
        using detail::member_address;
        return detail::spelling<member_address{std::addressof(std::get<I>(members(detail::field_sample<T>)))}>();
    }

    template <class T, class F> constexpr void for_each_member(T& object, F&& visit) {
        auto fields = members(object);
        [&]<std::size_t... I>(std::index_sequence<I...>) {
            (visit.template operator()<I>(std::get<I>(fields)), ...);
        }(std::make_index_sequence<std::tuple_size_v<decltype(fields)>>{});
    }
}
