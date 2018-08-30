#pragma once

#include <cassert>
#include <Windows.h>

#ifdef _DEBUG
#define assert_(_Expression) (void)( (!!(_Expression)) || (DebugBreak(), 0) )
#else
#define assert_(_Expression) ((void)0)
#endif

void maybe_assert(bool);