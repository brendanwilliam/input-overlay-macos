/*************************************************************************
 * This file is part of input-overlay
 * git.vrsal.cc/alex/input-overlay
 * Copyright 2026 univrsal <uni@vrsal.cc>.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2 of the License.
 *************************************************************************/

#include "uiohook_helper.hpp"

#include <ApplicationServices/ApplicationServices.h>
#include <cstdarg>
#include <cstdlib>
#include <obs-module.h>
#include <pthread.h>
#include <sched.h>
#include <uiohook.h>

namespace uiohook {

uint64_t last_scroll_time = 0;
bool state = false;

static pthread_t hook_thread;
static pthread_mutex_t hook_running_mutex;
static pthread_mutex_t hook_control_mutex;
static pthread_cond_t hook_control_cond;

void *hook_thread_proc(void *arg)
{
    const int status = hook_run();
    if (status != UIOHOOK_SUCCESS)
        *(int *)arg = status;

    pthread_cond_signal(&hook_control_cond);
    pthread_mutex_unlock(&hook_control_mutex);
    return arg;
}

extern "C" {
static void logger_proc(unsigned int level, void *, const char *format, va_list args)
{
    switch (level) {
    default:
    case LOG_LEVEL_DEBUG:
        blogva(LOG_DEBUG, format, args);
        break;
    case LOG_LEVEL_INFO:
        blogva(LOG_INFO, format, args);
        break;
    case LOG_LEVEL_WARN:
    case LOG_LEVEL_ERROR:
        blogva(LOG_WARNING, format, args);
        break;
    }
}
}

void dispatch_proc(uiohook_event *event, void *)
{
    switch (event->type) {
    case EVENT_HOOK_ENABLED:
        pthread_mutex_lock(&hook_running_mutex);
        pthread_cond_signal(&hook_control_cond);
        pthread_mutex_unlock(&hook_control_mutex);
        break;
    case EVENT_HOOK_DISABLED:
        pthread_mutex_lock(&hook_control_mutex);
        pthread_mutex_unlock(&hook_running_mutex);
    default:
        break;
    }
    process_event(event);
}

static int hook_enable()
{
    pthread_mutex_lock(&hook_control_mutex);
    int status = UIOHOOK_FAILURE;

    pthread_attr_t hook_thread_attr;
    pthread_attr_init(&hook_thread_attr);

    int policy;
    pthread_attr_getschedpolicy(&hook_thread_attr, &policy);
    const int priority = sched_get_priority_max(policy);

    auto *hook_thread_status = static_cast<int *>(malloc(sizeof(int)));
    if (hook_thread_status)
        *hook_thread_status = UIOHOOK_FAILURE;
    if (hook_thread_status &&
        pthread_create(&hook_thread, &hook_thread_attr, hook_thread_proc, hook_thread_status) == 0) {
        const sched_param param = {.sched_priority = priority};
        if (pthread_setschedparam(hook_thread, SCHED_OTHER, &param) != 0) {
            blog(LOG_WARNING, "[input-overlay] Could not set uiohook thread priority.");
        }

        pthread_cond_wait(&hook_control_cond, &hook_control_mutex);
        if (pthread_mutex_trylock(&hook_running_mutex) == 0) {
            void *thread_result = nullptr;
            pthread_join(hook_thread, &thread_result);
            status = *static_cast<int *>(thread_result);
            pthread_mutex_unlock(&hook_running_mutex);
        } else {
            status = UIOHOOK_SUCCESS;
        }
        free(hook_thread_status);
    } else {
        free(hook_thread_status);
        status = -1;
    }

    pthread_attr_destroy(&hook_thread_attr);
    pthread_mutex_unlock(&hook_control_mutex);
    return status;
}

void start()
{
    if (state)
        return;

    if (!AXIsProcessTrusted()) {
        blog(LOG_WARNING, "[input-overlay] macOS Accessibility permission is required for keyboard and mouse capture. "
                          "Enable OBS in System Settings > Privacy & Security > Accessibility, then restart OBS.");
        return;
    }

    pthread_mutex_init(&hook_running_mutex, nullptr);
    pthread_mutex_init(&hook_control_mutex, nullptr);
    pthread_cond_init(&hook_control_cond, nullptr);
    hook_set_logger_proc(&logger_proc, nullptr);
    hook_set_dispatch_proc(&dispatch_proc, nullptr);

    const int status = hook_enable();
    if (status == UIOHOOK_SUCCESS) {
        state = true;
        return;
    }

    switch (status) {
    case UIOHOOK_ERROR_AXAPI_DISABLED:
        blog(LOG_ERROR,
             "[input-overlay] macOS Accessibility permission was denied. Enable OBS in System Settings > Privacy & "
             "Security > Accessibility, then restart OBS. (%#X)",
             status);
        break;
    case UIOHOOK_ERROR_CREATE_EVENT_PORT:
        blog(LOG_ERROR, "[input-overlay] Failed to create macOS input event port. (%#X)", status);
        break;
    case UIOHOOK_ERROR_CREATE_RUN_LOOP_SOURCE:
        blog(LOG_ERROR, "[input-overlay] Failed to create macOS input run loop source. (%#X)", status);
        break;
    case UIOHOOK_ERROR_GET_RUNLOOP:
        blog(LOG_ERROR, "[input-overlay] Failed to acquire macOS input run loop. (%#X)", status);
        break;
    case UIOHOOK_ERROR_CREATE_OBSERVER:
        blog(LOG_ERROR, "[input-overlay] Failed to create macOS input run loop observer. (%#X)", status);
        break;
    default:
        blog(LOG_ERROR, "[input-overlay] Failed to start macOS input hook. (%#X)", status);
        break;
    }

    pthread_mutex_destroy(&hook_running_mutex);
    pthread_mutex_destroy(&hook_control_mutex);
    pthread_cond_destroy(&hook_control_cond);
}

void stop()
{
    if (!state)
        return;

    hook_stop();
    pthread_join(hook_thread, nullptr);
    state = false;
    pthread_mutex_destroy(&hook_running_mutex);
    pthread_mutex_destroy(&hook_control_mutex);
    pthread_cond_destroy(&hook_control_cond);
}

} // namespace uiohook
