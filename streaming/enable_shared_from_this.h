#pragma once

#include <memory>

class enable_shared_from_this : public std::enable_shared_from_this<enable_shared_from_this>
{
public:
    virtual ~enable_shared_from_this() {}

    template<class T>
    std::shared_ptr<T> shared_from_this()
    {
        return std::dynamic_pointer_cast<T>(
            std::enable_shared_from_this<enable_shared_from_this>::shared_from_this());
    }

    template<class T>
    std::shared_ptr<T> shared_from_this() const
    {
        return std::dynamic_pointer_cast<T>(
            std::enable_shared_from_this<enable_shared_from_this>::shared_from_this());
    }
};