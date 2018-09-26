#include "assert.h"
#include <iostream>
#include <sstream>

void maybe_assert(bool expr)
{
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