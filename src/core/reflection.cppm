module;
#include <rfl.hpp>
#include <rfl/json.hpp>
#include <rfl/xml.hpp>

export module reflection;

export namespace rfl {
    using rfl::DefaultIfMissing;
    using rfl::Description;
    using rfl::from_generic;
    using rfl::Generic;
    using rfl::get;
    using rfl::Literal;
    using rfl::NoExtraFields;
    using rfl::Object;
    using rfl::Reflector;
    using rfl::Rename;
    using rfl::TaggedUnion;
    using rfl::to_generic;

    namespace internal {
        using rfl::internal::StringLiteral;
    } // namespace internal

    namespace json {
        using rfl::json::read;
        using rfl::json::to_schema;
        using rfl::json::write;
    } // namespace json

    namespace xml {
        using rfl::xml::read;
    } // namespace xml
} // namespace rfl
