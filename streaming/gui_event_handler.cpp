#include "gui_event_handler.h"
#include <algorithm>

gui_event_provider::~gui_event_provider()
{
    assert_(this->event_handlers.empty());
}

void gui_event_provider::register_event_handler(gui_event_handler& e)
{
    if(std::find(this->event_handlers.begin(),
        this->event_handlers.end(), &e) != this->event_handlers.end())
        throw HR_EXCEPTION(E_UNEXPECTED);

    this->event_handlers.push_back(&e);
}

void gui_event_provider::unregister_event_handler(const gui_event_handler& e)
{
    auto it = std::find(this->event_handlers.begin(), this->event_handlers.end(), &e);
    if(it == this->event_handlers.end())
        throw HR_EXCEPTION(E_UNEXPECTED);

    this->event_handlers.erase(it);
}

void gui_event_provider::for_each(std::function<void(gui_event_handler*)> f)
{
    for(auto&& elem : this->event_handlers)
        f(elem);
}