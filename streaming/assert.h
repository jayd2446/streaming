#pragma once

#include <exception>
#include <string>
#include <cassert>
#include <mutex>
#include <atomic>
#include <Windows.h>

#ifdef _DEBUG
#define assert_(_Expression) (void)( (!!(_Expression)) || (DebugBreak(), 0) )
#else
#define assert_(_Expression) ((void)0)
#endif

void maybe_assert(bool);

namespace streaming {

extern std::atomic_bool async_callback_error;
extern std::mutex async_callback_error_mutex;

class exception : public std::exception
{
private:
    std::string error_str;
public:
    exception(HRESULT, int line_number, const char* filename);
    const char* what() const override {return this->error_str.c_str();}
};

void report_error(std::ostream& stream, HRESULT, int line_number, const char* filename);
void check_for_errors();
void print_error_and_abort(const char*);

}

#define HR_EXCEPTION(hr_) streaming::exception(hr_, __LINE__, BASE_FILE)
#define PRINT_ERROR(hr_) streaming::report_error(std::cout, hr_, __LINE__, BASE_FILE)