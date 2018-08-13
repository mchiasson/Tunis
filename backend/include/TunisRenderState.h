#ifndef TUNISRENDERSTATE_H
#define TUNISRENDERSTATE_H

#include <vector>
#include <glm/vec4.hpp>

namespace tunis
{

template<typename Ttype>
class RenderProperty
{
public:
    RenderProperty()
    {
    }

    RenderProperty(const Ttype& value) :
        m_value(value)
    {
    }

    RenderProperty(const RenderProperty& other) :
        m_value(other.m_value)
    {
    }

    inline Ttype& operator=(const Ttype& value)
    {
        if (value != m_value)
        {
            m_value = value;
            m_isDirty = true;
        }
        return m_value;
    }

    inline RenderProperty& operator=(const RenderProperty& other)
    {
        *this = other.m_value;
        m_stack = other.m_stack;
        return *this;
    }

    inline const Ttype& get() const
    {
        return m_value;
    }

    inline operator const Ttype&() const
    {
        return m_value;
    }

    inline bool isDirty() const
    {
        return m_isDirty;
    }

    inline void resetDirty()
    {
        m_isDirty = false;
    }

    inline void forceDirty()
    {
        m_isDirty = true;
    }

    inline void reset()
    {
        if (!m_stack.empty())
        {
            *this = m_stack.front();
            m_stack.clear();
        }
        m_isDirty = true;
    }

    inline void push(const Ttype& value)
    {
        m_stack.push_back(m_value);
        *this = value;
    }

    inline void pop()
    {
        if (!m_stack.empty())
        {
            *this = m_stack.back();
            m_stack.pop_back();
        }
    }

    inline size_t size() const
    {
        return m_stack.size();
    }

private:
    std::vector<Ttype> m_stack;
    Ttype m_value;
    bool m_isDirty = true;
};

class RenderState
{
public:
    RenderState();
    RenderState(const RenderState& other);
    RenderState& operator=(const RenderState& other);

    void reset();
    void apply();

    RenderProperty<glm::ivec4> viewport;

};



}


#endif // TUNISRENDERSTATE_H
