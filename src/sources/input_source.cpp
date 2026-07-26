/*************************************************************************
 * This file is part of input-overlay
 * git.vrsal.cc/alex/input-overlay
 * Copyright 2025 univrsal <uni@vrsal.xyz>.
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

#include "input_source.hpp"
#include "../hook/gamepad_hook_helper.hpp"
#include "../util/lang.h"
#include "../util/obs_util.hpp"
#include "../util/settings.h"
#include "../util/config.hpp"
#include "../network/websocket_server.hpp"
#include "../network/remote_connection.hpp"
#include <QFile>
#include <QColor>
#include <QFont>
#include <QImage>
#include <QPainter>
#include <obs-frontend-api.h>

extern "C" {
#include <graphics/graphics.h>
}

#include <cstring>

namespace sources {
namespace {
struct bundled_preset {
    const char *name;
    const char *id;
    const char *layout;
    const char *texture;
};

constexpr bundled_preset bundled_presets[] = {
    {"WASD full keyboard", "wasd-full", "wasd/wasd-full.json", "wasd/wasd.png"},
    {"WASD + arrow keys", "wasd-arrow-keys", "wasd/wasd-plus-arrow-keys.json", "wasd/wasd-plus-arrow-keys.png"},
    {"WASD minimal", "wasd-minimal", "wasd/wasd-minimal.json", "wasd/wasd.png"},
    {"QWERTY keyboard", "qwerty", "qwerty/qwerty.json", "qwerty/qwerty.png"},
    {"QWERTY + arrow keys", "qwerty-arrow-keys", "qwerty_arrow_keys/qwerty_arrow_keys.json",
     "qwerty_arrow_keys/qwerty_arrow_keys.png"},
    {"Mouse", "mouse", "mouse/mouse-dot.json", "mouse/mouse.png"},
    {"Xbox controller", "xbox-controller", "xbox-controller/xbox-controller.json",
     "xbox-controller/xbox-controller.png"},
    {"DualSense controller", "dualsense", "dualsense/dualsense.json", "dualsense/dualsense.png"},
};

QColor color_from_obs(uint32_t color)
{
    return QColor(static_cast<int>(color & 0xff), static_cast<int>((color >> 8) & 0xff),
                  static_cast<int>((color >> 16) & 0xff), static_cast<int>((color >> 24) & 0xff));
}

void migrate_legacy_colors(obs_data_t *settings)
{
    if (obs_data_get_bool(settings, "io.colors_with_alpha"))
        return;
    const char *color_keys[] = {S_PROCEDURAL_FILL_COLOR,         S_PROCEDURAL_PRESSED_COLOR,
                                S_PROCEDURAL_BORDER_COLOR,       S_PROCEDURAL_TEXT_COLOR,
                                S_PROCEDURAL_MOUSE_FILL_COLOR,   S_PROCEDURAL_MOUSE_PRESSED_COLOR,
                                S_PROCEDURAL_MOUSE_BORDER_COLOR, S_PROCEDURAL_MOUSE_TEXT_COLOR};
    for (const char *color_key : color_keys) {
        const uint32_t color = static_cast<uint32_t>(obs_data_get_int(settings, color_key));
        obs_data_set_int(settings, color_key, color | 0xff000000);
    }
    obs_data_set_bool(settings, "io.colors_with_alpha", true);
}

const bundled_preset *find_bundled_preset(const char *id)
{
    for (const auto &preset : bundled_presets) {
        if (strcmp(preset.id, id) == 0)
            return &preset;
    }
    return nullptr;
}

std::string bundled_preset_path(const char *relative_path)
{
    const std::string resource_path = std::string("presets/") + relative_path;
    char *path = obs_module_file(resource_path.c_str());
    if (!path)
        return {};
    std::string result(path);
    bfree(path);
    return result;
}

bool preset_changed(void *d, obs_properties_t *props, obs_property_t *, obs_data_t *data)
{
    const auto *preset = find_bundled_preset(obs_data_get_string(data, S_BUNDLED_PRESET));
    if (!preset)
        return false;

    const std::string layout_path = bundled_preset_path(preset->layout);
    const std::string texture_path = bundled_preset_path(preset->texture);
    if (layout_path.empty() || texture_path.empty())
        return false;

    obs_data_set_string(data, S_LAYOUT_FILE, layout_path.c_str());
    obs_data_set_string(data, S_OVERLAY_FILE, texture_path.c_str());
    return file_changed(d, props, nullptr, data);
}
} // namespace

class procedural_keyboard {
public:
    ~procedural_keyboard()
    {
        if (m_texture) {
            obs_enter_graphics();
            gs_texture_destroy(m_texture);
            obs_leave_graphics();
        }
    }

    void update(obs_data_t *settings)
    {
        m_enabled = obs_data_get_bool(settings, S_PROCEDURAL_ENABLED);
        m_shape = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_SHAPE));
        m_key_size = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_KEY_SIZE));
        if (!obs_data_has_user_value(settings, S_PROCEDURAL_KEY_WIDTH) &&
            obs_data_has_user_value(settings, S_PROCEDURAL_KEY_SIZE))
            obs_data_set_int(settings, S_PROCEDURAL_KEY_WIDTH, m_key_size);
        if (!obs_data_has_user_value(settings, S_PROCEDURAL_KEY_HEIGHT) &&
            obs_data_has_user_value(settings, S_PROCEDURAL_KEY_SIZE))
            obs_data_set_int(settings, S_PROCEDURAL_KEY_HEIGHT, m_key_size);
        m_key_width = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_KEY_WIDTH));
        m_key_height = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_KEY_HEIGHT));
        m_gap = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_GAP));
        m_radius = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_RADIUS));
        m_font_size = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_FONT_SIZE));
        if (auto *font = obs_data_get_obj(settings, S_PROCEDURAL_FONT)) {
            m_font_family = QString::fromUtf8(obs_data_get_string(font, "face"));
            m_font_style = QString::fromUtf8(obs_data_get_string(font, "style"));
            m_font_flags = static_cast<uint32_t>(obs_data_get_int(font, "flags"));
            m_has_font_selection = true;
            obs_data_release(font);
        } else {
            m_has_font_selection = false;
        }
        m_fill = color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_FILL_COLOR)));
        m_pressed = color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_PRESSED_COLOR)));
        m_border = color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_BORDER_COLOR)));
        m_text = color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_TEXT_COLOR)));
        parse_layout(QString::fromUtf8(obs_data_get_string(settings, S_PROCEDURAL_LAYOUT)));
    }

    bool enabled() const { return m_enabled; }
    uint32_t width() const { return static_cast<uint32_t>(m_columns * m_key_width + (m_columns - 1) * m_gap); }
    uint32_t height() const { return static_cast<uint32_t>(m_rows * m_key_height + (m_rows - 1) * m_gap); }

    void draw(gs_effect_t *effect, const overlay_settings &settings)
    {
        render_image(settings);
        if (m_texture && (m_texture_width != width() || m_texture_height != height())) {
            gs_texture_destroy(m_texture);
            m_texture = nullptr;
        }
        if (!m_texture) {
            m_texture = gs_texture_create(width(), height(), GS_RGBA, 1, nullptr, GS_DYNAMIC);
            m_texture_width = width();
            m_texture_height = height();
        }
        if (!m_texture)
            return;

        gs_texture_set_image(m_texture, m_image.constBits(), static_cast<uint32_t>(m_image.bytesPerLine()), false);
        gs_blend_state_push();
        gs_enable_blending(true);
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), m_texture);
        gs_draw_sprite(m_texture, 0, width(), height());
        gs_blend_state_pop();
    }

private:
    struct key {
        QString label;
        uint16_t code;
        int column;
        int row;
    };

    static uint16_t keycode_for_token(const QString &token)
    {
        const QString key = token.toUpper();
        if (key.size() == 1 && key[0].isLetter())
            return static_cast<uint16_t>(VC_A + key[0].unicode() - QChar('A').unicode());
        if (key.size() == 1 && key[0].isDigit())
            return static_cast<uint16_t>(VC_0 + key[0].unicode() - QChar('0').unicode());

        if (key.size() >= 2 && key[0] == 'F') {
            bool valid = false;
            const int number = key.mid(1).toInt(&valid);
            if (valid && number >= 1 && number <= 12)
                return static_cast<uint16_t>(VC_F1 + number - 1);
            if (valid && number >= 13 && number <= 24)
                return static_cast<uint16_t>(VC_F13 + number - 13);
        }

        if (key == "SPACE")
            return VC_SPACE;
        if (key == "SHIFT" || key == "LSHIFT")
            return VC_SHIFT_L;
        if (key == "RSHIFT")
            return VC_SHIFT_R;
        if (key == "CTRL" || key == "CONTROL" || key == "LCTRL" || key == "LCONTROL")
            return VC_CONTROL_L;
        if (key == "RCTRL" || key == "RCONTROL")
            return VC_CONTROL_R;
        if (key == "ALT" || key == "OPTION" || key == "OPT" || key == "LALT" || key == "LOPTION")
            return VC_ALT_L;
        if (key == "RALT" || key == "ROPTION")
            return VC_ALT_R;
        if (key == "COMMAND" || key == "CMD" || key == "META" || key == "LCOMMAND" || key == "LCMD")
            return VC_META_L;
        if (key == "RCOMMAND" || key == "RCMD")
            return VC_META_R;
        if (key == "FN" || key == "FUNCTION")
            return VC_FUNCTION;
        if (key == "CAPS" || key == "CAPSLOCK")
            return VC_CAPS_LOCK;
        if (key == "TAB")
            return VC_TAB;
        if (key == "ENTER" || key == "RETURN")
            return VC_ENTER;
        if (key == "ESC" || key == "ESCAPE")
            return VC_ESCAPE;
        if (key == "BACKSPACE" || key == "BKSP")
            return VC_BACKSPACE;
        if (key == "DELETE" || key == "FORWARDDELETE")
            return VC_DELETE;
        if (key == "INSERT")
            return VC_INSERT;
        if (key == "HOME")
            return VC_HOME;
        if (key == "END")
            return VC_END;
        if (key == "PAGEUP" || key == "PGUP")
            return VC_PAGE_UP;
        if (key == "PAGEDOWN" || key == "PGDN")
            return VC_PAGE_DOWN;
        if (key == "UP" || key == "↑")
            return VC_UP;
        if (key == "DOWN" || key == "↓")
            return VC_DOWN;
        if (key == "LEFT" || key == "←")
            return VC_LEFT;
        if (key == "RIGHT" || key == "→")
            return VC_RIGHT;
        if (key == "BACKTICK" || key == "GRAVE")
            return VC_BACK_QUOTE;
        if (key == "MINUS" || key == "-")
            return VC_MINUS;
        if (key == "EQUALS" || key == "=")
            return VC_EQUALS;
        if (key == "OPENBRACKET" || key == "[")
            return VC_OPEN_BRACKET;
        if (key == "CLOSEBRACKET" || key == "]")
            return VC_CLOSE_BRACKET;
        if (key == "BACKSLASH" || key == "\\")
            return VC_BACK_SLASH;
        if (key == "SEMICOLON" || key == ";")
            return VC_SEMICOLON;
        if (key == "QUOTE" || key == "'")
            return VC_QUOTE;
        if (key == "COMMA" || key == ",")
            return VC_COMMA;
        if (key == "PERIOD" || key == ".")
            return VC_PERIOD;
        if (key == "SLASH" || key == "/")
            return VC_SLASH;
        if (key == "PRINTSCREEN" || key == "PRTSC")
            return VC_PRINT_SCREEN;
        if (key == "SCROLLLOCK")
            return VC_SCROLL_LOCK;
        if (key == "PAUSE")
            return VC_PAUSE;
        if (key == "NUMLOCK")
            return VC_NUM_LOCK;
        return VC_UNDEFINED;
    }

    void parse_layout(const QString &layout)
    {
        m_keys.clear();
        const QString effective_layout = layout.trimmed().isEmpty() ? " W     \nASD[LEFT][DOWN][RIGHT]" : layout;
        const QStringList rows = effective_layout.split('\n', Qt::KeepEmptyParts);
        m_rows = std::max(1, static_cast<int>(rows.size()));
        m_columns = 1;
        for (int row = 0; row < rows.size(); row++) {
            int column = 0;
            for (int index = 0; index < rows[row].size(); index++, column++) {
                QString token = rows[row][index];
                if (token == "[") {
                    const int close = rows[row].indexOf(']', index);
                    if (close != -1) {
                        token = rows[row].mid(index + 1, close - index - 1);
                        index = close;
                    }
                }
                if (token == " " || token == "_")
                    continue;

                const QStringList parts = token.split('|');
                const uint16_t code = keycode_for_token(parts[0].trimmed());
                if (code != VC_UNDEFINED)
                    m_keys.push_back({parts.size() > 1 ? parts.mid(1).join("|").trimmed() : parts[0].toUpper(), code,
                                      column, row});
            }
            m_columns = std::max(m_columns, column);
        }
    }

    void render_image(const overlay_settings &settings)
    {
        m_image = QImage(static_cast<int>(width()), static_cast<int>(height()), QImage::Format_RGBA8888);
        m_image.fill(Qt::transparent);

        QPainter painter(&m_image);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont font = painter.font();
        if (m_has_font_selection) {
            font.setFamily(m_font_family);
            font.setStyleName(m_font_style);
            font.setBold(m_font_flags & OBS_FONT_BOLD);
            font.setItalic(m_font_flags & OBS_FONT_ITALIC);
            font.setUnderline(m_font_flags & OBS_FONT_UNDERLINE);
            font.setStrikeOut(m_font_flags & OBS_FONT_STRIKEOUT);
        } else {
            font.setBold(true);
        }
        font.setPixelSize(std::min(m_font_size, std::min(m_key_width, m_key_height) - 12));
        painter.setFont(font);
        painter.setPen(QPen(m_border, 2));

        for (const auto &key : m_keys) {
            const QRect rect(key.column * (m_key_width + m_gap), key.row * (m_key_height + m_gap), m_key_width,
                             m_key_height);
            const auto pressed = settings.data.keyboard.find(key.code);
            painter.setBrush(pressed != settings.data.keyboard.end() && pressed->second ? m_pressed : m_fill);
            const int radius = m_shape == 0 ? 0 : (m_shape == 2 ? std::min(m_key_width, m_key_height) / 2 : m_radius);
            painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), radius, radius);
            painter.setPen(m_text);
            QFont key_font = font;
            while (key_font.pixelSize() > 8 && QFontMetrics(key_font).horizontalAdvance(key.label) > rect.width() - 12)
                key_font.setPixelSize(key_font.pixelSize() - 1);
            painter.setFont(key_font);
            painter.drawText(rect, Qt::AlignCenter, key.label);
            painter.setFont(font);
            painter.setPen(QPen(m_border, 2));
        }
    }

    bool m_enabled = false;
    int m_shape = 1;
    int m_key_size = 80;
    int m_key_width = 80;
    int m_key_height = 80;
    int m_gap = 8;
    int m_radius = 12;
    int m_font_size = 28;
    QString m_font_family;
    QString m_font_style;
    uint32_t m_font_flags = 0;
    bool m_has_font_selection = false;
    QColor m_fill = QColor(35, 41, 57);
    QColor m_pressed = QColor(37, 99, 235);
    QColor m_border = QColor(148, 163, 184);
    QColor m_text = QColor(255, 255, 255);
    std::vector<key> m_keys;
    int m_columns = 7;
    int m_rows = 2;
    QImage m_image;
    gs_texture_t *m_texture = nullptr;
    uint32_t m_texture_width = 0;
    uint32_t m_texture_height = 0;
};

class procedural_mouse {
public:
    ~procedural_mouse()
    {
        if (m_texture) {
            obs_enter_graphics();
            gs_texture_destroy(m_texture);
            obs_leave_graphics();
        }
    }

    void update(obs_data_t *settings)
    {
        m_enabled = obs_data_get_bool(settings, S_PROCEDURAL_MOUSE_ENABLED);
        m_width = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_WIDTH));
        m_height = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_HEIGHT));
        m_gap = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_GAP));
        m_radius = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_RADIUS));
        m_font_size = static_cast<int>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_FONT_SIZE));
        if (auto *font = obs_data_get_obj(settings, S_PROCEDURAL_MOUSE_FONT)) {
            m_font_family = QString::fromUtf8(obs_data_get_string(font, "face"));
            m_font_style = QString::fromUtf8(obs_data_get_string(font, "style"));
            m_font_flags = static_cast<uint32_t>(obs_data_get_int(font, "flags"));
            m_has_font_selection = true;
            obs_data_release(font);
        } else {
            m_has_font_selection = false;
        }
        m_fill = color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_FILL_COLOR)));
        m_pressed =
            color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_PRESSED_COLOR)));
        m_border = color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_BORDER_COLOR)));
        m_text = color_from_obs(static_cast<uint32_t>(obs_data_get_int(settings, S_PROCEDURAL_MOUSE_TEXT_COLOR)));
        m_left_label = QString::fromUtf8(obs_data_get_string(settings, S_PROCEDURAL_MOUSE_LEFT_LABEL));
        m_right_label = QString::fromUtf8(obs_data_get_string(settings, S_PROCEDURAL_MOUSE_RIGHT_LABEL));
        m_middle_label = QString::fromUtf8(obs_data_get_string(settings, S_PROCEDURAL_MOUSE_MIDDLE_LABEL));
    }

    bool enabled() const { return m_enabled; }
    uint32_t width() const { return static_cast<uint32_t>(m_width); }
    uint32_t height() const { return static_cast<uint32_t>(m_height); }

    void draw(gs_effect_t *effect, const overlay_settings &settings)
    {
        render_image(settings);
        if (m_texture && (m_texture_width != width() || m_texture_height != height())) {
            gs_texture_destroy(m_texture);
            m_texture = nullptr;
        }
        if (!m_texture) {
            m_texture = gs_texture_create(width(), height(), GS_RGBA, 1, nullptr, GS_DYNAMIC);
            m_texture_width = width();
            m_texture_height = height();
        }
        if (!m_texture)
            return;

        gs_texture_set_image(m_texture, m_image.constBits(), static_cast<uint32_t>(m_image.bytesPerLine()), false);
        gs_blend_state_push();
        gs_enable_blending(true);
        gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
        gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), m_texture);
        gs_draw_sprite(m_texture, 0, width(), height());
        gs_blend_state_pop();
    }

private:
    static bool mouse_pressed(const overlay_settings &settings, uint16_t button)
    {
        const auto pressed = settings.data.mouse.find(button);
        return pressed != settings.data.mouse.end() && pressed->second;
    }

    void draw_button(QPainter &painter, const QRect &rect, const QString &label, bool pressed, const QFont &font) const
    {
        painter.setBrush(pressed ? m_pressed : m_fill);
        painter.setPen(QPen(m_border, 2));
        painter.drawRoundedRect(rect.adjusted(1, 1, -1, -1), std::min(m_radius, std::min(rect.width(), rect.height()) / 2),
                                std::min(m_radius, std::min(rect.width(), rect.height()) / 2));

        QFont button_font = font;
        while (button_font.pixelSize() > 8 && QFontMetrics(button_font).horizontalAdvance(label) > rect.width() - 12)
            button_font.setPixelSize(button_font.pixelSize() - 1);
        painter.setFont(button_font);
        painter.setPen(m_text);
        painter.drawText(rect, Qt::AlignCenter, label);
    }

    void render_image(const overlay_settings &settings)
    {
        m_image = QImage(m_width, m_height, QImage::Format_RGBA8888);
        m_image.fill(Qt::transparent);

        QPainter painter(&m_image);
        painter.setRenderHint(QPainter::Antialiasing);
        QFont font = painter.font();
        if (m_has_font_selection) {
            font.setFamily(m_font_family);
            font.setStyleName(m_font_style);
            font.setBold(m_font_flags & OBS_FONT_BOLD);
            font.setItalic(m_font_flags & OBS_FONT_ITALIC);
            font.setUnderline(m_font_flags & OBS_FONT_UNDERLINE);
            font.setStrikeOut(m_font_flags & OBS_FONT_STRIKEOUT);
        } else {
            font.setBold(true);
        }
        font.setPixelSize(std::min(m_font_size, std::min(m_width, m_height) / 4));

        const int top_height = std::max(m_height / 2, m_gap * 2 + 1);
        const int button_width = std::max((m_width - 3 * m_gap) / 2, 1);
        const QRect left(m_gap, m_gap, button_width, top_height - 2 * m_gap);
        const QRect right(m_gap * 2 + button_width, m_gap, button_width, top_height - 2 * m_gap);
        const int middle_width = std::max(m_width / 3, 1);
        const int middle_height = std::max(m_height - top_height - 2 * m_gap, 1);
        const QRect middle((m_width - middle_width) / 2, top_height + m_gap, middle_width, middle_height);

        draw_button(painter, left, m_left_label, mouse_pressed(settings, MOUSE_BUTTON1), font);
        draw_button(painter, right, m_right_label, mouse_pressed(settings, MOUSE_BUTTON2), font);
        draw_button(painter, middle, m_middle_label, mouse_pressed(settings, MOUSE_BUTTON3), font);
    }

    bool m_enabled = false;
    int m_width = 220;
    int m_height = 300;
    int m_gap = 8;
    int m_radius = 16;
    int m_font_size = 24;
    QString m_font_family;
    QString m_font_style;
    uint32_t m_font_flags = 0;
    bool m_has_font_selection = false;
    QColor m_fill = QColor(35, 41, 57);
    QColor m_pressed = QColor(37, 99, 235);
    QColor m_border = QColor(148, 163, 184);
    QColor m_text = QColor(255, 255, 255);
    QString m_left_label = "LMB";
    QString m_right_label = "RMB";
    QString m_middle_label = "MMB";
    QImage m_image;
    gs_texture_t *m_texture = nullptr;
    uint32_t m_texture_width = 0;
    uint32_t m_texture_height = 0;
};

bool overlay_settings::use_local_input()
{
    return selected_source.empty() || selected_source == T_LOCAL_SOURCE;
}

input_source::input_source(obs_source_t *source, obs_data_t *settings) : m_source(source)
{
    m_overlay = std::make_unique<overlay>(&m_settings);
    m_procedural_keyboard = std::make_unique<procedural_keyboard>();
    m_procedural_mouse = std::make_unique<procedural_mouse>();
    obs_source_update(m_source, settings);
    m_settings.image_file = obs_data_get_string(settings, S_OVERLAY_FILE);
    m_settings.layout_file = obs_data_get_string(settings, S_LAYOUT_FILE);
    m_settings.linear_alpha = obs_data_get_bool(settings, S_LINEAR_ALPHA);
    m_overlay->load();

    // Fix sources that used to use a remote connection but can't now because
    // remote connections are disabled
    if (!wss::state) {
        m_settings.selected_source = T_LOCAL_SOURCE;
        obs_data_set_string(settings, S_INPUT_SOURCE, T_LOCAL_SOURCE);
    }
}

input_source::~input_source() = default;

inline void input_source::update(obs_data_t *settings)
{
    migrate_legacy_colors(settings);
    m_procedural_keyboard->update(settings);
    m_procedural_mouse->update(settings);
    if (m_procedural_mouse->enabled()) {
        m_settings.cx = m_procedural_mouse->width();
        m_settings.cy = m_procedural_mouse->height();
    } else if (m_procedural_keyboard->enabled()) {
        m_settings.cx = m_procedural_keyboard->width();
        m_settings.cy = m_procedural_keyboard->height();
    }

    m_settings.selected_source = obs_data_get_string(settings, S_INPUT_SOURCE);

    m_settings.gamepad_name = obs_data_get_string(settings, S_CONTROLLER_ID);

    if (m_settings.use_local_input() && gamepad_hook::state && gamepad_hook::local_gamepads) {
        m_settings.gamepad = gamepad_hook::local_gamepads->get_controller_from_name(m_settings.gamepad_name);
    } else if (wss::state) {
        std::lock_guard<std::mutex> lock(network::remote_data_map_mutex);
        auto data = network::remote_data.find(m_settings.selected_source);
        if (data != network::remote_data.end())
            m_settings.remote_input_data = data->second;
    }

    m_settings.mouse_sens = std::max<uint16_t>(static_cast<uint16_t>(obs_data_get_int(settings, S_MOUSE_SENS)), 1);

    if ((m_settings.use_center = obs_data_get_bool(settings, S_MONITOR_USE_CENTER))) {
        m_settings.monitor_h = static_cast<uint32_t>(obs_data_get_int(settings, S_MONITOR_H_CENTER));
        m_settings.monitor_w = static_cast<uint32_t>(obs_data_get_int(settings, S_MONITOR_V_CENTER));
        m_settings.mouse_deadzone = static_cast<uint8_t>(obs_data_get_int(settings, S_MOUSE_DEAD_ZONE));
    }
}

inline void input_source::tick(float seconds)
{
    if (m_procedural_keyboard->enabled() || m_procedural_mouse->enabled())
        m_overlay->refresh_data();

    if (m_overlay->is_loaded()) {
        m_overlay->refresh_data();
        m_overlay->tick(seconds);
    }

    m_settings.input_source_check_timer += seconds;
    if (m_settings.input_source_check_timer >= 1) {
        if (m_settings.use_local_input() && gamepad_hook::state) {
            if (!m_settings.gamepad || !m_settings.gamepad->valid())
                m_settings.gamepad = gamepad_hook::local_gamepads->get_controller_from_name(m_settings.gamepad_name);
        }
        // Remote gamepads directly store their data in the gamepad maps

        // Check if remote connection is available
        if (!m_settings.remote_input_data && !m_settings.use_local_input()) {
            std::lock_guard<std::mutex> lock(network::remote_data_map_mutex);
            auto data = network::remote_data.find(m_settings.selected_source);
            if (data != network::remote_data.end())
                m_settings.remote_input_data = data->second;
        }
        m_settings.input_source_check_timer = 0.0f;
    }
}

inline void input_source::render(gs_effect_t *effect)
{
    if (m_procedural_mouse->enabled()) {
        m_procedural_mouse->draw(effect, m_settings);
        return;
    }

    if (m_procedural_keyboard->enabled()) {
        m_procedural_keyboard->draw(effect, m_settings);
        return;
    }

    if (!m_overlay->get_texture() || !m_overlay->get_texture()->texture)
        return;

    if (m_settings.layout_file.empty() || !m_overlay->is_loaded()) {
        gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), m_overlay->get_texture()->texture);
        gs_draw_sprite(m_overlay->get_texture()->texture, 0, cx, cy);
    } else {
        m_overlay->draw(effect);
    }
}

bool use_monitor_center_changed(obs_properties_t *props, obs_property_t *, obs_data_t *data)
{
    const auto use_center = obs_data_get_bool(data, S_MONITOR_USE_CENTER);
    obs_property_set_visible(GET_PROPS(S_MONITOR_H_CENTER), use_center);
    obs_property_set_visible(GET_PROPS(S_MONITOR_V_CENTER), use_center);
    return true;
}

bool reload_pads(obs_properties_t *, obs_property_t *property, void *data)
{
    auto *src = static_cast<input_source *>(data);
    obs_property_list_clear(property);

    if (src->m_settings.use_local_input() && gamepad_hook::state) {
        std::lock_guard<std::mutex> lock(gamepad_hook::local_gamepads->mutex());
        for (const auto &pad : gamepad_hook::local_gamepads->pads()) {
            auto controller = pad.second;
            auto name = controller->identifier();
            obs_property_list_add_string(property, name.c_str(), name.c_str());
        }
    } else if (wss::state && src->m_settings.remote_input_data) {
        auto input_data = src->m_settings.remote_input_data;
        std::lock_guard<std::mutex> lock(input_data->m_mutex);
        for (const auto &pad : input_data->remote_gamepad_names) {
            obs_property_list_add_string(property, pad.second.c_str(), pad.second.c_str());
        }
    }

    return true;
}

bool linear_alpha_changed(void *d, obs_properties_t *, obs_property_t *, obs_data_t *data)
{
    auto *src = static_cast<input_source *>(d);
    //Update settings value for Linear Alpha
    src->m_settings.linear_alpha = obs_data_get_bool(data, S_LINEAR_ALPHA);

    //Only reload texture if file and config have been previously loaded
    if (src->m_overlay->is_loaded()) {
        src->m_overlay->load_texture();
    }

    return true;
}

bool file_changed(void *d, obs_properties_t *props, obs_property_t *, obs_data_t *data)
{
    auto *src = static_cast<input_source *>(d);
    const auto *config = obs_data_get_string(data, S_LAYOUT_FILE);
    auto old_image_file = src->m_settings.image_file;
    src->m_settings.image_file = obs_data_get_string(data, S_OVERLAY_FILE);

    /* Only reload config file if path changed */
    if (src->m_settings.layout_file != config || src->m_settings.image_file != old_image_file) {
        src->m_settings.layout_file = config;
        if (!src->m_overlay->load()) {
            src->m_settings.layout_flags = 0;
        }
    }

    auto const &flags = src->m_settings.layout_flags;
    obs_property_set_visible(GET_PROPS(S_CONTROLLER_L_DEAD_ZONE), flags & OF_LEFT_STICK);
    obs_property_set_visible(GET_PROPS(S_CONTROLLER_R_DEAD_ZONE), flags & OF_RIGHT_STICK);
    obs_property_set_visible(GET_PROPS(S_CONTROLLER_ID),
                             flags & OF_GAMEPAD || (flags & OF_LEFT_STICK || flags & OF_RIGHT_STICK));
    obs_property_set_visible(GET_PROPS(S_MOUSE_SENS), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_MONITOR_USE_CENTER), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_MOUSE_DEAD_ZONE), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_RELOAD_PAD_DEVICES), flags & OF_GAMEPAD);
    reload_pads(nullptr, GET_PROPS(S_CONTROLLER_ID), src);

    return true;
}

bool reload_connections(obs_properties_t *, obs_property_t *list, void *)
{
    obs_property_list_clear(list);
    if (io_config::enable_gamepad_hook || io_config::enable_uiohook)
        obs_property_list_add_string(list, T_LOCAL_SOURCE, "");

    network::remote_data_map_mutex.lock();
    for (const auto &conn : network::remote_data) {
        obs_property_list_add_string(list, conn.first.c_str(), conn.first.c_str());
    }
    network::remote_data_map_mutex.unlock();
    return true;
}

obs_properties_t *get_properties_for_overlay(void *data)
{
    auto *src = static_cast<input_source *>(data);

    QString img_path, layout_path;
    auto *const props = obs_properties_create();
    const int flags = src->m_settings.layout_flags;

    const auto filter_img = util_file_filter(T_FILTER_IMAGE_FILES, "*.jpg *.png *.bmp");
    const auto filter_text = util_file_filter(T_FILTER_TEXT_FILES, "*.json");

    auto *preset = obs_properties_add_list(props, S_BUNDLED_PRESET, T_BUNDLED_PRESET, OBS_COMBO_TYPE_LIST,
                                           OBS_COMBO_FORMAT_STRING);
    obs_property_list_add_string(preset, "Custom files", "");
    for (const auto &entry : bundled_presets)
        obs_property_list_add_string(preset, entry.name, entry.id);
    obs_property_set_modified_callback2(preset, preset_changed, data);

    const auto procedural = obs_properties_add_bool(props, S_PROCEDURAL_ENABLED, T_PROCEDURAL_ENABLED);
    UNUSED_PARAMETER(procedural);
    auto *shape = obs_properties_add_list(props, S_PROCEDURAL_SHAPE, T_PROCEDURAL_SHAPE, OBS_COMBO_TYPE_LIST,
                                          OBS_COMBO_FORMAT_INT);
    obs_property_list_add_int(shape, T_PROCEDURAL_SHAPE_RECTANGLE, 0);
    obs_property_list_add_int(shape, T_PROCEDURAL_SHAPE_ROUNDED, 1);
    obs_property_list_add_int(shape, T_PROCEDURAL_SHAPE_PILL, 2);
    obs_properties_add_int_slider(props, S_PROCEDURAL_KEY_WIDTH, T_PROCEDURAL_KEY_WIDTH, 32, 300, 1);
    obs_properties_add_int_slider(props, S_PROCEDURAL_KEY_HEIGHT, T_PROCEDURAL_KEY_HEIGHT, 32, 300, 1);
    obs_properties_add_int_slider(props, S_PROCEDURAL_GAP, T_PROCEDURAL_GAP, 0, 40, 1);
    obs_properties_add_int_slider(props, S_PROCEDURAL_RADIUS, T_PROCEDURAL_RADIUS, 0, 80, 1);
    obs_properties_add_font(props, S_PROCEDURAL_FONT, T_PROCEDURAL_FONT);
    obs_properties_add_int_slider(props, S_PROCEDURAL_FONT_SIZE, T_PROCEDURAL_FONT_SIZE, 10, 96, 1);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_FILL_COLOR, T_PROCEDURAL_FILL_COLOR);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_PRESSED_COLOR, T_PROCEDURAL_PRESSED_COLOR);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_BORDER_COLOR, T_PROCEDURAL_BORDER_COLOR);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_TEXT_COLOR, T_PROCEDURAL_TEXT_COLOR);
    obs_properties_add_text(props, S_PROCEDURAL_LAYOUT, T_PROCEDURAL_LAYOUT, OBS_TEXT_MULTILINE);

    obs_properties_add_bool(props, S_PROCEDURAL_MOUSE_ENABLED, T_PROCEDURAL_MOUSE_ENABLED);
    obs_properties_add_int_slider(props, S_PROCEDURAL_MOUSE_WIDTH, T_PROCEDURAL_MOUSE_WIDTH, 100, 600, 1);
    obs_properties_add_int_slider(props, S_PROCEDURAL_MOUSE_HEIGHT, T_PROCEDURAL_MOUSE_HEIGHT, 120, 800, 1);
    obs_properties_add_int_slider(props, S_PROCEDURAL_MOUSE_GAP, T_PROCEDURAL_MOUSE_GAP, 0, 40, 1);
    obs_properties_add_int_slider(props, S_PROCEDURAL_MOUSE_RADIUS, T_PROCEDURAL_MOUSE_RADIUS, 0, 100, 1);
    obs_properties_add_font(props, S_PROCEDURAL_MOUSE_FONT, T_PROCEDURAL_MOUSE_FONT);
    obs_properties_add_int_slider(props, S_PROCEDURAL_MOUSE_FONT_SIZE, T_PROCEDURAL_MOUSE_FONT_SIZE, 10, 96, 1);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_MOUSE_FILL_COLOR, T_PROCEDURAL_MOUSE_FILL_COLOR);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_MOUSE_PRESSED_COLOR, T_PROCEDURAL_MOUSE_PRESSED_COLOR);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_MOUSE_BORDER_COLOR, T_PROCEDURAL_MOUSE_BORDER_COLOR);
    obs_properties_add_color_alpha(props, S_PROCEDURAL_MOUSE_TEXT_COLOR, T_PROCEDURAL_MOUSE_TEXT_COLOR);
    obs_properties_add_text(props, S_PROCEDURAL_MOUSE_LEFT_LABEL, T_PROCEDURAL_MOUSE_LEFT_LABEL, OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, S_PROCEDURAL_MOUSE_RIGHT_LABEL, T_PROCEDURAL_MOUSE_RIGHT_LABEL, OBS_TEXT_DEFAULT);
    obs_properties_add_text(props, S_PROCEDURAL_MOUSE_MIDDLE_LABEL, T_PROCEDURAL_MOUSE_MIDDLE_LABEL, OBS_TEXT_DEFAULT);

    /* Config and texture file path */
    auto *texture = obs_properties_add_path(props, S_OVERLAY_FILE, T_TEXTURE_FILE, OBS_PATH_FILE,
                                            qt_to_utf8(filter_img), qt_to_utf8(img_path));
    auto *cfg = obs_properties_add_path(props, S_LAYOUT_FILE, T_LAYOUT_FILE, OBS_PATH_FILE, qt_to_utf8(filter_text),
                                        qt_to_utf8(layout_path));
    auto *prop_alpha = obs_properties_add_bool(props, S_LINEAR_ALPHA, T_LINEAR_ALPHA);

    obs_property_set_modified_callback2(cfg, file_changed, data);
    obs_property_set_modified_callback2(texture, file_changed, data);
    obs_property_set_modified_callback2(prop_alpha, linear_alpha_changed, data);

    /* If enabled add dropdown to select input source */
    if (wss::state) {
        auto *list = obs_properties_add_list(props, S_INPUT_SOURCE, T_INPUT_SOURCE, OBS_COMBO_TYPE_EDITABLE,
                                             OBS_COMBO_FORMAT_STRING);
        obs_properties_add_button(props, S_RELOAD_CONNECTIONS, T_RELOAD_CONNECTIONS, reload_connections);
        if (io_config::enable_gamepad_hook || io_config::enable_uiohook)
            obs_property_list_add_string(list, T_LOCAL_SOURCE, "");

        network::remote_data_map_mutex.lock();
        for (const auto &conn : network::remote_data) {
            obs_property_list_add_string(list, conn.first.c_str(), conn.first.c_str());
        }
        network::remote_data_map_mutex.unlock();
    }
    /* Mouse stuff */
    obs_properties_add_int_slider(props, S_MOUSE_SENS, T_MOUSE_SENS, 1, 2000, 1);

    const auto use_center = obs_properties_add_bool(props, S_MONITOR_USE_CENTER, T_MONITOR_USE_CENTER);
    obs_property_set_modified_callback(use_center, use_monitor_center_changed);

    obs_properties_add_int(props, S_MONITOR_H_CENTER, T_MONITOR_H_CENTER, -9999, 9999, 1);
    obs_properties_add_int(props, S_MONITOR_V_CENTER, T_MONITOR_V_CENTER, -9999, 9999, 1);
    obs_properties_add_int_slider(props, S_MOUSE_DEAD_ZONE, T_MOUSE_DEAD_ZONE, 0, 500, 1);

    /* Gamepad stuff */
    obs_property_set_visible(obs_properties_add_list(props, S_CONTROLLER_ID, T_CONTROLLER_ID, OBS_COMBO_TYPE_EDITABLE,
                                                     OBS_COMBO_FORMAT_STRING),
                             false);

    auto *btn = obs_properties_add_button2(props, S_RELOAD_PAD_DEVICES, T_RELOAD_PAD_DEVICES, reload_pads, src);
    obs_property_set_visible(btn, false);

    obs_property_set_visible(GET_PROPS(S_CONTROLLER_L_DEAD_ZONE), flags & OF_LEFT_STICK);
    obs_property_set_visible(GET_PROPS(S_CONTROLLER_R_DEAD_ZONE), flags & OF_RIGHT_STICK);
    obs_property_set_visible(GET_PROPS(S_CONTROLLER_ID),
                             flags & OF_GAMEPAD || (flags & OF_LEFT_STICK || flags & OF_RIGHT_STICK));
    obs_property_set_visible(GET_PROPS(S_MOUSE_SENS), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_MONITOR_USE_CENTER), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_MOUSE_DEAD_ZONE), flags & OF_MOUSE);
    obs_property_set_visible(GET_PROPS(S_RELOAD_PAD_DEVICES), flags & OF_GAMEPAD);
    reload_pads(nullptr, GET_PROPS(S_CONTROLLER_ID), src);
    return props;
}

void register_overlay_source()
{
    /* Input Overlay */
    obs_source_info si = {};
    si.id = "input-overlay";
    si.type = OBS_SOURCE_TYPE_INPUT;
    si.output_flags = OBS_SOURCE_VIDEO;
    si.get_properties = get_properties_for_overlay;
    si.icon_type = OBS_ICON_TYPE_GAME_CAPTURE;
    si.get_name = [](void *) { return obs_module_text("InputOverlay"); };
    si.create = [](obs_data_t *settings, obs_source_t *source) {
        return static_cast<void *>(new input_source(source, settings));
    };
    si.destroy = [](void *data) { delete static_cast<input_source *>(data); };
    si.get_width = [](void *data) { return static_cast<input_source *>(data)->m_settings.cx; };
    si.get_height = [](void *data) { return static_cast<input_source *>(data)->m_settings.cy; };
    si.get_defaults = [](obs_data_t *settings) {
        obs_data_set_default_bool(settings, S_PROCEDURAL_ENABLED, false);
        obs_data_set_default_int(settings, S_PROCEDURAL_SHAPE, 1);
        obs_data_set_default_int(settings, S_PROCEDURAL_KEY_SIZE, 80);
        obs_data_set_default_int(settings, S_PROCEDURAL_KEY_WIDTH, 80);
        obs_data_set_default_int(settings, S_PROCEDURAL_KEY_HEIGHT, 80);
        obs_data_set_default_int(settings, S_PROCEDURAL_GAP, 8);
        obs_data_set_default_int(settings, S_PROCEDURAL_RADIUS, 12);
        obs_data_set_default_int(settings, S_PROCEDURAL_FONT_SIZE, 28);
        obs_data_set_default_int(settings, S_PROCEDURAL_FILL_COLOR, 0xFF392923);
        obs_data_set_default_int(settings, S_PROCEDURAL_PRESSED_COLOR, 0xFFEB6325);
        obs_data_set_default_int(settings, S_PROCEDURAL_BORDER_COLOR, 0xFFB8A394);
        obs_data_set_default_int(settings, S_PROCEDURAL_TEXT_COLOR, 0xFFFFFFFF);
        obs_data_set_default_string(settings, S_PROCEDURAL_LAYOUT, " W     \nASD[LEFT][DOWN][RIGHT]");
        obs_data_set_default_bool(settings, S_PROCEDURAL_MOUSE_ENABLED, false);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_WIDTH, 220);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_HEIGHT, 300);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_GAP, 8);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_RADIUS, 16);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_FONT_SIZE, 24);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_FILL_COLOR, 0xFF392923);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_PRESSED_COLOR, 0xFFEB6325);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_BORDER_COLOR, 0xFFB8A394);
        obs_data_set_default_int(settings, S_PROCEDURAL_MOUSE_TEXT_COLOR, 0xFFFFFFFF);
        obs_data_set_default_string(settings, S_PROCEDURAL_MOUSE_LEFT_LABEL, "LMB");
        obs_data_set_default_string(settings, S_PROCEDURAL_MOUSE_RIGHT_LABEL, "RMB");
        obs_data_set_default_string(settings, S_PROCEDURAL_MOUSE_MIDDLE_LABEL, "MMB");
        obs_data_set_default_int(settings, S_MOUSE_SENS, 100);
    };
    si.update = [](void *data, obs_data_t *settings) { static_cast<input_source *>(data)->update(settings); };
    si.video_tick = [](void *data, float seconds) { static_cast<input_source *>(data)->tick(seconds); };
    si.video_render = [](void *data, gs_effect_t *effect) { static_cast<input_source *>(data)->render(effect); };
    obs_register_source(&si);
}
}
