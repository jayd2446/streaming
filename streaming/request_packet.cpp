#include "request_packet.h"

bool request_packet::get_clock(presentation_clock_t& clock) const
{
    if(!this->topology)
        return false;

    clock = this->topology->get_clock();
    return !!clock;
}