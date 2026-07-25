/*************************************************************************
 * This file is part of input-overlay
 * git.vrsal.xyz/alex/input-overlay
 * Copyright 2023 Alex <uni@vrsal.xyz>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *************************************************************************/

#include "input_data.hpp"
#include <util/platform.h>
#include <SDL3/SDL.h>

namespace local_data {
input_data data;
}

void input_data::copy(const input_data *other, bool with_gamepad_data)
{
    last_event.store(other->last_event);
    keyboard = other->keyboard;
    mouse = other->mouse;
    last_mouse_movement = other->last_mouse_movement;
    last_wheel_event_time = other->last_wheel_event_time;
    last_wheel_event = other->last_wheel_event;
    last_event_type.store(other->last_event_type);
    trace_sequence = other->trace_sequence;
    trace = other->trace;

    if (with_gamepad_data) {
        gamepad_axis = other->gamepad_axis;
        gamepad_buttons = other->gamepad_buttons;
    }
}

void input_data::dispatch_uiohook_event(const uiohook_event *event)
{
    trace_event trace_entry{};
    trace_entry.sequence = ++trace_sequence;
    trace_entry.time_ns = os_gettime_ns();
    trace_entry.type = event->type;
    if (event->type == EVENT_KEY_PRESSED || event->type == EVENT_KEY_RELEASED) {
        trace_entry.code = event->data.keyboard.keycode;
        trace_entry.keychar = event->data.keyboard.keychar;
    } else if (event->type >= EVENT_MOUSE_CLICKED) {
        trace_entry.code = event->data.mouse.button;
        trace_entry.x = event->data.mouse.x;
        trace_entry.y = event->data.mouse.y;
    }
    trace.push_back(trace_entry);
    if (trace.size() > trace_capacity)
        trace.pop_front();
    if (event->type == EVENT_MOUSE_WHEEL) {
        last_wheel_event = event->data.wheel;
        last_wheel_event_time = os_gettime_ns();
        last_event = event->time;
    } else if (event->type == EVENT_MOUSE_DRAGGED || event->type == EVENT_MOUSE_MOVED) {
        last_mouse_movement = event->data.mouse;
        last_event = event->time;
    } else if (event->type == EVENT_KEY_PRESSED || event->type == EVENT_KEY_RELEASED) {
        keyboard[event->data.keyboard.keycode] = event->type == EVENT_KEY_PRESSED;
        last_event = event->time;
    } else if (event->type == EVENT_MOUSE_PRESSED || event->type == EVENT_MOUSE_RELEASED) {
        last_event = event->time;
        mouse[event->data.mouse.button] = event->type == EVENT_MOUSE_PRESSED;
    }
    last_event_type = event->type;
}

void input_data::events_after(uint64_t &cursor, std::vector<trace_event> &out) const
{
    out.clear();
    if (trace.empty())
        return;
    const uint64_t oldest = trace.front().sequence;
    if (cursor + 1 < oldest)
        cursor = oldest - 1;
    for (const auto &event : trace) {
        if (event.sequence > cursor)
            out.push_back(event);
    }
    cursor = trace.back().sequence;
}
