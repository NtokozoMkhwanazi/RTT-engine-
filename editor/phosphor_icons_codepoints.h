// Phosphor Icons - UTF-8 encoded for ImGui
// Using raw UTF-8 bytes from codepoints

#ifndef PHOSPHOR_CUSTOM_ICONS_H
#define PHOSPHOR_CUSTOM_ICONS_H

#include <cstdint>

namespace phosphor_icons {

// Codepoints from Phosphor font style.css
// Format: uint32_t to convert at runtime

enum IconCode : uint32_t {
    ICON_FOLDER = 0xe24a,
    ICON_FOLDER_OPEN = 0xe256,
    ICON_FOLDER_PLUS = 0xe258,
    ICON_FILE = 0xe230,
    ICON_FILE_PLUS = 0xe236,
    ICON_FILE_TEXT = 0xe23a,
    ICON_CUBE = 0xe1da,
    ICON_CUBE_TRANSPARENT = 0xec7c,
    ICON_SPHERE = 0xeabc,
    ICON_TRIANGLE = 0xe61e,
    ICON_PLAY = 0xe9c4,
    ICON_PAUSE = 0xea38,
    ICON_STOP = 0xea3a,
    ICON_PLAY_PAUSE = 0xe9c6,
    ICON_SAVE = 0xe248,
    ICON_DOWNLOAD = 0xe20a,
    ICON_UPLOAD = 0xeb9c,
    ICON_TRASH = 0xeb48,
    ICON_TRASH_SIMPLE = 0xeb4a,
    ICON_PENCIL = 0xe270,
    ICON_PENCIL_SIMPLE = 0xe272,
    ICON_ERASER = 0xe21e,
    ICON_SCISSORS = 0xeaa2,
    ICON_COPY = 0xe1ca,
    ICON_CLOCK = 0xe19a,
    ICON_PLUS = 0xee61,
    ICON_MINUS = 0xee63,
    ICON_X = 0xe9c8,
    ICON_CHECK = 0xe182,
    ICON_STAR = 0xe807,
    ICON_HEART = 0xe9d4,
    ICON_BELL = 0xe0ce,
    ICON_USER = 0xe9d8,
    ICON_USERS = 0xe9dc,
    ICON_EYE = 0xe220,
    ICON_EYE_SLASH = 0xe224,
    ICON_MAGNIFYING_GLASS = 0xe268,
    ICON_GEAR = 0xe988,
    ICON_GEAR_SIX = 0xe98c,
    ICON_HOME = 0xe3a8,
    ICON_HOUSE = 0xe3a8,
    ICON_SIGN_OUT = 0xeabc,
    ICON_ARROW_LEFT = 0xe058,
    ICON_ARROW_RIGHT = 0xe06c,
    ICON_ARROW_UP = 0xe08e,
    ICON_ARROW_DOWN = 0xe03e,
    ICON_ARROW_CLOCKWISE = 0xe094,
    ICON_ARROW_COUNTER_CLOCKWISE = 0xe096,
    ICON_CHEVRON_RIGHT = 0xea3e,
    ICON_CHEVRON_DOWN = 0xea3c,
    ICON_CHEVRON_LEFT = 0xea3a,
    ICON_CHEVRON_UP = 0xea3c,
    ICON_CARET_RIGHT = 0xe13a,
    ICON_CARET_DOWN = 0xe136,
    ICON_CARET_LEFT = 0xe138,
    ICON_LINK = 0xe74c,
    ICON_LINK_BREAK = 0xe74e,
    ICON_CAMERA = 0xe10e,
    ICON_IMAGE = 0xe104,
    ICON_GLOBE = 0xe662,
    ICON_CLOUD = 0xe1aa,
    ICON_GRID_FOUR = 0xe898,
    ICON_LIST = 0xe8b4,
    ICON_ROWS = 0xeb80,
    ICON_COLUMNS = 0xea40,
    ICON_SQUARES_FOUR = 0xe190,
    ICON_ARROWS_OUT = 0xe0a2,
    ICON_ARROWS_IN = 0xe09a,
    ICON_WARNING = 0xea48,
    ICON_INFO = 0xe948,
    ICON_ERROR = 0xea46,
    ICON_BUG = 0xe5f4,
    ICON_CHAT = 0xe15c,
    ICON_CHATS = 0xe15e,
    ICON_CODE = 0xe1bc,
    ICON_TERMINAL = 0xe998,
    ICON_CONSOLE = 0xe998,
    ICON_PLUS_CIRCLE = 0xea40,
    ICON_MINUS_CIRCLE = 0xea42,
    ICON_X_CIRCLE = 0xea46,
    ICON_DOTS_THREE = 0xe1fe,
    ICON_DOTS_SIX = 0xe794,
    ICON_DOTS_NINE = 0xe1fc,
    ICON_MUSIC_NOTE = 0xe99c,
    ICON_SPEED = 0xeae0,
    ICON_FILE_CPP = 0xeb2e,
    ICON_FILE_CS = 0xeb30,
    ICON_FILE_PY = 0xeb2c,
    ICON_FILE_JS = 0xeb24,
    ICON_FILE_TS = 0xeb26,
    ICON_FILE_HTML = 0xeb38,
    ICON_FILE_CSS = 0xeb34,
    ICON_SPARKLE = 0xecc8,
    ICON_FILE_ZIP = 0xe958,
    ICON_FRAME_CORNERS = 0xeb70,
    ICON_ARROWS_OUTLINE = 0xe0a0,
    ICON_SYNC = 0xe099,
    ICON_CIRCLE_HALF = 0xeb18,
    ICON_CIRCLE_DASHED = 0xeb1c,
    ICON_ARROW_SQUARE_OUT = 0xe09e,
    ICON_CORNERS_IN = 0xe8a4,
    ICON_ARROW_U_UP_LEFT = 0xe070,
    ICON_ARROW_SQUARE_IN = 0xe09c,
    ICON_RESIZE = 0xeaaa
};

// Convert Unicode codepoint to UTF-8 string
inline void codepointToUTF8(uint32_t cp, char* out) {
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        out[1] = '\0';
    } else if (cp < 0x800) {
        out[0] = static_cast<char>(0xC0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 0x3F));
        out[2] = '\0';
    } else if (cp < 0x10000) {
        out[0] = static_cast<char>(0xE0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (cp & 0x3F));
        out[3] = '\0';
    } else {
        out[0] = static_cast<char>(0xF0 | (cp >> 18));
        out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (cp & 0x3F));
        out[4] = '\0';
    }
}

// Get UTF-8 string for an icon
inline const char* get(IconCode code) {
    static char buffer[8][5];
    static int index = 0;
    char* out = buffer[index];
    index = (index + 1) % 8;
    codepointToUTF8(code, out);
    return out;
}

// Shortcut functions for common icons
inline const char* folder() { return get(ICON_FOLDER); }
inline const char* folder_open() { return get(ICON_FOLDER_OPEN); }
inline const char* folder_plus() { return get(ICON_FOLDER_PLUS); }
inline const char* file() { return get(ICON_FILE); }
inline const char* file_plus() { return get(ICON_FILE_PLUS); }
inline const char* file_text() { return get(ICON_FILE_TEXT); }
inline const char* cube() { return get(ICON_CUBE); }
inline const char* sphere() { return get(ICON_SPHERE); }
inline const char* triangle() { return get(ICON_TRIANGLE); }
inline const char* play() { return get(ICON_PLAY); }
inline const char* pause() { return get(ICON_PAUSE); }
inline const char* stop() { return get(ICON_STOP); }
inline const char* save() { return get(ICON_SAVE); }
inline const char* download() { return get(ICON_DOWNLOAD); }
inline const char* upload() { return get(ICON_UPLOAD); }
inline const char* trash() { return get(ICON_TRASH); }
inline const char* pencil() { return get(ICON_PENCIL); }
inline const char* eye() { return get(ICON_EYE); }
inline const char* gear() { return get(ICON_GEAR); }
inline const char* home() { return get(ICON_HOME); }
inline const char* magnifying_glass() { return get(ICON_MAGNIFYING_GLASS); }
inline const char* plus() { return get(ICON_PLUS); }
inline const char* minus() { return get(ICON_MINUS); }
inline const char* x() { return get(ICON_X); }
inline const char* check() { return get(ICON_CHECK); }
inline const char* star() { return get(ICON_STAR); }
inline const char* heart() { return get(ICON_HEART); }
inline const char* bell() { return get(ICON_BELL); }
inline const char* user() { return get(ICON_USER); }
inline const char* users() { return get(ICON_USERS); }
inline const char* copy() { return get(ICON_COPY); }
inline const char* clock() { return get(ICON_CLOCK); }
inline const char* arrow_left() { return get(ICON_ARROW_LEFT); }
inline const char* arrow_right() { return get(ICON_ARROW_RIGHT); }
inline const char* arrow_up() { return get(ICON_ARROW_UP); }
inline const char* arrow_down() { return get(ICON_ARROW_DOWN); }
inline const char* link() { return get(ICON_LINK); }
inline const char* camera() { return get(ICON_CAMERA); }
inline const char* image() { return get(ICON_IMAGE); }
inline const char* globe() { return get(ICON_GLOBE); }
inline const char* grid() { return get(ICON_GRID_FOUR); }
inline const char* list() { return get(ICON_LIST); }
inline const char* console() { return get(ICON_CONSOLE); }
inline const char* code() { return get(ICON_CODE); }
inline const char* warning() { return get(ICON_WARNING); }
inline const char* info() { return get(ICON_INFO); }
inline const char* bug() { return get(ICON_BUG); }
inline const char* chat() { return get(ICON_CHAT); }
inline const char* dots_three() { return get(ICON_DOTS_THREE); }
inline const char* squares_four() { return get(ICON_SQUARES_FOUR); }
inline const char* arrows_in() { return get(ICON_ARROWS_IN); }
inline const char* arrows_out() { return get(ICON_ARROWS_OUT); }
inline const char* music_note() { return get(ICON_MUSIC_NOTE); }
inline const char* zip() { return get(ICON_FILE_ZIP); }
inline const char* speed() { return get(ICON_SPEED); }
inline const char* caret_right() { return get(ICON_CARET_RIGHT); }
inline const char* caret_left() { return get(ICON_CARET_LEFT); }
inline const char* frame_corners() { return get(ICON_FRAME_CORNERS); }
inline const char* sync() { return get(ICON_SYNC); }
inline const char* arrows_outline() { return get(ICON_ARROWS_OUTLINE); }
inline const char* circle_half() { return get(ICON_CIRCLE_HALF); }
inline const char* circle_dashed() { return get(ICON_CIRCLE_DASHED); }
inline const char* arrow_square_out() { return get(ICON_ARROW_SQUARE_OUT); }
inline const char* corners_in() { return get(ICON_CORNERS_IN); }
inline const char* arrow_u_up_left() { return get(ICON_ARROW_U_UP_LEFT); }
inline const char* arrow_square_in() { return get(ICON_ARROW_SQUARE_IN); }
inline const char* arrow_clockwise() { return get(ICON_ARROW_CLOCKWISE); }
inline const char* arrow_counter_clockwise() { return get(ICON_ARROW_COUNTER_CLOCKWISE); }
inline const char* resize() { return get(ICON_RESIZE); }

} // namespace phosphor_icons

#endif