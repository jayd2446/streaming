#include "async_callback.h"

std::atomic_bool async_callback_error = false;
std::mutex async_callback_error_mutex;