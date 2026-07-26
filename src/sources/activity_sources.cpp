#include "activity_sources.hpp"

#include "../network/remote_connection.hpp"
#include "../network/websocket_server.hpp"
#include "../util/lang.h"
#include "../util/settings.h"
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <memory>
#include <optional>
#include <type_traits>
#include <obs-module.h>
#include <uiohook.h>
#include <util/platform.h>

extern "C" {
#include <graphics/graphics.h>
}

namespace sources {
namespace {
constexpr uint64_t minute_ns = 60ULL * 1000 * 1000 * 1000;
constexpr uint64_t trail_ns = 3ULL * 1000 * 1000 * 1000;

QColor obs_color(uint32_t color)
{
    return {static_cast<int>(color & 0xff), static_cast<int>((color >> 8) & 0xff),
            static_cast<int>((color >> 16) & 0xff)};
}

class activity_source {
public:
    activity_source(obs_source_t *source, obs_data_t *settings) : source(source)
    {
        obs_source_update(source, settings);
    }
    virtual ~activity_source()
    {
        if (texture) {
            obs_enter_graphics();
            gs_texture_destroy(texture);
            obs_leave_graphics();
        }
    }
    virtual void update(obs_data_t *settings)
    {
        selected_source = obs_data_get_string(settings, S_INPUT_SOURCE);
        width = std::max(1, static_cast<int>(obs_data_get_int(settings, "activity.width")));
        height = std::max(1, static_cast<int>(obs_data_get_int(settings, "activity.height")));
        padding = std::max(0, static_cast<int>(obs_data_get_int(settings, "activity.padding")));
        font_size = std::max(8, static_cast<int>(obs_data_get_int(settings, "activity.font_size")));
        text_color = obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "activity.text_color")));
        if (auto *font = obs_data_get_obj(settings, "activity.font")) {
            font_family = QString::fromUtf8(obs_data_get_string(font, "face"));
            obs_data_release(font);
        }
        remote.reset();
        if (!use_local() && wss::state) {
            std::lock_guard<std::mutex> lock(network::remote_data_map_mutex);
            const auto found = network::remote_data.find(selected_source);
            if (found != network::remote_data.end())
                remote = found->second;
        }
    }
    virtual void tick(float)
    {
        if (!use_local() && !remote && wss::state) {
            std::lock_guard<std::mutex> lock(network::remote_data_map_mutex);
            const auto found = network::remote_data.find(selected_source);
            if (found != network::remote_data.end())
                remote = found->second;
        }
        consume_events();
    }
    void draw(gs_effect_t *effect)
    {
        image = QImage(width, height, QImage::Format_RGBA8888);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        render(painter);
        if (texture && (texture_width != width || texture_height != height)) {
            gs_texture_destroy(texture);
            texture = nullptr;
        }
        if (!texture) {
            texture = gs_texture_create(width, height, GS_RGBA, 1, nullptr, GS_DYNAMIC);
            texture_width = width;
            texture_height = height;
        }
        if (!texture)
            return;
        gs_texture_set_image(texture, image.constBits(), static_cast<uint32_t>(image.bytesPerLine()), false);
        gs_blend_state_push();
        gs_enable_blending(true);
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
        gs_draw_sprite(texture, 0, width, height);
        gs_blend_state_pop();
    }
    bool use_local() const { return selected_source.empty() || selected_source == T_LOCAL_SOURCE; }
    void consume_events()
    {
        std::vector<input_data::trace_event> events;
        input_data::button_map<uint16_t> keyboard;
        input_data::button_map<uint16_t> mouse;
        if (use_local()) {
            std::lock_guard<std::mutex> lock(local_data::data.m_mutex);
            local_data::data.events_after(cursor, events);
            keyboard = local_data::data.keyboard;
            mouse = local_data::data.mouse;
        } else if (remote) {
            std::lock_guard<std::mutex> lock(remote->m_mutex);
            remote->events_after(cursor, events);
            keyboard = remote->keyboard;
            mouse = remote->mouse;
        }
        for (const auto &event : events)
            on_event(event);
        on_snapshot(keyboard, mouse);
    }
    virtual void on_event(const input_data::trace_event &) {}
    virtual void on_snapshot(const input_data::button_map<uint16_t> &, const input_data::button_map<uint16_t> &) {}
    virtual void render(QPainter &) = 0;
    QFont font() const
    {
        QFont result(font_family);
        result.setBold(true);
        result.setPixelSize(font_size);
        return result;
    }
    obs_source_t *source{};
    int width = 480, height = 180, padding = 12, font_size = 28;
    QColor text_color{255, 255, 255};
    QString font_family;
    std::string selected_source;
    std::shared_ptr<input_data> remote;
    uint64_t cursor{};

private:
    QImage image;
    gs_texture_t *texture{};
    int texture_width{}, texture_height{};
};

QString key_name(const input_data::trace_event &event)
{
    if (event.keychar >= 32 && event.keychar < 127)
        return QString(QChar(event.keychar)).toUpper();
    if (event.code >= VC_A && event.code <= VC_Z)
        return QString(QChar('A' + event.code - VC_A));
    if (event.code >= VC_0 && event.code <= VC_9)
        return QString(QChar('0' + event.code - VC_0));
    if (event.code >= VC_F1 && event.code <= VC_F12)
        return QString("F%1").arg(event.code - VC_F1 + 1);
    if (event.code >= VC_F13 && event.code <= VC_F24)
        return QString("F%1").arg(event.code - VC_F13 + 13);
    if (event.code >= VC_KP_0 && event.code <= VC_KP_9)
        return QString("Num %1").arg(event.code - VC_KP_0);
    switch (event.code) {
    case VC_SHIFT_L:
    case VC_SHIFT_R:
        return "Shift";
    case VC_CONTROL_L:
    case VC_CONTROL_R:
        return "Ctrl";
    case VC_ALT_L:
    case VC_ALT_R:
        return "Alt";
    case VC_META_L:
    case VC_META_R:
        return "Cmd";
    case VC_SPACE:
        return "Space";
    case VC_ENTER:
        return "Enter";
    case VC_ESCAPE:
        return "Esc";
    case VC_BACKSPACE:
        return "Backspace";
    case VC_TAB:
        return "Tab";
    case VC_CAPS_LOCK:
        return "Caps Lock";
    case VC_MINUS:
        return "-";
    case VC_EQUALS:
        return "=";
    case VC_OPEN_BRACKET:
        return "[";
    case VC_CLOSE_BRACKET:
        return "]";
    case VC_BACK_SLASH:
        return "\\";
    case VC_SEMICOLON:
        return ";";
    case VC_QUOTE:
        return "'";
    case VC_COMMA:
        return ",";
    case VC_PERIOD:
        return ".";
    case VC_SLASH:
        return "/";
    case VC_BACK_QUOTE:
        return "`";
    case VC_UP:
        return "Up";
    case VC_DOWN:
        return "Down";
    case VC_LEFT:
        return "Left";
    case VC_RIGHT:
        return "Right";
    case VC_HOME:
        return "Home";
    case VC_END:
        return "End";
    case VC_PAGE_UP:
        return "Page Up";
    case VC_PAGE_DOWN:
        return "Page Down";
    case VC_INSERT:
        return "Insert";
    case VC_DELETE:
        return "Delete";
    case VC_PRINT_SCREEN:
        return "Print Screen";
    case VC_SCROLL_LOCK:
        return "Scroll Lock";
    case VC_PAUSE:
        return "Pause";
    case VC_NUM_LOCK:
        return "Num Lock";
    case VC_KP_DIVIDE:
        return "Num /";
    case VC_KP_MULTIPLY:
        return "Num *";
    case VC_KP_SUBTRACT:
        return "Num -";
    case VC_KP_ADD:
        return "Num +";
    case VC_KP_DECIMAL:
        return "Num .";
    case VC_KP_ENTER:
        return "Num Enter";
    default:
        return QString("Unknown key");
    }
}

class live_keys_source final : public activity_source {
public:
    using activity_source::activity_source;
    void update(obs_data_t *settings) override
    {
        activity_source::update(settings);
        maximum = std::max(1, static_cast<int>(obs_data_get_int(settings, "live_keys.maximum")));
        fade_duration_ns =
            static_cast<uint64_t>(std::max<int64_t>(0, obs_data_get_int(settings, "live_keys.fade_ms"))) * 1000 * 1000;
        active_color = obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "live_keys.color")));
    }
    void on_event(const input_data::trace_event &event) override
    {
        if (event.type == EVENT_KEY_PRESSED && !held[event.code]) {
            held[event.code] = true;
            ordered.erase(std::remove_if(ordered.begin(), ordered.end(),
                                         [&event](const auto &key) { return key.code == event.code; }),
                          ordered.end());
            ordered.push_back({event.code, key_name(event), 0});
        } else if (event.type == EVENT_KEY_RELEASED) {
            held[event.code] = false;
            for (auto &key : ordered) {
                if (key.code == event.code)
                    key.fade_until = event.time_ns + fade_duration_ns;
            }
        }
    }
    void on_snapshot(const input_data::button_map<uint16_t> &keyboard,
                     const input_data::button_map<uint16_t> &) override
    {
        const uint64_t now = os_gettime_ns();
        for (auto it = ordered.begin(); it != ordered.end();) {
            const auto pressed = keyboard.find(it->code);
            if (pressed == keyboard.end() || !pressed->second) {
                held[it->code] = false;
                if (it->fade_until == 0)
                    it->fade_until = now + fade_duration_ns;
                if (it->fade_until <= now)
                    it = ordered.erase(it);
                else
                    ++it;
            } else {
                ++it;
            }
        }
        for (const auto &[code, pressed] : keyboard) {
            if (pressed && !held[code]) {
                held[code] = true;
                input_data::trace_event event{};
                event.code = code;
                ordered.push_back({code, key_name(event), 0});
            }
        }
    }
    void render(QPainter &painter) override
    {
        painter.setFont(font());
        const int row_height = std::max(1, (height - padding * 2) / maximum);
        const int start = std::max(0, static_cast<int>(ordered.size()) - maximum);
        const uint64_t now = os_gettime_ns();
        for (int index = start; index < static_cast<int>(ordered.size()); ++index) {
            const QRect row(padding, padding + (index - start) * row_height, width - padding * 2, row_height - 2);
            const auto &key = ordered[index];
            const int alpha = key.fade_until > now && fade_duration_ns > 0
                                  ? static_cast<int>(255 * static_cast<double>(key.fade_until - now) / fade_duration_ns)
                                  : (key.fade_until ? 0 : 255);
            QColor fill = active_color;
            fill.setAlpha(std::clamp(alpha, 0, 255));
            QColor text = text_color;
            text.setAlpha(std::clamp(alpha, 0, 255));
            painter.setBrush(fill);
            painter.setPen(Qt::NoPen);
            painter.drawRoundedRect(row, 6, 6);
            painter.setPen(text);
            painter.drawText(row, Qt::AlignCenter, key.label);
        }
    }

private:
    struct active_key {
        uint16_t code;
        QString label;
        uint64_t fade_until;
    };
    int maximum = 8;
    uint64_t fade_duration_ns = 300ULL * 1000 * 1000;
    QColor active_color{37, 99, 235};
    std::unordered_map<uint16_t, bool> held;
    std::vector<active_key> ordered;
};

class mouse_activity_source final : public activity_source {
public:
    using activity_source::activity_source;
    void update(obs_data_t *settings) override
    {
        activity_source::update(settings);
        left_label = QString::fromUtf8(obs_data_get_string(settings, "mouse_activity.left_label"));
        right_label = QString::fromUtf8(obs_data_get_string(settings, "mouse_activity.right_label"));
        middle_label = QString::fromUtf8(obs_data_get_string(settings, "mouse_activity.middle_label"));
        active_color = obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "mouse_activity.color")));
        display = static_cast<int>(obs_data_get_int(settings, "mouse_activity.display"));
        load_display();
        if (heatmap.isNull() || heatmap.width() != width || heatmap.height() != height) {
            heatmap = QImage(width, height, QImage::Format_RGBA8888);
            heatmap.fill(Qt::transparent);
        }
    }
    void clear() { heatmap.fill(Qt::transparent); }
    void on_event(const input_data::trace_event &event) override
    {
        if (event.type == EVENT_MOUSE_PRESSED || event.type == EVENT_MOUSE_RELEASED)
            buttons[event.code] = event.type == EVENT_MOUSE_PRESSED;
        if ((event.type != EVENT_MOUSE_MOVED && event.type != EVENT_MOUSE_DRAGGED) || monitor.width == 0)
            return;
        if (event.x < monitor.x || event.y < monitor.y || event.x >= monitor.x + monitor.width ||
            event.y >= monitor.y + monitor.height)
            return;
        const QPoint point((event.x - monitor.x) * width / monitor.width,
                           (event.y - monitor.y) * height / monitor.height);
        trail.push_back({event.time_ns, point});
        QPainter painter(&heatmap);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(active_color.red(), active_color.green(), active_color.blue(), 18));
        painter.drawEllipse(point, 10, 10);
    }
    void on_snapshot(const input_data::button_map<uint16_t> &, const input_data::button_map<uint16_t> &mouse) override
    {
        for (uint16_t button = MOUSE_BUTTON1; button <= MOUSE_BUTTON3; ++button) {
            const auto pressed = mouse.find(button);
            buttons[button] = pressed != mouse.end() && pressed->second;
        }
    }
    void tick(float seconds) override
    {
        activity_source::tick(seconds);
        const uint64_t now = os_gettime_ns();
        while (!trail.empty() && now - trail.front().first > trail_ns)
            trail.pop_front();
    }
    void render(QPainter &painter) override
    {
        painter.drawImage(0, 0, heatmap);
        painter.setFont(font());
        const int button_width = (width - padding * 4) / 3;
        draw_button(painter, QRect(padding, padding, button_width, std::max(32, height / 4)), left_label,
                    MOUSE_BUTTON1);
        draw_button(painter, QRect(padding * 2 + button_width, padding, button_width, std::max(32, height / 4)),
                    right_label, MOUSE_BUTTON2);
        draw_button(painter, QRect(padding * 3 + button_width * 2, padding, button_width, std::max(32, height / 4)),
                    middle_label, MOUSE_BUTTON3);
        const uint64_t now = os_gettime_ns();
        for (const auto &point : trail) {
            const int alpha = static_cast<int>(220 * (1.0 - static_cast<double>(now - point.first) / trail_ns));
            painter.setBrush(QColor(active_color.red(), active_color.green(), active_color.blue(), std::max(0, alpha)));
            painter.setPen(Qt::NoPen);
            painter.drawEllipse(point.second, 5, 5);
        }
    }

private:
    void load_display()
    {
        unsigned char count{};
        std::unique_ptr<screen_data, decltype(&free)> screens(hook_create_screen_info(&count), &free);
        if (screens && display >= 0 && display < count)
            monitor = screens.get()[display];
        else
            monitor = {};
    }
    void draw_button(QPainter &painter, const QRect &rect, const QString &label, uint16_t button)
    {
        painter.setBrush(buttons[button] ? active_color : QColor(30, 30, 30, 210));
        painter.setPen(text_color);
        painter.drawRoundedRect(rect, 6, 6);
        painter.drawText(rect, Qt::AlignCenter, label);
    }
    QString left_label{"LMB"}, right_label{"RMB"}, middle_label{"MMB"};
    QColor active_color{37, 99, 235};
    int display{};
    screen_data monitor{};
    std::unordered_map<uint16_t, bool> buttons;
    std::deque<std::pair<uint64_t, QPoint>> trail;
    QImage heatmap;
};

class statistics_source final : public activity_source {
public:
    statistics_source(obs_source_t *source, obs_data_t *settings) : activity_source(source, settings)
    {
        hotkey = obs_hotkey_register_source(
            source, "reset_input_statistics", obs_module_text("Statistics.ResetHotkey"),
            [](void *data, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
                if (pressed)
                    static_cast<statistics_source *>(data)->reset();
            },
            this);
    }
    ~statistics_source() override { obs_hotkey_unregister(hotkey); }
    void on_event(const input_data::trace_event &event) override
    {
        if (event.type == EVENT_KEY_PRESSED) {
            if (!held_keys[event.code]) {
                held_keys[event.code] = true;
                keys.push_back(event.time_ns);
            }
        } else if (event.type == EVENT_KEY_RELEASED) {
            held_keys[event.code] = false;
        } else if (event.type == EVENT_MOUSE_PRESSED && event.code >= MOUSE_BUTTON1 && event.code <= MOUSE_BUTTON3) {
            if (!held_buttons[event.code]) {
                held_buttons[event.code] = true;
                clicks.push_back(event.time_ns);
            }
        } else if (event.type == EVENT_MOUSE_RELEASED) {
            held_buttons[event.code] = false;
        } else if (event.type == EVENT_MOUSE_MOVED || event.type == EVENT_MOUSE_DRAGGED) {
            if (last_motion)
                distance += std::hypot(static_cast<double>(event.x - last_motion->x),
                                       static_cast<double>(event.y - last_motion->y));
            last_motion = event;
        }
    }
    void on_snapshot(const input_data::button_map<uint16_t> &keyboard,
                     const input_data::button_map<uint16_t> &mouse) override
    {
        held_keys = keyboard;
        for (uint16_t button = MOUSE_BUTTON1; button <= MOUSE_BUTTON3; ++button) {
            const auto pressed = mouse.find(button);
            held_buttons[button] = pressed != mouse.end() && pressed->second;
        }
    }
    void tick(float seconds) override
    {
        activity_source::tick(seconds);
        const uint64_t cutoff = os_gettime_ns() - minute_ns;
        while (!keys.empty() && keys.front() < cutoff)
            keys.pop_front();
        while (!clicks.empty() && clicks.front() < cutoff)
            clicks.pop_front();
    }
    void render(QPainter &painter) override
    {
        painter.setFont(font());
        painter.setPen(text_color);
        const QString text = QString("KPM: %1\nCPM: %2\nAPM: %3\nDistance: %4 px")
                                 .arg(keys.size())
                                 .arg(clicks.size())
                                 .arg(keys.size() + clicks.size())
                                 .arg(distance, 0, 'f', 0);
        painter.drawText(QRect(padding, padding, width - padding * 2, height - padding * 2),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
    void reset()
    {
        keys.clear();
        clicks.clear();
        distance = 0;
        last_motion.reset();
    }

private:
    obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
    std::deque<uint64_t> keys, clicks;
    std::unordered_map<uint16_t, bool> held_keys, held_buttons;
    double distance{};
    std::optional<input_data::trace_event> last_motion;
};

bool reload_connections(obs_properties_t *, obs_property_t *property, void *)
{
    obs_property_list_clear(property);
    obs_property_list_add_string(property, T_LOCAL_SOURCE, "");
    std::lock_guard<std::mutex> lock(network::remote_data_map_mutex);
    for (const auto &connection : network::remote_data)
        obs_property_list_add_string(property, connection.first.c_str(), connection.first.c_str());
    return true;
}
void add_common_properties(obs_properties_t *props)
{
    auto *list = obs_properties_add_list(props, S_INPUT_SOURCE, T_INPUT_SOURCE, OBS_COMBO_TYPE_EDITABLE,
                                         OBS_COMBO_FORMAT_STRING);
    reload_connections(nullptr, list, nullptr);
    obs_properties_add_button(props, S_RELOAD_CONNECTIONS, T_RELOAD_CONNECTIONS, reload_connections);
    obs_properties_add_int(props, "activity.width", obs_module_text("Activity.Width"), 64, 3840, 1);
    obs_properties_add_int(props, "activity.height", obs_module_text("Activity.Height"), 32, 2160, 1);
    obs_properties_add_int(props, "activity.padding", obs_module_text("Activity.Padding"), 0, 200, 1);
    obs_properties_add_font(props, "activity.font", obs_module_text("Activity.Font"));
    obs_properties_add_int(props, "activity.font_size", obs_module_text("Activity.FontSize"), 8, 256, 1);
    obs_properties_add_color(props, "activity.text_color", obs_module_text("Activity.TextColor"));
}
template<typename T> void register_source(const char *id, const char *name, obs_properties_t *(*properties)(void *))
{
    obs_source_info info{};
    info.id = id;
    info.type = OBS_SOURCE_TYPE_INPUT;
    info.output_flags = OBS_SOURCE_VIDEO;
    if constexpr (std::is_same_v<T, live_keys_source>)
        info.get_name = [](void *) { return obs_module_text("LiveKeys"); };
    else if constexpr (std::is_same_v<T, mouse_activity_source>)
        info.get_name = [](void *) { return obs_module_text("MouseActivity"); };
    else
        info.get_name = [](void *) { return obs_module_text("InputStatistics"); };
    info.create = [](obs_data_t *settings, obs_source_t *source) {
        return static_cast<void *>(new T(source, settings));
    };
    info.destroy = [](void *data) { delete static_cast<T *>(data); };
    info.update = [](void *data, obs_data_t *settings) { static_cast<T *>(data)->update(settings); };
    info.video_tick = [](void *data, float seconds) { static_cast<T *>(data)->tick(seconds); };
    info.video_render = [](void *data, gs_effect_t *effect) { static_cast<T *>(data)->draw(effect); };
    info.get_width = [](void *data) { return static_cast<uint32_t>(static_cast<T *>(data)->width); };
    info.get_height = [](void *data) { return static_cast<uint32_t>(static_cast<T *>(data)->height); };
    info.get_properties = properties;
    info.get_defaults = [](obs_data_t *settings) {
        obs_data_set_default_int(settings, "activity.width", 480);
        obs_data_set_default_int(settings, "activity.height", 180);
        obs_data_set_default_int(settings, "activity.padding", 12);
        obs_data_set_default_int(settings, "activity.font_size", 28);
        obs_data_set_default_int(settings, "activity.text_color", 0xffffff);
        if constexpr (std::is_same_v<T, live_keys_source>) {
            obs_data_set_default_int(settings, "live_keys.maximum", 8);
            obs_data_set_default_int(settings, "live_keys.fade_ms", 300);
            obs_data_set_default_int(settings, "live_keys.color", 0xeb6325);
        } else if constexpr (std::is_same_v<T, mouse_activity_source>) {
            obs_data_set_default_string(settings, "mouse_activity.left_label", "LMB");
            obs_data_set_default_string(settings, "mouse_activity.right_label", "RMB");
            obs_data_set_default_string(settings, "mouse_activity.middle_label", "MMB");
            obs_data_set_default_int(settings, "mouse_activity.color", 0xeb6325);
        }
    };
    obs_register_source(&info);
}
obs_properties_t *keys_properties(void *)
{
    auto *p = obs_properties_create();
    add_common_properties(p);
    obs_properties_add_int(p, "live_keys.maximum", obs_module_text("LiveKeys.Maximum"), 1, 64, 1);
    obs_properties_add_int_slider(p, "live_keys.fade_ms", obs_module_text("LiveKeys.FadeDuration"), 0, 5000, 10);
    obs_properties_add_color(p, "live_keys.color", obs_module_text("Activity.ActiveColor"));
    return p;
}
obs_properties_t *mouse_properties(void *data)
{
    auto *p = obs_properties_create();
    add_common_properties(p);
    obs_properties_add_text(p, "mouse_activity.left_label", obs_module_text("MouseActivity.LeftLabel"),
                            OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, "mouse_activity.right_label", obs_module_text("MouseActivity.RightLabel"),
                            OBS_TEXT_DEFAULT);
    obs_properties_add_text(p, "mouse_activity.middle_label", obs_module_text("MouseActivity.MiddleLabel"),
                            OBS_TEXT_DEFAULT);
    obs_properties_add_color(p, "mouse_activity.color", obs_module_text("Activity.ActiveColor"));
    auto *displays = obs_properties_add_list(p, "mouse_activity.display", obs_module_text("MouseActivity.Display"),
                                             OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
    unsigned char count{};
    std::unique_ptr<screen_data, decltype(&free)> screens(hook_create_screen_info(&count), &free);
    for (int i = 0; screens && i < count; ++i) {
        const auto &s = screens.get()[i];
        const QByteArray label =
            QString("Display %1 (%2x%3 at %4,%5)").arg(i + 1).arg(s.width).arg(s.height).arg(s.x).arg(s.y).toUtf8();
        obs_property_list_add_int(displays, label.constData(), i);
    }
    obs_properties_add_button2(
        p, "mouse_activity.clear", obs_module_text("MouseActivity.ClearHeatmap"),
        [](obs_properties_t *, obs_property_t *, void *d) {
            static_cast<mouse_activity_source *>(d)->clear();
            return true;
        },
        data);
    return p;
}
obs_properties_t *statistics_properties(void *)
{
    auto *p = obs_properties_create();
    add_common_properties(p);
    return p;
}
} // namespace

void register_activity_sources()
{
    register_source<live_keys_source>("input-overlay-live-keys", "LiveKeys", keys_properties);
    register_source<mouse_activity_source>("input-overlay-mouse-activity", "MouseActivity", mouse_properties);
    register_source<statistics_source>("input-overlay-statistics", "InputStatistics", statistics_properties);
}
} // namespace sources
