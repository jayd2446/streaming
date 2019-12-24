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
    virtual void write_sample(bool video, frame_unit fps_num, frame_unit fps_den,
        const CComPtr<IMFSample>&) = 0;
};

using output_class_t = std::shared_ptr<output_class>;