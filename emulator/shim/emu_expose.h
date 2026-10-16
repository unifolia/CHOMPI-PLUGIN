// Reaching private firmware members for session recall, without editing the
// firmware. Explicit instantiation may name private members, and the friend
// it defines hands out the member pointer:
//   EMU_EXPOSE(Tag, Class, Type, member);  ...  object.*Get(Tag{})
// Use inside namespace emu { namespace { ... } }: the friend declared by the
// tag and the one Expose defines are the same function only when both live
// in that one unnamed namespace.
#pragma once

namespace emu
{
namespace
{
template <typename Tag, typename Tag::type Member>
struct Expose
{
    friend typename Tag::type Get(Tag) { return Member; }
};
} // namespace
} // namespace emu

#define EMU_EXPOSE(Name, Class, Type, Member)                       \
    struct Name                                                      \
    {                                                                \
        using type = Type Class::*;                                  \
        friend type Get(Name);                                       \
    };                                                               \
    template struct Expose<Name, &Class::Member>
