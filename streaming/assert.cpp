#include "assert.h"
#include <iostream>
#include <sstream>

std::atomic_bool streaming::async_callback_error = false;
std::mutex streaming::async_callback_error_mutex;

void maybe_assert(bool expr)
{
    expr;
    /*assert_(expr);*/
}

streaming::exception::exception(HRESULT hr, int line_number, const char* filename)
{
    assert_(false);

    std::ostringstream sts;
    report_error(sts, hr, line_number, filename);
    this->error_str = std::move(sts.str());
}

void streaming::report_error(std::ostream& stream, HRESULT hr, int line_number, const char* filename)
{
    stream << "EXCEPTION THROWN: hresult " << "0x" << std::hex << hr << std::dec
        << " at line " << line_number << ", " << filename << std::endl;
}

void streaming::check_for_errors()
{
    if(async_callback_error)
    {
        async_callback_error_mutex.lock();
        async_callback_error_mutex.unlock();
        /*scoped_lock(::async_callback_error_mutex);*/
    }
}

void streaming::print_error_and_abort(const char* str)
{
    typedef std::lock_guard<std::mutex> scoped_lock;
    scoped_lock lock(async_callback_error_mutex);
    async_callback_error = true;

    std::cout << str;
    system("pause");

    abort();
}