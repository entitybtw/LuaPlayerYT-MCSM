/*-----------------------------------------------------------------------------------------------------------------------#
#------------- This file is part of: ------------------------------------------------------------------------------------#
#---█████----------------------------███████████--████------------------------------------------█████-█████-███████████--#
#--░░███----------------------------░░███░░░░░███░░███-----------------------------------------░░███-░░███-░█░░░███░░░█--#
#---░███--------█████-████--██████---░███----░███-░███---██████---█████-████--██████--████████--░░███-███--░---░███--░---#
#---░███-------░░███-░███--░░░░░███--░██████████--░███--░░░░░███-░░███-░███--███░░███░░███░░███--░░█████-------░███------#
#---░███--------░███-░███---███████--░███░░░░░░---░███---███████--░███-░███-░███████--░███-░░░----░░███--------░███------#
#---░███------█-░███-░███--███░░███--░███---------░███--███░░███--░███-░███-░███░░░---░███---------░███--------░███------#
#---███████████-░░████████░░████████-█████--------█████░░████████-░░███████-░░██████--█████--------█████-------█████-----#
#--░░░░░░░░░░░---░░░░░░░░--░░░░░░░░-░░░░░--------░░░░░--░░░░░░░░---░░░░░███--░░░░░░--░░░░░--------░░░░░-------░░░░░------#
#------------------------------------------------------------------███-░███----------------------------------------------#
#-----------------------------------------------------------------░░██████-----------------------------------------------#
#------------------------------------------------------------------░░░░░░------------------------------------------------#
#------------------------------------------------------------------------------------ Made by Kodilo --------------------#
#- Special Thanks to: ---------------------------------------------------------------------------------------------------#
#------------------------------------------------------------------------------------------------------------------------#
#- BenHur for intraFont -------------------------------------------------------------------------------------------------#
#- Geecko for gLib2D ----------------------------------------------------------------------------------------------------#
#- Arshia001 for PSPAALIB -----------------------------------------------------------------------------------------------#
#- jonny & Raphael for PMP Mod ------------------------------------------------------------------------------------------#
#- InsertWittyName & MK2k for PGE source code ---------------------------------------------------------------------------#
#- Rinnegatamante & Nanni for example of awesome Lua Player -------------------------------------------------------------#
#-----------------------------------------------------------------------------------------------------------------------*/

#include "libs/callbacks.h"
#include "libs/include_res/output_png.c"

#include "lua/LUA.h"
//#include "lua/asyncCycle.h"
#include "lua/audio.h"
#include "lua/ctrl.h"
#include "lua/graphics.h"
#include "lua/batch.h"
//#include "lua/particles.h"
#include "lua/system.h"
#include "lua/timer.h"
#include "lua/usb.h"
#include "lua/vfpu_math.h"

#define LPYT_MAJOR 0
#define LPYT_MINOR 5
PSP_MODULE_INFO("LuaPlayerYT", 0, LPYT_MAJOR, LPYT_MINOR);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
int sce_newlib_heap_kb_size = (-1024);

extern const unsigned short _ctype_b[];
const unsigned short *__ctype_ptr__ = _ctype_b;

typedef struct
{
    char main_script[256];
    int debug_intro;
    int razgon_bilya;
} LPYT_Config;

void trim(char *str) {
    if (!str || !*str) return;

    char *start = str;
    while (*start && isspace((unsigned char)*start)) start++;

    char *end = str + strlen(str) - 1;
    while (end > start && isspace((unsigned char)*end)) end--;

    *(end + 1) = '\0';

    if (start != str) {
        memmove(str, start, end - start + 2);
    }
}

int ReadConfig(const char *filename, LPYT_Config *config) {
    FILE *file = fopen(filename, "r");
    if (!file) {
        printf("Не удалось открыть конфиг файл: %s\n", filename);
        return 0;
    }

    // Установка значений по умолчанию
    strcpy(config->main_script, "script.lua");
    config->debug_intro = 0;
    config->razgon_bilya = 0;

    char line[256];
    int in_general_section = 0;

    while (fgets(line, sizeof(line), file)) {
        // Удаляем символы перевода строки
        line[strcspn(line, "\r\n")] = '\0';

        // Пропускаем пустые строки и комментарии
        if (line[0] == '\0' || line[0] == '#') continue;

        // Проверяем секции
        if (line[0] == '[') {
            in_general_section = (strcmp(line, "[GENERAL]") == 0);
            continue;
        }

        // Парсим только если находимся в нужной секции
        if (in_general_section) {
            char *separator = strchr(line, '=');
            if (separator) {
                *separator = '\0';
                char *key = line;
                char *value = separator + 1;

                // Удаляем пробелы вокруг ключа и значения
                trim(key);
                trim(value);

                // Заполняем конфиг
                if (strcmp(key, "MAIN_SCRIPT") == 0) {
                    strncpy(config->main_script, value, sizeof(config->main_script) - 1);
                } else if (strcmp(key, "DEBUG_INTRO") == 0) {
                    config->debug_intro = atoi(value);
                } else if (strcmp(key, "RAZGON_BILYA") == 0) {
                    config->razgon_bilya = atoi(value);
                }
            }
        }
    }

    fclose(file);
    return 1;
}

int GETFREEMEMORY() {
    if (SYSTEMint_getMODEL() == 1)
        return (-1024);
    else
        return (-1024);
}

int MESH_init(lua_State *L);

void initEngine(lua_State *L) {
    //ASYNC_init(L);
    AUDIO_init(L);
    COLOR_init(L);
    CTRL_init(L);
    INTRAFONT_init(L);
    LUA_init(L);
    GRAPHICS_init(L);
    BATCH_init(L);
    //PARTICLE_init(L);
    PMP_init(L);
    SYSTEM_init(L);
    TIMER_init(L);
    MESH_init(L);
    USB_init(L);
    VFPU_init(L);
}

// The crash screen: res/error.png, with the text of the error in its white box.
#define ERROR_BOX_X 138
#define ERROR_BOX_Y 56
#define ERROR_BOX_W 300
#define ERROR_BOX_H 160
#define ERROR_PADDING 8
#define ERROR_LINE_GAP 2
#define ERROR_MAX_LINES 32

typedef struct
{
    const char *start;
    int letters;
} ErrorLine;

// Breaks the text into the lines of the box, at a space or a slash when a line has
// one, so that a long path still fits. Returns how many lines the text needs.
// The lengths of intraFont are in letters, not in bytes.
static int layoutError(intraFont *font, const char *text, ErrorLine *lines) {
    const float width = ERROR_BOX_W - 2 * ERROR_PADDING;
    int count = 0;
    const char *p = text;

    while (*p) {
        while (*p == ' ') p++;

        const char *start = p;
        const char *lastBreak = NULL;
        int letters = 0, lettersAtBreak = 0;

        while (*p && *p != '\n') {
            const char *next = p + 1;
            while ((*next & 0xC0) == 0x80) next++; // the rest of a UTF-8 letter

            if (intraFontMeasureTextEx(font, start, letters + 1) > width && letters > 0) {
                if (lastBreak) {
                    p = lastBreak;
                    letters = lettersAtBreak;
                }
                break;
            }

            letters++;
            if (*p == ' ' || *p == '/') {
                lastBreak = next;
                lettersAtBreak = letters;
            }
            p = next;
        }

        if (count < ERROR_MAX_LINES) {
            lines[count].start = start;
            lines[count].letters = letters;
        }
        count++;

        if (*p == '\n') p++;
    }

    return count;
}

static int errorLineHeight(intraFont *font) {
    return intraFontTextLineHeight(font) + ERROR_LINE_GAP;
}

static int errorLinesFit(intraFont *font, int count) {
    return count <= ERROR_MAX_LINES && count * errorLineHeight(font) <= ERROR_BOX_H - 2 * ERROR_PADDING;
}

// minecraft.ttf stands on the pixel grid at the sizes 16 and 8 only. The built-in
// font is for a crash that left no memory or no font file.
static intraFont *loadErrorFont(const char *text, ErrorLine *lines, int *count, bool *ownFont) {
    static const float sizes[] = {16.f, 8.f};
    unsigned int i;

    *ownFont = TRUE;
    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        intraFont *font = intraFontLoadTTF("assets/minecraft.ttf", INTRAFONT_STRING_UTF8, sizes[i]);
        if (!font) break;

        intraFontSetStyle(font, 1.f, G2D_RGBA(0, 0, 0, 255), 0, 0.f, 0);
        *count = layoutError(font, text, lines);
        if (errorLinesFit(font, *count) || i == sizeof(sizes) / sizeof(sizes[0]) - 1)
            return font;

        intraFontUnload(font);
    }

    *ownFont = FALSE;
    intraFontSetStyle(luaFont, 0.8f, G2D_RGBA(0, 0, 0, 255), 0, 0.f, 0);
    *count = layoutError(luaFont, text, lines);
    return luaFont;
}

static void showErrorScreen(lua_State *L, const char *message) {
    static ErrorLine lines[ERROR_MAX_LINES];
    bool USB_ACTIVE = FALSE;
    bool ownFont;
    int count, i;

    lua_gc(L, LUA_GCCOLLECT, 0); // the crash may be out of memory

    g2dImage *ERR = g2dTexLoad(NULL, error_data, size_error_data, G2D_VOID);
    intraFont *font = loadErrorFont(message, lines, &count, &ownFont);

    int lineHeight = errorLineHeight(font);
    int maxLines = (ERROR_BOX_H - 2 * ERROR_PADDING) / lineHeight;
    if (maxLines > ERROR_MAX_LINES) maxLines = ERROR_MAX_LINES;
    if (count > maxLines) count = maxLines;

    AalibPlay(PSPAALIB_CHANNEL_WAV_32);

    while (1) {
        controls_read();

        if (controls_pressed(PSP_CTRL_CROSS))
            sceKernelExitGame();

        if (controls_pressed(PSP_CTRL_TRIANGLE)) {
            if (USB_ACTIVE)
                USB_deactivate();
            else
                USB_activate();

            USB_ACTIVE = !USB_ACTIVE;
        }

        g2dClear(G2D_RGBA(0, 0, 0, 255));
        g2dResetScissor();

        if (ERR) {
            g2dBeginRects(ERR);
            g2dAdd();
            g2dEnd();
        } else { // no memory for the picture: the text still needs its box
            g2dBeginRects(NULL);
            g2dSetColor(G2D_RGBA(255, 255, 255, 255));
            g2dSetCoordXY(ERROR_BOX_X, ERROR_BOX_Y);
            g2dSetScaleWH(ERROR_BOX_W, ERROR_BOX_H);
            g2dAdd();
            g2dEnd();
        }

        intraFontActivate(font, !ownFont); // no smoothing on the pixel font
        for (i = 0; i < count; i++) {
            intraFontPrintEx(font, ERROR_BOX_X + ERROR_PADDING,
                ERROR_BOX_Y + ERROR_PADDING + i * lineHeight + intraFontTextHeight(font),
                lines[i].start, lines[i].letters);
        }

        g2dFlip(G2D_VSYNC);
    }
}

int main() {
    LPYT_Config config;

    strncpy(config.main_script, "script.lua", sizeof(config.main_script) - 1);

    //if (!ReadConfig("lpyt.ini", &config))
    //    sceKernelExitGame();

    SetupCallbacks();

    lua_State *L = lua_open();

    luaL_openlibs(L);
    initEngine(L);

    SYSTEM_getTITLEID("EBOOT.PBP", LPYTGameTitle, LPYTGameID);

    if (scePowerGetCpuClockFrequency() != 333)
        scePowerSetClockFrequency(333, 333, 166);

    FILE *f = fopen(config.main_script, "r");
    if (!f) sceKernelExitGame();
    fclose(f);

    while (1) {
        if (luaL_loadfile(L, config.main_script) != 0 || lua_pcall(L, 0, LUA_MULTRET, 0) != 0) { //error
            const char *message = lua_tostring(L, -1);
            if (!message) message = "unknown error";

            FILE *f = fopen("error_log.txt", "a");
            if (f) {
                fprintf(f, "%s\n", message);
                fclose(f);
            }

            showErrorScreen(L, message);
            lua_pop(L, 1);
        }
    }
    lua_close(L);
    free(config.main_script);
    LPYT_FastFinish();
    return 0;
}