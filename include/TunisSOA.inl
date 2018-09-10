#include <TunisSOA.h>

namespace tunis
{

template <typename... Elements>
inline RefCountedSOA<Elements...>::RefCountedSOA()
{
    if (_available.size() > 0)
    {
        _id = _available.back();
        _available.pop_back();
    }
    else
    {
        _id = static_cast<uint32_t>(_soa.size());
        _soa.resize(_soa.size()+1);
    }

    _soa.get<_refCount>(_id) = 1;
}

template <typename... Elements>
inline RefCountedSOA<Elements...>::RefCountedSOA(const RefCountedSOA &other) :
    _id(other._id)
{
    ++_soa.get<_refCount>(_id);
}

template <typename... Elements>
inline RefCountedSOA<Elements...>::RefCountedSOA(RefCountedSOA &&other) :
    _id(std::move(other._id))
{
    ++_soa.get<_refCount>(_id);
}

template <typename... Elements>
inline RefCountedSOA<Elements...>::~RefCountedSOA()
{
    if (--_soa.get<_refCount>(_id) == 0)
    {
        _available.push_back(_id);
    }
}

template <typename... Elements>
inline RefCountedSOA<Elements...> &RefCountedSOA<Elements...>::operator=(const RefCountedSOA &other)
{
    if (this != &other)
    {
        if (--_soa.get<_refCount>(_id) == 0)
        {
            _available.push_back(_id);
        }

        _id = other._id;

        ++_soa.get<_refCount>(_id);
    }

    return *this;
}

template <typename... Elements>
inline RefCountedSOA<Elements...> &RefCountedSOA<Elements...>::operator=(RefCountedSOA &&other)
{
    if (this != &other)
    {
        if (--_soa.get<_refCount>(_id) == 0)
        {
            _available.push_back(_id);
        }

        _id = std::move(other._id);

        ++_soa.get<_refCount>(_id);
    }

    return *this;
}

template <typename... Elements>
template <typename T>
inline T RefCountedSOA<Elements...>::clone()
{
    T instance;

    _soa.copy(_id, instance._id);

    // since SoA::copy copies every fields cluding the refCount fields of the
    // current instance, we need to manually reset our new instance's refcount
    // field since there are no way to tell SoA::copy to exclude fields.
    _soa.get<_refCount>(instance._id) = 1;

    return std::move(instance);
}

template <typename... Elements>
template <size_t ArrayIndex>
inline typename SoA<Elements...>::NthTypeOf<ArrayIndex>& RefCountedSOA<Elements...>::get() const
{
    return _soa.get<ArrayIndex>(_id);
}


template <typename... Elements>
inline void RefCountedSOA<Elements...>::reserve(size_t size)
{
    _soa.reserve(size);
}

template <typename... Elements>
SoA<Elements..., typename RefCountedSOA<Elements...>::RefCount> RefCountedSOA<Elements...>::_soa;

template <typename... Elements>
std::vector<uint32_t> RefCountedSOA<Elements...>::_available;


}


