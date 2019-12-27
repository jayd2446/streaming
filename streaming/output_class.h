#pragma once

#include "media_sample.h"
#include <atlbase.h>
#include <mfapi.h>
#include <memory>

class output_class
{
private:
public:
    virtual ~output_class() = default;
    virtual void write_sample(bool video, const CComPtr<IMFSample>&) = 0;
};

using output_class_t = std::shared_ptr<output_class>;