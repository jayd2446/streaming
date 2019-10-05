#pragma once

#include "assert.h"
#include "enable_shared_from_this.h"
#include <memory>
#include <stack>
#include <mutex>
#include <utility>
#include <limits>
#include <functional>

#define FREE_CONTROL_BLOCK(ptr) ::operator delete(ptr)

// TODO: rename to object pool

#undef min
#undef max

template<class PooledBuffer>
class buffer_pool : public enable_shared_from_this
{
    friend typename PooledBuffer;
    template<class T, class U>
    friend struct control_block_allocator;
public:
    struct control_block_desc_t
    {
        void* control_block_ptr;
        size_t control_block_len;
        bool in_use;

        control_block_desc_t() : control_block_ptr(nullptr), control_block_len(0), in_use(false) {}
    };

    typedef std::unique_lock<std::recursive_mutex> scoped_lock;
    typedef PooledBuffer pooled_buffer_t;
    typedef std::stack<std::shared_ptr<pooled_buffer_t>> buffer_pool_t;
    typedef std::stack<std::shared_ptr<control_block_desc_t>> control_block_pool_t;
private:
    volatile bool disposed;
    buffer_pool_t container;
    control_block_pool_t control_block_descs;
public:
    buffer_pool();

    // mutex must be locked when using buffer_pool methods
    std::recursive_mutex mutex;

    // the buffer might be uninitialized
    typename pooled_buffer_t::buffer_t acquire_buffer();
    bool is_empty() const {return this->container.empty();}

    // the pool must be manually disposed;
    // it breaks the circular dependency between the pool and its objects
    // and frees the cached control blocks;
    // failure to dispose causes memory leak
    void dispose();
    bool is_disposed() const {return this->disposed;}
};

class buffer_poolable
{
    // classes derived from this must implement initialize and uninitialize
private:
    bool initialized;
protected:
    void initialize() {this->initialized = true;}
    virtual void uninitialize() {assert_(this->initialized); this->initialized = false;}
public:
    buffer_poolable() : initialized(false) {}
    virtual ~buffer_poolable() {}
};

template<class Poolable>
class buffer_pooled final : public Poolable, public enable_shared_from_this
{
    static_assert(std::is_base_of_v<buffer_poolable, Poolable>,
        "template parameter must inherit from poolable");
    static_assert(!std::is_base_of_v<enable_shared_from_this, Poolable>,
        "pooled buffers do not work with enable_shared_from_this");
public:
    typedef Poolable buffer_raw_t;
    typedef std::shared_ptr<Poolable> buffer_t;
    typedef buffer_pool<buffer_pooled> buffer_pool;
private:
    std::shared_ptr<buffer_pool> pool;
    void deleter(buffer_raw_t*);
public:
    explicit buffer_pooled(const std::shared_ptr<buffer_pool>& pool);
    buffer_t create_pooled_buffer();
};

template<class T, class BufferPool>
struct control_block_allocator
{
    typedef BufferPool buffer_pool;
    typedef typename buffer_pool::control_block_desc_t control_block_desc_t;
    typedef T value_type;
    typedef T* pointer;
    typedef std::size_t size_type;

    std::shared_ptr<buffer_pool> pool;
    std::shared_ptr<control_block_desc_t> control_block_desc;
    bool allocated;

    explicit control_block_allocator(const std::shared_ptr<buffer_pool>& pool);
    template<typename U>
    control_block_allocator(const control_block_allocator<U, BufferPool>&);

    /*template<class T, class... Args>
    void construct(T* p, Args&&... args)
    {
        if constexpr (!std::is_base_of_v<buffer_poolable, T>)
            ::new (static_cast<void*>(p)) T(std::forward<Args>(args)...);
    }

    template<class T>
    void destroy(T* p)
    {
        if constexpr (!std::is_base_of_v<buffer_poolable, T>)
            p->~T();
    }*/

    pointer allocate(size_type n);
    // frees the memory of the control_block_t if pool has been disposed
    void deallocate(T* p, size_type n);
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T, class U>
control_block_allocator<T, U>::control_block_allocator(const std::shared_ptr<buffer_pool>& pool) :
    pool(pool)
{
    // buffer pool lock is assumed
    if(this->pool->control_block_descs.empty())
        this->control_block_desc.reset(new control_block_desc_t);
    else
    {
        this->control_block_desc = this->pool->control_block_descs.top();
        this->pool->control_block_descs.pop();
    }
}

template<class T, class U>
template<class V>
control_block_allocator<T, U>::control_block_allocator(const control_block_allocator<V, U>& other) :
    pool(other.pool), control_block_desc(other.control_block_desc)
{
}

template<class T, class U>
typename control_block_allocator<T, U>::pointer control_block_allocator<T, U>::allocate(size_type n)
{
    // buffer pool lock is assumed

    // a check to ensure that the shared ptr doesn't allocate more internal data than the
    // control block
    if(this->control_block_desc->in_use)
        throw HR_EXCEPTION(E_UNEXPECTED);

    // allocate new memory only if the control block isn't allocated already
    if(!this->control_block_desc->control_block_ptr)
    {
        this->control_block_desc->control_block_len = n * sizeof(T);
        this->control_block_desc->control_block_ptr =
            ::operator new(this->control_block_desc->control_block_len);
    }

    if(this->control_block_desc->control_block_len != (n * sizeof(T)))
        throw HR_EXCEPTION(E_UNEXPECTED);

    this->control_block_desc->in_use = true;

    return (pointer)this->control_block_desc->control_block_ptr;
}

template<class T, class U>
void control_block_allocator<T, U>::deallocate(T* p, size_type /*n*/)
{
    // the shared pointer deallocates only the control block because it
    // has a custom deleter aswell

    // the passed allocator is used to allocate the control block,
    // but a new allocator is constructed from the passed args later on

    typename buffer_pool::scoped_lock lock(this->pool->mutex);

    assert_(p == this->control_block_desc->control_block_ptr); p;

    if(this->pool->is_disposed())
        FREE_CONTROL_BLOCK(this->control_block_desc->control_block_ptr);
    else
    {
        this->control_block_desc->in_use = false;
        this->pool->control_block_descs.push(this->control_block_desc);
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T>
buffer_pool<T>::buffer_pool() : disposed(false)
{
}

template<class T>
typename buffer_pool<T>::pooled_buffer_t::buffer_t buffer_pool<T>::acquire_buffer()
{
    if(this->is_empty())
    {
        /*return std::shared_ptr<pooled_buffer_t>(new pooled_buffer_t(
            this->shared_from_this<buffer_pool>()));*/
        std::shared_ptr<pooled_buffer_t> pooled_buffer(new pooled_buffer_t(
            this->shared_from_this<buffer_pool>()));
        typename pooled_buffer_t::buffer_t buffer = pooled_buffer->create_pooled_buffer();
        return buffer;
    }
    else
    {
        typename pooled_buffer_t::buffer_t buffer = 
            this->container.top()->create_pooled_buffer();
        this->container.pop();
        return buffer;
    }
}

template<class T>
void buffer_pool<T>::dispose()
{
    assert_(!this->disposed);

    this->disposed = true;
    this->container = buffer_pool_t();
    while(!this->control_block_descs.empty())
    {
        assert_(!this->control_block_descs.top()->in_use);
        FREE_CONTROL_BLOCK(this->control_block_descs.top()->control_block_ptr);
        this->control_block_descs.pop();
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T>
buffer_pooled<T>::buffer_pooled(const std::shared_ptr<buffer_pool>& pool) : pool(pool)
{
}

template<class T>
typename buffer_pooled<T>::buffer_t buffer_pooled<T>::create_pooled_buffer()
{
    // media_buffer_pooled will stay alive at least as long as the wrapped buffer is alive
    using std::placeholders::_1;
    auto deleter_f = std::bind(&buffer_pooled::deleter,
        this->shared_from_this<buffer_pooled>(), _1);

    // the custom allocator won't work if the shared ptr allocates dynamic memory
    // more than one time;
    // it shouldn't though, because that would be detrimental to performance
    return buffer_t(this, deleter_f, control_block_allocator<buffer_pooled, buffer_pool>(this->pool));
}

template<class T>
void buffer_pooled<T>::deleter(buffer_raw_t* buffer)
{
    assert_(buffer == this); buffer;

    // locking the pool mutex before uninitializing can lock the whole pipeline for
    // unnecessarily long time
    buffer->uninitialize();

    // move the buffer back to sample pool if the pool isn't disposed yet;
    // otherwise, this object will be destroyed after the std bind releases the last reference
    buffer_pool::scoped_lock lock(this->pool->mutex);
    if(!this->pool->is_disposed())
        this->pool->container.push(this->shared_from_this<buffer_pooled>());
}