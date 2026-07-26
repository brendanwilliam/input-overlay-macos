#include "activity_sources.hpp"

#include "../network/remote_connection.hpp"
#include "../network/websocket_server.hpp"
#include "../util/lang.h"
#include "../util/settings.h"
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <vector>
#include <obs-module.h>
#include <uiohook.h>
#include <util/platform.h>

extern "C" {
#include <graphics/graphics.h>
}

namespace sources {
namespace {
constexpr uint64_t minute_ns = 60ULL * 1000 * 1000 * 1000;
constexpr uint64_t max_heatmap_gap_ns = 250ULL * 1000 * 1000;
constexpr qreal heatmap_hex_radius = 10.0;

QColor obs_color(uint32_t color)
{
    return {static_cast<int>(color & 0xff), static_cast<int>((color >> 8) & 0xff),
            static_cast<int>((color >> 16) & 0xff)};
}

class activity_source {
public:
    activity_source(obs_source_t *source, obs_data_t *settings) : source(source)
    {
        {
            std::lock_guard<std::mutex> lock(activity_sources_mutex);
            activity_sources.insert(this);
        }
        obs_source_update(source, settings);
    }
    virtual ~activity_source()
    {
        {
            std::lock_guard<std::mutex> lock(activity_sources_mutex);
            activity_sources.erase(this);
        }
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
    virtual void reset_activity() {}
    virtual void render(QPainter &) = 0;
    static void reset_all_activity()
    {
        std::lock_guard<std::mutex> lock(activity_sources_mutex);
        for (auto *activity : activity_sources)
            activity->reset_activity();
    }
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
    static std::mutex activity_sources_mutex;
    static std::unordered_set<activity_source *> activity_sources;
    QImage image;
    gs_texture_t *texture{};
    int texture_width{}, texture_height{};
};

std::mutex activity_source::activity_sources_mutex;
std::unordered_set<activity_source *> activity_source::activity_sources;

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
        row_layout = obs_data_get_bool(settings, "live_keys.row_layout");
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
            ordered.push_back({event.code, key_name(event), 0, ++press_counts[event.code]});
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
                ordered.push_back({code, key_name(event), 0, press_counts[code]});
            }
        }
    }
    void render(QPainter &painter) override
    {
        painter.setFont(font());
        const int start = std::max(0, static_cast<int>(ordered.size()) - maximum);
        const int visible = static_cast<int>(ordered.size()) - start;
        const int gap = 2;
        const int key_width = row_layout ? std::max(1, (width - padding * 2 - gap * (visible - 1)) / std::max(1, visible))
                                         : std::max(1, width - padding * 2);
        const int key_height = row_layout ? std::max(1, height - padding * 2)
                                          : std::max(1, (height - padding * 2) / maximum - gap);
        const uint64_t now = os_gettime_ns();
        for (int index = start; index < static_cast<int>(ordered.size()); ++index) {
            const int position = index - start;
            const QRect row(row_layout ? padding + position * (key_width + gap) : padding,
                            row_layout ? padding : padding + position * (key_height + gap), key_width, key_height);
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
            painter.drawText(row, Qt::AlignCenter, QString("%1\n%2").arg(key.label).arg(key.press_count));
        }
    }

private:
    struct active_key {
        uint16_t code;
        QString label;
        uint64_t fade_until;
        uint64_t press_count;
    };
    int maximum = 8;
    uint64_t fade_duration_ns = 300ULL * 1000 * 1000;
    QColor active_color{37, 99, 235};
    std::unordered_map<uint16_t, bool> held;
    std::unordered_map<uint16_t, uint64_t> press_counts;
    std::vector<active_key> ordered;
    bool row_layout{};
};

class mouse_activity_source final : public activity_source {
public:
    using activity_source::activity_source;
    void update(obs_data_t *settings) override
    {
        const QRect previous_heatmap = heatmap_rect();
        activity_source::update(settings);
        left_label = QString::fromUtf8(obs_data_get_string(settings, "mouse_activity.left_label"));
        right_label = QString::fromUtf8(obs_data_get_string(settings, "mouse_activity.right_label"));
        middle_label = QString::fromUtf8(obs_data_get_string(settings, "mouse_activity.middle_label"));
        show_coordinates = obs_data_get_bool(settings, "mouse_activity.show_coordinates");
        heatmap_gradient = obs_data_get_string(settings, "mouse_activity.heatmap_gradient");
        trail_duration_ns = static_cast<uint64_t>(
            std::max<int64_t>(100, obs_data_get_int(settings, "mouse_activity.trail_ms")) * 1000 * 1000);
        active_color = obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "mouse_activity.color")));
        button_colors[MOUSE_BUTTON1] =
            obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "mouse_activity.left_color")));
        button_colors[MOUSE_BUTTON2] =
            obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "mouse_activity.right_color")));
        button_colors[MOUSE_BUTTON3] =
            obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "mouse_activity.middle_color")));
        const bool new_map_clicks = std::string(obs_data_get_string(settings, "mouse_activity.map")) == "clicks";
        const int new_display = static_cast<int>(obs_data_get_int(settings, "mouse_activity.display"));
        const bool display_changed = new_display != display;
        if (display_changed || new_map_clicks != map_clicks) {
            coordinates.reset();
            clear();
        }
        map_clicks = new_map_clicks;
        display = new_display;
        load_display();
        resize_heatmap();
        if (display_changed || heatmap_rect() != previous_heatmap)
            trail.clear();
    }
    void clear()
    {
        for (auto &bin : hex_bins)
            bin.value = 0;
        last_motion.reset();
    }
    void on_event(const input_data::trace_event &event) override
    {
        if (event.type == EVENT_MOUSE_PRESSED || event.type == EVENT_MOUSE_RELEASED)
            buttons[event.code] = event.type == EVENT_MOUSE_PRESSED;
        const bool moved = event.type == EVENT_MOUSE_MOVED || event.type == EVENT_MOUSE_DRAGGED;
        const bool clicked = event.type == EVENT_MOUSE_PRESSED && event.code >= MOUSE_BUTTON1 &&
                             event.code <= MOUSE_BUTTON3;
        if ((!moved && !clicked) || monitor.width == 0)
            return;
        if (event.x < monitor.x || event.y < monitor.y || event.x >= monitor.x + monitor.width ||
            event.y >= monitor.y + monitor.height) {
            coordinates.reset();
            last_motion.reset();
            return;
        }
        const int relative_x = event.x - monitor.x;
        const int relative_y = event.y - monitor.y;
        coordinates = QPoint(relative_x, relative_y);
        const QRect heatmap = heatmap_rect();
        const QPoint point(heatmap.x() + relative_x * heatmap.width() / monitor.width,
                           heatmap.y() + relative_y * heatmap.height() / monitor.height);
        if (moved) {
            const qreal maximum_trail_jump = std::hypot(heatmap.width(), heatmap.height()) / 3.0;
            if (!trail.empty() && std::hypot(point.x() - trail.back().second.x(), point.y() - trail.back().second.y()) >
                                      maximum_trail_jump)
                trail.clear();
            trail.push_back({event.time_ns, point});
        }
        const size_t hex_index = nearest_hex(point);
        if (map_clicks && clicked) {
            ++hex_bins[hex_index].value;
        } else if (!map_clicks && moved && last_motion && event.time_ns > last_motion->time_ns) {
            const uint64_t duration = std::min(event.time_ns - last_motion->time_ns, max_heatmap_gap_ns);
            hex_bins[last_motion->hex_index].value += duration;
        }
        if (moved)
            last_motion = motion_point{event.time_ns, hex_index};
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
        while (!trail.empty() && now - trail.front().first > trail_duration_ns)
            trail.pop_front();
    }
    void render(QPainter &painter) override
    {
        const QRect heatmap = heatmap_rect();
        draw_heatmap(painter, heatmap);
        draw_trail(painter, os_gettime_ns());
        painter.setFont(font());
        draw_pointer(painter);
        if (show_coordinates && coordinates) {
            painter.setPen(text_color);
            painter.drawText(coordinate_rect(), Qt::AlignBottom | Qt::AlignHCenter,
                             QString("X: %1  Y: %2").arg(coordinates->x()).arg(coordinates->y()));
        }
    }

private:
    struct motion_point {
        uint64_t time_ns;
        size_t hex_index;
    };
    struct hex_bin {
        QPointF center;
        uint64_t value{};
    };
    void resize_heatmap()
    {
        const QRect rect = heatmap_rect();
        if (rect == heatmap_bounds)
            return;
        heatmap_bounds = rect;
        build_hex_lattice();
        last_motion.reset();
    }
    QRect coordinate_rect() const { return {padding, padding, width - padding * 2, font_size}; }
    QRect heatmap_rect() const
    {
        const int top = show_coordinates ? coordinate_rect().bottom() + padding + 1 : padding;
        return {padding, top, std::max(1, width - padding * 2), std::max(1, height - padding - top)};
    }
    void build_hex_lattice()
    {
        hex_bins.clear();
        const QRect rect = heatmap_rect();
        const qreal hex_width = std::sqrt(3.0) * heatmap_hex_radius;
        const qreal row_step = 1.5 * heatmap_hex_radius;
        hex_columns = std::max(1, static_cast<int>(std::ceil(rect.width() / hex_width)) + 1);
        hex_rows = std::max(1, static_cast<int>(std::ceil(rect.height() / row_step)) + 1);
        hex_bins.reserve(static_cast<size_t>(hex_columns * hex_rows));
        for (int row = 0; row < hex_rows; ++row) {
            const qreal x_offset = row % 2 ? hex_width / 2.0 : 0.0;
            for (int column = 0; column < hex_columns; ++column)
                hex_bins.push_back({{rect.left() + hex_width / 2.0 + x_offset + column * hex_width,
                                     rect.top() + heatmap_hex_radius + row * row_step}});
        }
    }
    size_t nearest_hex(const QPointF &point) const
    {
        const QRect rect = heatmap_rect();
        const qreal hex_width = std::sqrt(3.0) * heatmap_hex_radius;
        const qreal row_step = 1.5 * heatmap_hex_radius;
        const int estimated_row =
            static_cast<int>(std::floor((point.y() - rect.top() - heatmap_hex_radius) / row_step));
        size_t nearest{};
        qreal nearest_distance = std::numeric_limits<qreal>::max();
        for (int row = std::max(0, estimated_row - 1); row <= std::min(hex_rows - 1, estimated_row + 1); ++row) {
            const qreal x_offset = row % 2 ? hex_width / 2.0 : 0.0;
            const int estimated_column =
                static_cast<int>(std::floor((point.x() - rect.left() - hex_width / 2.0 - x_offset) / hex_width));
            for (int column = std::max(0, estimated_column - 1);
                 column <= std::min(hex_columns - 1, estimated_column + 1); ++column) {
                const size_t index = static_cast<size_t>(row * hex_columns + column);
                const qreal dx = point.x() - hex_bins[index].center.x();
                const qreal dy = point.y() - hex_bins[index].center.y();
                const qreal distance = dx * dx + dy * dy;
                if (distance < nearest_distance) {
                    nearest_distance = distance;
                    nearest = index;
                }
            }
        }
        return nearest;
    }
    QPainterPath trail_path(size_t first, size_t last) const
    {
        QPainterPath path(trail[first].second);
        if (first == last)
            return path;
        if (first + 1 == last) {
            path.lineTo(trail[last].second);
            return path;
        }
        for (size_t index = first + 1; index < last; ++index) {
            const QPointF midpoint = (trail[index].second + trail[index + 1].second) / 2.0;
            path.quadTo(trail[index].second, midpoint);
        }
        path.lineTo(trail[last].second);
        return path;
    }
    void draw_trail(QPainter &painter, uint64_t now) const
    {
        if (trail.empty())
            return;
        painter.setBrush(Qt::NoBrush);
        size_t first_segment = 0;
        while (first_segment + 1 < trail.size()) {
            const double age =
                std::clamp(static_cast<double>(now - trail[first_segment + 1].first) / trail_duration_ns, 0.0, 1.0);
            const int band = std::min(3, static_cast<int>(age * 4.0));
            size_t last_point = first_segment + 1;
            while (last_point + 1 < trail.size()) {
                const double next_age =
                    std::clamp(static_cast<double>(now - trail[last_point + 1].first) / trail_duration_ns, 0.0, 1.0);
                if (std::min(3, static_cast<int>(next_age * 4.0)) != band)
                    break;
                ++last_point;
            }
            const double strength = 1.0 - (band + 0.5) / 4.0;
            QPen pen(QColor(active_color.red(), active_color.green(), active_color.blue(),
                            static_cast<int>(180 * strength * strength)));
            pen.setWidthF(2.0 + 6.0 * strength);
            pen.setCapStyle(Qt::FlatCap);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            painter.drawPath(trail_path(first_segment, last_point));
            first_segment = last_point;
        }
        painter.setBrush(active_color);
        painter.setPen(text_color);
        painter.drawEllipse(trail.back().second, 8, 8);
    }
    void draw_heatmap(QPainter &painter, const QRect &rect) const
    {
        std::vector<uint64_t> visited;
        visited.reserve(hex_bins.size());
        for (const auto &bin : hex_bins)
            if (bin.value)
                visited.push_back(bin.value);
        if (visited.empty())
            return;
        std::sort(visited.begin(), visited.end());
        const uint64_t first_quartile = visited[(visited.size() - 1) / 4];
        const uint64_t second_quartile = visited[(visited.size() - 1) / 2];
        const uint64_t third_quartile = visited[(visited.size() - 1) * 3 / 4];
        painter.save();
        painter.setClipRect(rect);
        for (const auto &bin : hex_bins) {
            if (!bin.value)
                continue;
            const int band = bin.value <= first_quartile    ? 0
                             : bin.value <= second_quartile ? 1
                             : bin.value <= third_quartile  ? 2
                                                            : 3;
            QColor color = heatmap_color(band);
            color.setAlpha(150);
            painter.setBrush(color);
            QColor outline = heatmap_color(band);
            outline.setAlpha(210);
            QPen pen(outline, 0.75);
            pen.setJoinStyle(Qt::RoundJoin);
            painter.setPen(pen);
            QPolygonF hexagon;
            for (int corner = 0; corner < 6; ++corner) {
                const qreal angle = (30.0 + corner * 60.0) * M_PI / 180.0;
                hexagon << QPointF(bin.center.x() + heatmap_hex_radius * std::cos(angle),
                                   bin.center.y() + heatmap_hex_radius * std::sin(angle));
            }
            painter.drawPolygon(hexagon);
        }
        painter.restore();
    }
    QColor heatmap_color(int band) const
    {
        if (heatmap_gradient == "lime") {
            const QColor colors[] = {{101, 163, 13}, {132, 204, 22}, {190, 242, 100}, {250, 204, 21}};
            return colors[band];
        }
        if (heatmap_gradient == "ocean") {
            const QColor colors[] = {{30, 64, 175}, {14, 116, 144}, {34, 197, 94}, {250, 204, 21}};
            return colors[band];
        }
        const QColor colors[] = {{59, 130, 246}, {6, 182, 212}, {250, 204, 21}, {239, 68, 68}};
        return colors[band];
    }
    void load_display()
    {
        unsigned char count{};
        std::unique_ptr<screen_data, decltype(&free)> screens(hook_create_screen_info(&count), &free);
        if (screens && display >= 0 && display < count)
            monitor = screens.get()[display];
        else
            monitor = {};
    }
    void draw_pointer(QPainter &painter) const
    {
        if (!coordinates || monitor.width == 0 || monitor.height == 0)
            return;
        const QRect heatmap = heatmap_rect();
        const QPointF point(heatmap.x() + coordinates->x() * heatmap.width() / monitor.width,
                            heatmap.y() + coordinates->y() * heatmap.height() / monitor.height);
        const auto is_pressed = [&](uint16_t button) {
            const auto found = buttons.find(button);
            return found != buttons.end() && found->second;
        };
        const uint16_t highlighted_button = is_pressed(MOUSE_BUTTON1)   ? MOUSE_BUTTON1
                                            : is_pressed(MOUSE_BUTTON2) ? MOUSE_BUTTON2
                                            : is_pressed(MOUSE_BUTTON3) ? MOUSE_BUTTON3
                                                                        : 0;
        const QColor color = highlighted_button ? button_colors.at(highlighted_button) : active_color;
        QPainterPath pointer;
        pointer.moveTo(point);
        pointer.lineTo(point + QPointF(0, 22));
        pointer.lineTo(point + QPointF(6, 16));
        pointer.lineTo(point + QPointF(11, 27));
        pointer.lineTo(point + QPointF(16, 25));
        pointer.lineTo(point + QPointF(11, 14));
        pointer.lineTo(point + QPointF(19, 14));
        pointer.closeSubpath();
        painter.setBrush(color);
        painter.setPen(QPen(text_color, 1.5));
        painter.drawPath(pointer);
        if (!highlighted_button)
            return;
        const QString &label = highlighted_button == MOUSE_BUTTON1   ? left_label
                               : highlighted_button == MOUSE_BUTTON2 ? right_label
                                                                     : middle_label;
        if (label.isEmpty())
            return;
        const QRect label_rect(static_cast<int>(point.x()) + 18, static_cast<int>(point.y()) + 12,
                               std::max(1, font_size * 3), font_size + 8);
        QColor label_background = color;
        label_background.setAlpha(220);
        painter.setBrush(label_background);
        painter.setPen(text_color);
        painter.drawRoundedRect(label_rect, 4, 4);
        painter.drawText(label_rect, Qt::AlignCenter, label);
    }
    QString left_label{"L"}, right_label{"R"}, middle_label{"M"};
    QColor active_color{37, 99, 235};
    std::unordered_map<uint16_t, QColor> button_colors{{MOUSE_BUTTON1, {37, 99, 235}},
                                                       {MOUSE_BUTTON2, {239, 68, 68}},
                                                       {MOUSE_BUTTON3, {250, 204, 21}}};
    bool show_coordinates{}, map_clicks{};
    std::string heatmap_gradient{"spectrum"};
    uint64_t trail_duration_ns{1500ULL * 1000 * 1000};
    int display{};
    screen_data monitor{};
    std::unordered_map<uint16_t, bool> buttons;
    std::deque<std::pair<uint64_t, QPoint>> trail;
    std::optional<QPoint> coordinates;
    std::optional<motion_point> last_motion;
    QRect heatmap_bounds;
    int hex_columns{}, hex_rows{};
    std::vector<hex_bin> hex_bins;
};

class statistics_source final : public activity_source {
public:
    statistics_source(obs_source_t *source, obs_data_t *settings) : activity_source(source, settings)
    {
        hotkey = obs_hotkey_register_source(
            source, "reset_input_statistics", obs_module_text("Statistics.ResetHotkey"),
            [](void *, obs_hotkey_id, obs_hotkey_t *, bool pressed) {
                if (pressed)
                    activity_source::reset_all_activity();
            },
            this);
    }
    ~statistics_source() override { obs_hotkey_unregister(hotkey); }
    void update(obs_data_t *settings) override
    {
        activity_source::update(settings);
        mouse_dpi = std::max<int64_t>(1, obs_data_get_int(settings, "statistics.mouse_dpi"));
    }
    void on_event(const input_data::trace_event &event) override
    {
        if (event.type == EVENT_KEY_PRESSED) {
            if (!held_keys[event.code]) {
                held_keys[event.code] = true;
                keys.push_back(event.time_ns);
                ++total_keys;
            }
        } else if (event.type == EVENT_KEY_RELEASED) {
            held_keys[event.code] = false;
        } else if (event.type == EVENT_MOUSE_PRESSED && event.code >= MOUSE_BUTTON1 && event.code <= MOUSE_BUTTON3) {
            if (!held_buttons[event.code]) {
                held_buttons[event.code] = true;
                clicks.push_back(event.time_ns);
                ++total_clicks;
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
        const QString text = QString("KPM: %1  Total keys: %2\nCPM: %3  Total clicks: %4\nAPM: %5  Total actions: %6\n"
                                     "Distance: %7 px (%8 in)")
                                 .arg(keys.size())
                                 .arg(total_keys)
                                 .arg(clicks.size())
                                 .arg(total_clicks)
                                 .arg(keys.size() + clicks.size())
                                 .arg(total_keys + total_clicks)
                                 .arg(distance, 0, 'f', 0)
                                 .arg(distance / mouse_dpi, 0, 'f', 2);
        painter.drawText(QRect(padding, padding, width - padding * 2, height - padding * 2),
                         Qt::AlignLeft | Qt::AlignVCenter, text);
    }
    void reset_activity() override
    {
        keys.clear();
        clicks.clear();
        distance = 0;
        total_keys = 0;
        total_clicks = 0;
        last_motion.reset();
    }

private:
    obs_hotkey_id hotkey = OBS_INVALID_HOTKEY_ID;
    std::deque<uint64_t> keys, clicks;
    std::unordered_map<uint16_t, bool> held_keys, held_buttons;
    double distance{};
    uint64_t total_keys{}, total_clicks{};
    int64_t mouse_dpi{800};
    std::optional<input_data::trace_event> last_motion;
};

enum class intensity_metric { keyboard, mouse, actions, key, button, velocity };

struct intensity_row {
    bool enabled{};
    intensity_metric metric{intensity_metric::actions};
    uint16_t key{};
    uint16_t button{MOUSE_BUTTON1};

    bool operator==(const intensity_row &other) const
    {
        return enabled == other.enabled && metric == other.metric && key == other.key && button == other.button;
    }
    bool operator!=(const intensity_row &other) const { return !(*this == other); }
};

class input_intensity_source final : public activity_source {
public:
    using sample = std::array<double, 8>;

    using activity_source::activity_source;

    void update(obs_data_t *settings) override
    {
        activity_source::update(settings);

        const int new_window =
            std::clamp(static_cast<int>(obs_data_get_int(settings, "input_intensity.window")), 1, 60);
        std::array<intensity_row, 8> new_rows{};
        for (size_t index = 0; index < new_rows.size(); ++index) {
            const std::string prefix = "input_intensity.row" + std::to_string(index) + ".";
            new_rows[index].enabled = obs_data_get_bool(settings, (prefix + "enabled").c_str());
            const std::string type = obs_data_get_string(settings, (prefix + "metric").c_str());
            if (type == "keyboard")
                new_rows[index].metric = intensity_metric::keyboard;
            else if (type == "mouse")
                new_rows[index].metric = intensity_metric::mouse;
            else if (type == "key")
                new_rows[index].metric = intensity_metric::key;
            else if (type == "button")
                new_rows[index].metric = intensity_metric::button;
            else if (type == "velocity")
                new_rows[index].metric = intensity_metric::velocity;
            else
                new_rows[index].metric = intensity_metric::actions;
            new_rows[index].key = static_cast<uint16_t>(obs_data_get_int(settings, (prefix + "key").c_str()));
            new_rows[index].button = static_cast<uint16_t>(obs_data_get_int(settings, (prefix + "button").c_str()));
        }
        const QColor new_color = obs_color(static_cast<uint32_t>(obs_data_get_int(settings, "input_intensity.color")));
        const bool data_changed =
            configured && (new_window != window_seconds || new_rows != rows || selected_source != configured_source);
        window_seconds = new_window;
        rows = new_rows;
        accent_color = new_color;
        configured_source = selected_source;
        if (data_changed)
            clear_samples();
        configured = true;
    }

    void on_event(const input_data::trace_event &event) override
    {
        advance_to(event.time_ns);
        if (event.type == EVENT_KEY_PRESSED) {
            if (!held_keys[event.code]) {
                held_keys[event.code] = true;
                for_each_matching([&](const intensity_row &row) {
                    return row.metric == intensity_metric::keyboard || row.metric == intensity_metric::actions ||
                           (row.metric == intensity_metric::key && row.key == event.code);
                });
            }
        } else if (event.type == EVENT_KEY_RELEASED) {
            held_keys[event.code] = false;
        } else if (event.type == EVENT_MOUSE_PRESSED && event.code >= MOUSE_BUTTON1 && event.code <= MOUSE_BUTTON5) {
            if (!held_buttons[event.code]) {
                held_buttons[event.code] = true;
                for_each_matching([&](const intensity_row &row) {
                    return row.metric == intensity_metric::mouse || row.metric == intensity_metric::actions ||
                           (row.metric == intensity_metric::button && row.button == event.code);
                });
            }
        } else if (event.type == EVENT_MOUSE_RELEASED) {
            held_buttons[event.code] = false;
        } else if (event.type == EVENT_MOUSE_MOVED || event.type == EVENT_MOUSE_DRAGGED) {
            if (last_motion) {
                const double distance = std::hypot(static_cast<double>(event.x - last_motion->x),
                                                   static_cast<double>(event.y - last_motion->y));
                for_each_matching([&](const intensity_row &row) { return row.metric == intensity_metric::velocity; },
                                  distance);
            }
            last_motion = event;
        }
    }

    void on_snapshot(const input_data::button_map<uint16_t> &keyboard,
                     const input_data::button_map<uint16_t> &mouse) override
    {
        held_keys = keyboard;
        held_buttons = mouse;
    }

    void tick(float seconds) override
    {
        activity_source::tick(seconds);
        advance_to(os_gettime_ns());
    }

    void reset_activity() override { clear_samples(); }

    void render(QPainter &painter) override
    {
        std::vector<size_t> active_rows;
        for (size_t index = 0; index < rows.size(); ++index)
            if (rows[index].enabled)
                active_rows.push_back(index);
        if (active_rows.empty()) {
            painter.setFont(font());
            painter.setPen(text_color);
            painter.drawText(QRect(padding, padding, width - padding * 2, height - padding * 2), Qt::AlignCenter,
                             obs_module_text("InputIntensity.NoMetrics"));
            return;
        }

        const QRect bounds(padding, padding, std::max(1, width - padding * 2), std::max(1, height - padding * 2));
        const int row_height = std::max(1, bounds.height() / static_cast<int>(active_rows.size()));
        QFont row_font = font();
        row_font.setPixelSize(std::clamp(row_height / 3, 9, font_size));
        painter.setFont(row_font);

        for (size_t visible_index = 0; visible_index < active_rows.size(); ++visible_index) {
            const size_t row_index = active_rows[visible_index];
            const QRect row_rect(bounds.left(), bounds.top() + static_cast<int>(visible_index) * row_height,
                                 bounds.width(), row_height);
            const int label_height = std::max(1, std::min(row_rect.height() / 3, row_font.pixelSize() + 2));
            const int value_label_height = std::max(1, std::min(row_rect.height() / 3, row_font.pixelSize() + 2));
            const QRect label_rect(row_rect.left(), row_rect.top(), row_rect.width(), label_height);
            const QRect chart_rect(row_rect.left(), label_rect.bottom() + 3, row_rect.width(),
                                   std::max(1, row_rect.height() - label_height - value_label_height - 4));
            const QRect value_label_rect(row_rect.left(), chart_rect.bottom() + 1, row_rect.width(),
                                         value_label_height + 1);
            painter.setPen(text_color);
            painter.drawText(label_rect, Qt::AlignLeft | Qt::AlignTop, row_label(rows[row_index]));
            draw_box_plot(painter, chart_rect, value_label_rect, row_index);
        }
    }

private:
    template<typename Predicate> void for_each_matching(Predicate predicate, double amount = 1.0)
    {
        for (size_t index = 0; index < rows.size(); ++index)
            if (rows[index].enabled && predicate(rows[index]))
                current[index] += amount;
    }

    void advance_to(uint64_t now)
    {
        if (now == 0)
            return;
        if (bucket_start_ns == 0) {
            bucket_start_ns = now;
            return;
        }
        if (now <= bucket_start_ns)
            return;
        while (now - bucket_start_ns >= second_ns) {
            samples.push_back(current);
            if (samples.size() > 60)
                samples.pop_front();
            current.fill(0.0);
            bucket_start_ns += second_ns;
        }
    }

    void clear_samples()
    {
        samples.clear();
        current.fill(0.0);
        bucket_start_ns = 0;
        held_keys.clear();
        held_buttons.clear();
        last_motion.reset();
    }

    QString row_label(const intensity_row &row) const
    {
        switch (row.metric) {
        case intensity_metric::keyboard:
            return QString("%1 (/s)").arg(obs_module_text("InputIntensity.Metric.Keyboard"));
        case intensity_metric::mouse:
            return QString("%1 (/s)").arg(obs_module_text("InputIntensity.Metric.Mouse"));
        case intensity_metric::actions:
            return QString("%1 (/s)").arg(obs_module_text("InputIntensity.Metric.Actions"));
        case intensity_metric::key: {
            input_data::trace_event event{};
            event.code = row.key;
            return QString("%1 (/s)").arg(key_name(event));
        }
        case intensity_metric::button:
            return QString("%1 %2 (/s)").arg(obs_module_text("InputIntensity.Metric.MouseButton")).arg(row.button);
        case intensity_metric::velocity:
            return QString("%1 (px/s)").arg(obs_module_text("InputIntensity.Metric.Velocity"));
        }
        return {};
    }

    void draw_box_plot(QPainter &painter, const QRect &rect, const QRect &value_label_rect, size_t row_index) const
    {
        std::vector<double> values;
        const size_t first = samples.size() > static_cast<size_t>(window_seconds)
                                 ? samples.size() - static_cast<size_t>(window_seconds)
                                 : 0;
        values.reserve(std::max<size_t>(1, samples.size() - first));
        for (size_t index = first; index < samples.size(); ++index)
            values.push_back(samples[index][row_index]);
        if (values.empty())
            values.push_back(0.0);
        std::sort(values.begin(), values.end());
        const double minimum = values.front();
        const double maximum = values.back();
        const double first_quartile = values[(values.size() - 1) / 4];
        const double median = values[(values.size() - 1) / 2];
        const double third_quartile = values[(values.size() - 1) * 3 / 4];
        const double current_value = current_rate(row_index);
        const double scale_minimum = std::min(minimum, current_value);
        const double scale_maximum = std::max(maximum, current_value);
        const double range = scale_maximum - scale_minimum;
        const auto position = [&](double value) {
            if (range == 0.0)
                return rect.center().x();
            return rect.left() + static_cast<int>(std::lround((value - scale_minimum) / range * (rect.width() - 1)));
        };
        const int min_x = position(minimum);
        const int max_x = position(maximum);
        const int q1_x = position(first_quartile);
        const int q3_x = position(third_quartile);
        const int median_x = position(median);
        const int current_x = position(current_value);
        const int center_y = rect.center().y();
        const int box_height = std::max(6, rect.height() / 2);
        const QRect box(std::min(q1_x, q3_x), center_y - box_height / 2, std::max(1, std::abs(q3_x - q1_x)),
                        box_height);

        QPen whisker(text_color, 1.0);
        painter.setPen(whisker);
        painter.setBrush(Qt::NoBrush);
        painter.drawLine(min_x, center_y, max_x, center_y);
        painter.drawLine(min_x, center_y - 3, min_x, center_y + 3);
        painter.drawLine(max_x, center_y - 3, max_x, center_y + 3);
        QColor fill = accent_color;
        fill.setAlpha(150);
        painter.setBrush(fill);
        painter.drawRect(box);
        QPen median_pen(text_color, 2.0);
        painter.setPen(median_pen);
        painter.drawLine(median_x, box.top(), median_x, box.bottom());

        QPen current_pen(accent_color, 2.0);
        painter.setPen(current_pen);
        painter.drawLine(current_x, rect.top(), current_x, rect.bottom());
        painter.setBrush(accent_color);
        painter.drawEllipse(QPoint(current_x, center_y), 3, 3);

        const QString min_label = number_label(minimum);
        const QString max_label = number_label(maximum);
        painter.setPen(text_color);
        painter.drawText(value_label_rect, Qt::AlignLeft | Qt::AlignVCenter, min_label);
        painter.drawText(value_label_rect, Qt::AlignRight | Qt::AlignVCenter, max_label);
    }

    double current_rate(size_t row_index) const
    {
        if (bucket_start_ns == 0)
            return 0.0;
        const uint64_t now = os_gettime_ns();
        if (now <= bucket_start_ns)
            return 0.0;
        const uint64_t elapsed_ns = std::min(second_ns, now - bucket_start_ns);
        if (elapsed_ns == 0)
            return 0.0;
        return current[row_index] * static_cast<double>(second_ns) / static_cast<double>(elapsed_ns);
    }

    static QString number_label(double value) { return QString::number(value, 'f', value < 10.0 ? 1 : 0); }

    static constexpr uint64_t second_ns = 1000ULL * 1000 * 1000;
    int window_seconds{30};
    std::array<intensity_row, 8> rows{};
    std::deque<sample> samples;
    sample current{};
    std::unordered_map<uint16_t, bool> held_keys, held_buttons;
    std::optional<input_data::trace_event> last_motion;
    QColor accent_color{37, 99, 235};
    uint64_t bucket_start_ns{};
    std::string configured_source;
    bool configured{};
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
    else if constexpr (std::is_same_v<T, input_intensity_source>)
        info.get_name = [](void *) { return obs_module_text("InputIntensity"); };
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
            obs_data_set_default_bool(settings, "live_keys.row_layout", false);
            obs_data_set_default_int(settings, "live_keys.fade_ms", 300);
            obs_data_set_default_int(settings, "live_keys.color", 0xeb6325);
        } else if constexpr (std::is_same_v<T, mouse_activity_source>) {
            obs_data_set_default_string(settings, "mouse_activity.left_label", "LMB");
            obs_data_set_default_string(settings, "mouse_activity.right_label", "RMB");
            obs_data_set_default_bool(settings, "mouse_activity.show_middle_button", true);
            obs_data_set_default_bool(settings, "mouse_activity.show_coordinates", false);
            obs_data_set_default_int(settings, "mouse_activity.button_height", 48);
            obs_data_set_default_int(settings, "mouse_activity.trail_ms", 1500);
            obs_data_set_default_string(settings, "mouse_activity.heatmap_gradient", "spectrum");
            obs_data_set_default_int(settings, "mouse_activity.color", 0xeb6325);
        } else if constexpr (std::is_same_v<T, statistics_source>) {
            obs_data_set_default_int(settings, "statistics.mouse_dpi", 800);
        } else if constexpr (std::is_same_v<T, input_intensity_source>) {
            obs_data_set_default_int(settings, "input_intensity.window", 30);
            obs_data_set_default_int(settings, "input_intensity.color", 0xeb6325);
            for (size_t index = 0; index < 8; ++index) {
                const std::string prefix = "input_intensity.row" + std::to_string(index) + ".";
                obs_data_set_default_bool(settings, (prefix + "enabled").c_str(), index == 0);
                obs_data_set_default_string(settings, (prefix + "metric").c_str(), "actions");
                obs_data_set_default_int(settings, (prefix + "key").c_str(), VC_SPACE);
                obs_data_set_default_int(settings, (prefix + "button").c_str(), MOUSE_BUTTON1);
            }
        }
    };
    obs_register_source(&info);
}
obs_properties_t *keys_properties(void *)
{
    auto *p = obs_properties_create();
    add_common_properties(p);
    obs_properties_add_int(p, "live_keys.maximum", obs_module_text("LiveKeys.Maximum"), 1, 64, 1);
    obs_properties_add_bool(p, "live_keys.row_layout", obs_module_text("LiveKeys.RowLayout"));
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
    obs_properties_add_bool(p, "mouse_activity.show_middle_button", obs_module_text("MouseActivity.ShowMiddle"));
    obs_properties_add_bool(p, "mouse_activity.show_coordinates", obs_module_text("MouseActivity.ShowCoordinates"));
    obs_properties_add_int_slider(p, "mouse_activity.button_height", obs_module_text("MouseActivity.ButtonHeight"),
                                  24, 240, 1);
    obs_properties_add_int_slider(p, "mouse_activity.trail_ms", obs_module_text("MouseActivity.TrailDuration"), 100,
                                  10000, 50);
    auto *gradient = obs_properties_add_list(p, "mouse_activity.heatmap_gradient",
                                             obs_module_text("MouseActivity.HeatmapGradient"), OBS_COMBO_TYPE_LIST,
                                             OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(gradient, obs_module_text("MouseActivity.HeatmapGradient.Spectrum"), "spectrum");
    obs_property_list_add_string(gradient, obs_module_text("MouseActivity.HeatmapGradient.Lime"), "lime");
    obs_property_list_add_string(gradient, obs_module_text("MouseActivity.HeatmapGradient.Ocean"), "ocean");
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
    obs_properties_add_int(p, "statistics.mouse_dpi", obs_module_text("Statistics.MouseDPI"), 1, 100000, 1);
    return p;
}

void add_intensity_key_list(obs_property_t *list)
{
    for (uint16_t code = VC_A; code <= VC_Z; ++code) {
        input_data::trace_event event{};
        event.code = code;
        const QByteArray label = key_name(event).toUtf8();
        obs_property_list_add_int(list, label.constData(), code);
    }
    for (uint16_t code = VC_0; code <= VC_9; ++code) {
        input_data::trace_event event{};
        event.code = code;
        const QByteArray label = key_name(event).toUtf8();
        obs_property_list_add_int(list, label.constData(), code);
    }
    const uint16_t common_codes[] = {
        VC_SPACE,      VC_ENTER,     VC_ESCAPE, VC_TAB,    VC_BACKSPACE, VC_SHIFT_L, VC_CONTROL_L,    VC_ALT_L,
        VC_META_L,     VC_CAPS_LOCK, VC_UP,     VC_DOWN,   VC_LEFT,      VC_RIGHT,   VC_HOME,         VC_END,
        VC_PAGE_UP,    VC_PAGE_DOWN, VC_INSERT, VC_DELETE, VC_MINUS,     VC_EQUALS,  VC_OPEN_BRACKET, VC_CLOSE_BRACKET,
        VC_BACK_SLASH, VC_SEMICOLON, VC_QUOTE,  VC_COMMA,  VC_PERIOD,    VC_SLASH,   VC_BACK_QUOTE};
    for (const uint16_t code : common_codes) {
        input_data::trace_event event{};
        event.code = code;
        const QByteArray label = key_name(event).toUtf8();
        obs_property_list_add_int(list, label.constData(), code);
    }
    for (uint16_t code = VC_F1; code <= VC_F12; ++code) {
        input_data::trace_event event{};
        event.code = code;
        const QByteArray label = key_name(event).toUtf8();
        obs_property_list_add_int(list, label.constData(), code);
    }
    for (uint16_t code = VC_F13; code <= VC_F24; ++code) {
        input_data::trace_event event{};
        event.code = code;
        const QByteArray label = key_name(event).toUtf8();
        obs_property_list_add_int(list, label.constData(), code);
    }
    for (uint16_t code = VC_KP_0; code <= VC_KP_9; ++code) {
        input_data::trace_event event{};
        event.code = code;
        const QByteArray label = key_name(event).toUtf8();
        obs_property_list_add_int(list, label.constData(), code);
    }
    const uint16_t keypad_codes[] = {VC_KP_DIVIDE, VC_KP_MULTIPLY, VC_KP_SUBTRACT,
                                     VC_KP_ADD,    VC_KP_DECIMAL,  VC_KP_ENTER};
    for (const uint16_t code : keypad_codes) {
        input_data::trace_event event{};
        event.code = code;
        const QByteArray label = key_name(event).toUtf8();
        obs_property_list_add_int(list, label.constData(), code);
    }
}

obs_properties_t *intensity_properties(void *)
{
    auto *p = obs_properties_create();
    add_common_properties(p);
    obs_properties_add_int_slider(p, "input_intensity.window", obs_module_text("InputIntensity.Window"), 1, 60, 1);
    obs_properties_add_color(p, "input_intensity.color", obs_module_text("InputIntensity.Color"));
    for (size_t index = 0; index < 8; ++index) {
        const std::string prefix = "input_intensity.row" + std::to_string(index) + ".";
        const QByteArray row_label =
            QString("%1 %2").arg(obs_module_text("InputIntensity.Row")).arg(index + 1).toUtf8();
        obs_properties_add_bool(p, (prefix + "enabled").c_str(), row_label.constData());
        auto *metric = obs_properties_add_list(p, (prefix + "metric").c_str(), obs_module_text("InputIntensity.Metric"),
                                               OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
        obs_property_list_add_string(metric, obs_module_text("InputIntensity.Metric.Actions"), "actions");
        obs_property_list_add_string(metric, obs_module_text("InputIntensity.Metric.Keyboard"), "keyboard");
        obs_property_list_add_string(metric, obs_module_text("InputIntensity.Metric.Mouse"), "mouse");
        obs_property_list_add_string(metric, obs_module_text("InputIntensity.Metric.Key"), "key");
        obs_property_list_add_string(metric, obs_module_text("InputIntensity.Metric.MouseButton"), "button");
        obs_property_list_add_string(metric, obs_module_text("InputIntensity.Metric.Velocity"), "velocity");
        auto *key = obs_properties_add_list(p, (prefix + "key").c_str(), obs_module_text("InputIntensity.Key"),
                                            OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        add_intensity_key_list(key);
        auto *button = obs_properties_add_list(p, (prefix + "button").c_str(), obs_module_text("InputIntensity.Button"),
                                               OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
        for (uint16_t code = MOUSE_BUTTON1; code <= MOUSE_BUTTON5; ++code) {
            const QByteArray label =
                QString("%1 %2").arg(obs_module_text("InputIntensity.Metric.MouseButton")).arg(code).toUtf8();
            obs_property_list_add_int(button, label.constData(), code);
        }
    }
    return p;
}
} // namespace

void register_activity_sources()
{
    register_source<live_keys_source>("input-overlay-live-keys", "LiveKeys", keys_properties);
    register_source<mouse_activity_source>("input-overlay-mouse-activity", "MouseActivity", mouse_properties);
    register_source<statistics_source>("input-overlay-statistics", "InputStatistics", statistics_properties);
    register_source<input_intensity_source>("input-overlay-input-intensity", "InputIntensity", intensity_properties);
}
} // namespace sources
