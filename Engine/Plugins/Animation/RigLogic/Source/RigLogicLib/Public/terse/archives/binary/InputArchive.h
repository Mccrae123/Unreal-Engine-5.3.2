#pragma once

#include "terse/Archive.h"
#include "terse/archives/binary/Traits.h"
#include "terse/utils/ArchiveOffset.h"
#include "terse/utils/Endianness.h"

#ifdef _MSC_VER
    #pragma warning(push)
    #pragma warning(disable : 4365 4987)
#endif
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>
#ifdef _MSC_VER
    #pragma warning(pop)
#endif

namespace pma {

class MemoryResource;

}  // namespace pma

namespace terse {

namespace impl {

template<typename T>
struct ValueFactory {
    static constexpr bool NeedsAllocator = traits::needs_allocator<T>::value;
    static constexpr bool NeedsMemoryResource = std::is_constructible<T, pma::MemoryResource*>::value;
    static constexpr bool IsPair = traits::is_pair<T>::value;
    static constexpr bool IsTuple = traits::is_tuple<T>::value;
    static constexpr bool IsPrimitive = (!NeedsAllocator && !NeedsMemoryResource & !IsPair & !IsTuple);

    template<class ParentAllocator, bool IsPrimitive = IsPrimitive>
    static typename std::enable_if<IsPrimitive, T>::type create(const ParentAllocator&  /*unused*/) {
        return T{};
    }

    template<class ParentAllocator, bool IsPrimitive = IsPrimitive>
    static typename std::enable_if<!IsPrimitive && NeedsMemoryResource, T>::type create(const ParentAllocator& alloc) {
        return T{alloc.getMemoryResource()};
    }

    template<class ParentAllocator, bool IsPrimitive = IsPrimitive>
    static typename std::enable_if<!IsPrimitive && !NeedsMemoryResource && NeedsAllocator, T>::type create(
        const ParentAllocator& alloc) {
        using TAllocator = typename std::allocator_traits<ParentAllocator>::template rebind_alloc<typename T::value_type>;
        return T{TAllocator{alloc}};
    }

    template<class ParentAllocator, bool IsPrimitive = IsPrimitive>
    static typename std::enable_if<!IsPrimitive && !NeedsMemoryResource && !NeedsAllocator && IsPair, T>::type create(
        const ParentAllocator& alloc) {
        using K = typename T::first_type;
        using V = typename T::second_type;
        return T{ValueFactory<K>::create(alloc), ValueFactory<V>::create(alloc)};
    }

    template<class ParentAllocator, bool IsPrimitive = IsPrimitive>
    static typename std::enable_if<!IsPrimitive && !NeedsMemoryResource && !NeedsAllocator && !IsPair && IsTuple, T>::type create(
        const ParentAllocator& alloc) {
        using K = typename std::tuple_element<0, T>::type;
        using V = typename std::tuple_element<0, T>::type;
        return T{ValueFactory<K>::create(alloc), ValueFactory<V>::create(alloc)};
    }

};

}  // namespace impl

template<class TExtender, class TStream, typename TSize, typename TOffset>
class ExtendableBinaryInputArchive : public Archive<TExtender> {
    public:
        // Given the possibility of both 32 and 64bit platforms, use a fixed width type during serialization
        using SizeType = TSize;
        using OffsetType = TOffset;

    private:
        using BaseArchive = Archive<TExtender>;

    public:
        ExtendableBinaryInputArchive(TExtender* extender, TStream* stream_) : BaseArchive{extender}, stream{stream_} {
        }

    protected:
        template<typename T>
        void reconstruct(T& value) {
            using UIntType = typename traits::uint_of_size<sizeof(T)>::type;
            static_assert(sizeof(T) == sizeof(UIntType), "No matching unsigned integral type found for the given type.");
            // Using memcpy is the only well-defined way of reconstructing arbitrary types from raw bytes.
            // The seemingly unnecessary copies and memcpy calls are all optimized away,
            // compiler knows what's up.
            UIntType hostOrder;
            std::memcpy(std::addressof(hostOrder), std::addressof(value), sizeof(T));
            hostOrder = ntoh(hostOrder);
            std::memcpy(std::addressof(value), std::addressof(hostOrder), sizeof(T));
        }

        void process(ArchiveOffset<OffsetType>& dest) {
            // Store the position of the offset itself, so it can be seeked to when writing the stream
            dest.position = stream->tell();
            // Load the offset value itself (this points forward within the stream to the position of
            // the data with which the offset is associated)
            process(dest.value);
            // Sanity check for making sure there is an associated proxy with the offset
            assert(dest.proxy != nullptr);
        }

        void process(typename ArchiveOffset<OffsetType>::Proxy& dest) {
            // Rely on the offset value stored in the associated `ArchiveOffset` and seek to it
            stream->seek(dest.target->value);
        }

        template<typename T>
        typename std::enable_if<traits::has_load<T>::value && traits::has_serialize<T>::value, void>::type process(T& dest) {
            dest.load(*static_cast<TExtender*>(this));
        }

        template<typename T>
        typename std::enable_if<traits::has_load<T>::value && !traits::has_serialize<T>::value, void>::type process(T& dest) {
            dest.load(*static_cast<TExtender*>(this));
        }

        template<typename T>
        typename std::enable_if<!traits::has_load<T>::value && traits::has_serialize<T>::value, void>::type process(T& dest) {
            dest.serialize(*static_cast<TExtender*>(this));
        }

        template<typename T>
        typename std::enable_if<!traits::has_load<T>::value && !traits::has_serialize<T>::value, void>::type process(T& dest) {
            // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
            stream->read(reinterpret_cast<char*>(&dest), sizeof(T));
            reconstruct(dest);
        }

        template<typename T, std::size_t N>
        void process(std::array<T, N>& dest) {
            for (auto& element : dest) {
                BaseArchive::dispatch(element);
            }
        }

        template<typename T, typename ... Args>
        void process(std::vector<T, Args...>& dest) {
            const auto size = processSize();
            processElements(dest, size);
        }

        template<typename T, typename ... Args>
        void process(std::basic_string<T, Args...>& dest) {
            const auto size = processSize();
            processElements(dest, size);
        }

        template<typename K, typename V>
        void process(std::pair<K, V>& dest) {
            BaseArchive::dispatch(dest.first);
            BaseArchive::dispatch(dest.second);
        }

        template<typename K, typename V>
        void process(std::tuple<K, V>& dest) {
            BaseArchive::dispatch(std::get<0>(dest));
            BaseArchive::dispatch(std::get<1>(dest));
        }

        std::size_t processSize() {
            SizeType size{};
            process(size);
            return static_cast<std::size_t>(size);
        }

        template<class TContainer>
        typename std::enable_if<!traits::is_batchable<TContainer>::value>::type
        processElements(TContainer& dest, std::size_t size) {
            using ValueType = typename TContainer::value_type;
            dest.reserve(size);
            for (std::size_t i = 0ul; i < size; ++i) {
                dest.push_back(impl::ValueFactory<ValueType>::create(dest.get_allocator()));
                BaseArchive::dispatch(dest.back());
            }
        }

        template<class TContainer>
        typename std::enable_if<traits::is_batchable<TContainer>::value && traits::has_wide_elements<TContainer>::value>::type
        processElements(TContainer& dest, std::size_t size) {
            using ValueType = typename TContainer::value_type;
            if (size != 0ul) {
                dest.resize(size);
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                stream->read(reinterpret_cast<char*>(std::addressof(dest[0])), size * sizeof(ValueType));
                for (std::size_t i = 0ul; i < size; ++i) {
                    reconstruct(dest[i]);
                }
            }
        }

        template<class TContainer>
        typename std::enable_if<traits::is_batchable<TContainer>::value && !traits::has_wide_elements<TContainer>::value>::type
        processElements(TContainer& dest, std::size_t size) {
            using ValueType = typename TContainer::value_type;
            if (size != 0ul) {
                dest.resize(size);
                // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
                stream->read(reinterpret_cast<char*>(std::addressof(dest[0])), size * sizeof(ValueType));
            }
        }

    private:
        TStream* stream;
};

template<class TStream, typename TSize = std::uint32_t, typename TOffset = TSize>
class BinaryInputArchive : public ExtendableBinaryInputArchive<BinaryInputArchive<TStream>, TStream, TSize, TOffset> {
    private:
        using BaseArchive = ExtendableBinaryInputArchive<BinaryInputArchive, TStream, TSize, TOffset>;
        friend Archive<BinaryInputArchive>;

    public:
        explicit BinaryInputArchive(TStream* stream_) : BaseArchive{this, stream_} {
        }

    private:
        template<typename T>
        void process(T&& dest) {
            BaseArchive::process(std::forward<T>(dest));
        }

};

}  // namespace terse
