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

#undef min
#undef max

template<class BufferPool, typename T = int>
struct control_block_allocator
{
    typedef BufferPool buffer_pool;
    typedef typename buffer_pool::state_t state_t;

    typedef T value_type;
    typedef T* pointer;
    typedef const T* const_pointer;
    typedef T& reference;
    typedef const T& const_reference;
    typedef std::size_t size_type;
    typedef std::ptrdiff_t difference_type;
    using propagate_on_container_move_assignment = std::true_type;
    template< class U > struct rebind { typedef control_block_allocator<BufferPool, U> other; };
    /*using is_always_equal = std::true_type;*/

    // the state is tied to the pooled object
    std::shared_ptr<state_t> state;

    bool allocated;

    explicit control_block_allocator(const std::shared_ptr<state_t>& state);
    template<typename U>
    control_block_allocator(const control_block_allocator<BufferPool, U>&);

    pointer address(reference x) const {return &x;}
    const_pointer address(const_reference x) const {return &x;}
    size_type max_size() const {return std::numeric_limits<size_type>::max() / sizeof(value_type);}
    void construct(pointer p, const_reference val) {new((void*)p) T(val);}
    template<class U, class... Args>
    void construct(U* p, Args&&... args) {::new((void*)p) U(std::forward<Args>(args)...);}
    void destroy(pointer p) {((T*)p)->~T();}
    template<class U>
    void destroy(U* p) {p->~U();}

    pointer allocate(size_type n, const void* hint = 0);
    // frees the memory of the control_block_t if pool has been disposed
    void deallocate(T* p, std::size_t n);
};

template<class PooledBuffer>
class buffer_pool : public enable_shared_from_this
{
    friend typename PooledBuffer;
public:
    struct state_t
    {
        std::shared_ptr<buffer_pool> pool;
        bool allocated;
        void* control_block_ptr;
        size_t control_block_len;

        state_t(const std::shared_ptr<buffer_pool>& pool) : pool(pool),
            control_block_ptr(NULL), control_block_len(0), allocated(false) {}
    };

    typedef std::lock_guard<std::recursive_mutex> scoped_lock;
    typedef PooledBuffer pooled_buffer_t;
    typedef std::stack<std::shared_ptr<pooled_buffer_t>> buffer_pool_t;
private:
    volatile bool disposed;
    buffer_pool_t container;
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

class buffer_poolable : public enable_shared_from_this
{
    // classes derived from this must implement initialize and uninitialize
private:
    bool initialized;
protected:
    virtual void initialize() {this->initialized = true;}
    virtual void uninitialize() {assert_(this->initialized); this->initialized = false;}
public:
    buffer_poolable() : initialized(false) {}
    virtual ~buffer_poolable() {}
};

template<typename Poolable>
class buffer_pooled : public Poolable
{
    friend class buffer_pool<buffer_pooled>;
    static_assert(std::is_base_of<buffer_poolable, Poolable>::value,
        "template parameter must inherit from poolable");
public:
    typedef Poolable buffer_raw_t;
    typedef std::shared_ptr<Poolable> buffer_t;
    typedef buffer_pool<buffer_pooled> buffer_pool;
    typedef typename buffer_pool::state_t state_t;
private:
    std::shared_ptr<buffer_pool> pool;
    std::shared_ptr<state_t> state;

    void deleter(buffer_raw_t*);
public:
    explicit buffer_pooled(const std::shared_ptr<buffer_pool>& pool);
    buffer_t create_pooled_buffer();
};


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<class T, typename U>
control_block_allocator<T, U>::control_block_allocator(const std::shared_ptr<state_t>& state) : 
    state(state), allocated(false)
{
}

template<class T, typename U>
template<typename V>
control_block_allocator<T, U>::control_block_allocator(const control_block_allocator<T, V>& other) :
    state(other.state), allocated(false)
{
}

template<class T, typename U>
typename control_block_allocator<T, U>::pointer control_block_allocator<T, U>::allocate(size_type n,
    const void* /*hint*/)
{
    // a check to ensure that the shared ptr doesn't allocate more internal data than the
    // control block
    if(this->allocated)
        throw HR_EXCEPTION(E_UNEXPECTED);

    // allocate new memory only if the control block is null
    if(this->state->control_block_ptr == NULL)
    {
        this->state->control_block_len = n * sizeof(U);
        this->state->control_block_ptr = ::operator new(this->state->control_block_len);
        this->allocated = true;
    }
    else if(this->state->control_block_len != (n * sizeof(U)))
        throw HR_EXCEPTION(E_UNEXPECTED);

    this->state->allocated = true;
    return (pointer)this->state->control_block_ptr;
}

template<class T, typename U>
void control_block_allocator<T, U>::deallocate(U* p, std::size_t /*n*/)
{
    // the passed allocator is used to allocate the control block,
    // but a new allocator is constructed from the passed args later on

    typename buffer_pool::scoped_lock lock(this->state->pool->mutex);
    if(this->state->pool->is_disposed())
    {
        assert_(this->state->allocated);
        assert_(p == this->state->control_block_ptr); p;

        FREE_CONTROL_BLOCK(this->state->control_block_ptr);
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
    // free the control block allocations of pooled objects
    while(!this->container.empty())
    {
        assert_(this->container.top()->state->allocated);
        FREE_CONTROL_BLOCK(this->container.top()->state->control_block_ptr);

        this->container.pop();
    }
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


template<typename T>
buffer_pooled<T>::buffer_pooled(const std::shared_ptr<buffer_pool>& pool) :
    pool(pool),
    state(new state_t(pool))
{
}

template<typename T>
typename buffer_pooled<T>::buffer_t buffer_pooled<T>::create_pooled_buffer()
{
    // media_buffer_pooled will stay alive at least as long as the wrapped buffer is alive
    using std::placeholders::_1;
    auto deleter_f = std::bind(&buffer_pooled::deleter,
        this->shared_from_this<buffer_pooled>(), _1);

    // the custom allocator won't work if the shared ptr allocates dynamic memory
    // more than one time;
    // it shouldn't though, because that would be detrimental to performance
    return buffer_t(this, deleter_f, control_block_allocator<buffer_pool>(this->state));
}

template<typename T>
void buffer_pooled<T>::deleter(buffer_raw_t* buffer)
{
    assert_(buffer == this); buffer;

    // move the buffer back to sample pool if the pool isn't disposed yet;
    // otherwise, this object will be destroyed after the std bind releases the last reference
    buffer_pool::scoped_lock lock(this->pool->mutex);
    buffer->uninitialize();
    if(!this->pool->is_disposed())
    {
        this->pool->container.push(this->shared_from_this<buffer_pooled>());
    }
}