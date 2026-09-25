#include <cstdint>

namespace WTF {

struct StringImplShape {
    uint32_t m_refCount;
    unsigned m_length;
    mutable unsigned m_hashAndFlags;
};

struct StringImpl {
    class StaticStringImpl : StringImplShape {
    public:
        StaticStringImpl() : StringImplShape{ 3, 0, 16 } {}
    };
    static StaticStringImpl s_emptyAtomString;
};

StringImpl::StaticStringImpl StringImpl::s_emptyAtomString;

struct StaticString {
    StringImpl::StaticStringImpl* m_pointer;
};

struct StaticAtomString {
    StringImpl::StaticStringImpl* m_pointer;
};

extern const StaticString emptyStringData{ &StringImpl::s_emptyAtomString };
extern const StaticString nullStringData{ nullptr };
extern const StaticAtomString nullAtomData{ nullptr };

} // namespace WTF

// MSVC STL internal helper — clang-cl emits calls to it for std::find on uint8_t
// but the UWP CRT (ucrtbase.dll / msvcprt_*.dll) doesn't export it.
extern "C" const void* __stdcall __std_find_last_trivial_1(
    const void* _First, const void* _Last, uint8_t _Val) noexcept
{
    const uint8_t* first = static_cast<const uint8_t*>(_First);
    const uint8_t* last  = static_cast<const uint8_t*>(_Last);
    if (first >= last) return _Last;
    const uint8_t* p = last;
    while (p > first) {
        --p;
        if (*p == _Val) return p;
    }
    return _Last;
}
