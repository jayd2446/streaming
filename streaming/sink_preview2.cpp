#include "sink_preview2.h"
#include "assert.h"
#include "control_pipeline.h"
#include "control_video.h"
#include "gui_previewwnd.h"
#include <Windows.h>
#define _USE_MATH_DEFINES
#include <math.h>

#define CHECK_HR(hr_) {if(FAILED(hr_)) {goto done;}}

#undef max
#undef min

sink_preview2::sink_preview2(const media_session_t& session) : 
    media_sink(session),
    texture_requests(MAX_TEXTURE_REQUESTS),
    render(true)
{
}

void sink_preview2::initialize(const control_pipeline_t& ctrl_pipeline, HWND hwnd)
{
    this->ctrl_pipeline = ctrl_pipeline;
    this->hwnd = hwnd;
}

void sink_preview2::draw_sample(const media_component_args* args_)
{
    if(!this->render)
        return;

    scoped_lock lock(this->mutex);

    // videomixer outputs h264 encoder args;
    // sink preview just previews the last frame that is send to encoder;
    // it is assumed that there is at least one frame if the args isn't empty
    const media_component_h264_encoder_args* args =
        static_cast<const media_component_h264_encoder_args*>(args_);

    if(args)
    {
        // find the first available bitmap
        for(auto&& item : args->sample->frames)
        {
            if(item.buffer && item.buffer->bitmap)
            {
                std::atomic_store(&this->last_buffer, item.buffer);
                break;
            }
        }
    }

    if(this->hwnd && this->texture_requests > 0)
    {
        this->texture_requests--;
        PostMessage(this->hwnd, GUI_PREVIEWWND_MESSAGE, 0, 0);
    }
}

media_stream_t sink_preview2::create_stream()
{
    return stream_preview2_t(new stream_preview2(this->shared_from_this<sink_preview2>()));
}

void sink_preview2::clear_preview_wnd()
{
    scoped_lock lock(this->mutex);
    this->hwnd = NULL;
}


/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////
/////////////////////////////////////////////////////////////////


stream_preview2::stream_preview2(const sink_preview2_t& sink) : sink(sink)
{
}

media_stream::result_t stream_preview2::request_sample(const request_packet& rp, const media_stream*)
{
    return this->sink->session->request_sample(this, rp) ? OK : FATAL_ERROR;
}

media_stream::result_t stream_preview2::process_sample(
    const media_component_args* args, const request_packet& rp, const media_stream*)
{
    this->sink->draw_sample(args);
    return this->sink->session->give_sample(this, args, rp) ? OK : FATAL_ERROR;
}
