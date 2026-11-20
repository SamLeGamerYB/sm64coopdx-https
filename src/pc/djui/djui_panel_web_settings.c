#include "djui.h"
#include "djui_panel.h"
#include "djui_panel_menu.h"
#include "djui_panel_web_settings.h"
#include "pc/fs/fs.h"
#include "pc/lua/smlua_network_api.h"
#include "pc/utils/misc.h"

#include <stdio.h>
#include <string.h>

#define WEB_HISTORY_LINES 24
#define WEB_HISTORY_LINE_LENGTH 512

static void djui_panel_web_settings_open_folder(UNUSED struct DjuiBase* caller) {
    smlua_network_api_settings_init();
    open_folder(fs_get_write_path(""));
}

static void djui_panel_web_settings_history_create(struct DjuiBase* caller) {
    struct DjuiThreePanel* panel = djui_panel_menu_create("Web Access History", false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    char lines[WEB_HISTORY_LINES][WEB_HISTORY_LINE_LENGTH] = { 0 };
    unsigned int count = 0;
    smlua_network_api_settings_init();

    const char* path = fs_get_write_path("network_access_history.txt");
    FILE* file = path != NULL ? fopen(path, "rb") : NULL;
    if (file != NULL) {
        char line[WEB_HISTORY_LINE_LENGTH];
        while (fgets(line, sizeof(line), file) != NULL) {
            size_t length = strlen(line);
            while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
                line[--length] = '\0';
            }
            if (count < WEB_HISTORY_LINES) {
                snprintf(lines[count++], WEB_HISTORY_LINE_LENGTH, "%s", line);
            } else {
                memmove(lines, lines + 1, sizeof(lines[0]) * (WEB_HISTORY_LINES - 1));
                snprintf(lines[WEB_HISTORY_LINES - 1], WEB_HISTORY_LINE_LENGTH, "%s", line);
            }
        }
        fclose(file);
    }

    if (count == 0) {
        djui_text_create(body, "No web access recorded yet.");
    } else {
        djui_text_create(body, "Most recent accesses (up to 24):");
        for (unsigned int i = 0; i < count; ++i) {
            djui_text_create(body, lines[i]);
        }
    }
    djui_button_create(body, DJUI_MENU_BACK, DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);
    djui_panel_add(caller, panel, NULL);
}

void djui_panel_web_settings_create(struct DjuiBase* caller) {
    struct DjuiThreePanel* panel = djui_panel_menu_create("Web Settings", false);
    struct DjuiBase* body = djui_three_panel_get_body(panel);
    smlua_network_api_settings_init();

    djui_text_create(body, "Global HTTPS and WSS settings for all mods.");
    djui_text_create(body, "Edit network_blacklist.txt in the game user folder.");
    djui_text_create(body, "One domain per line; # starts a comment.");
    djui_button_create(body, "Open user folder (blacklist file)", DJUI_BUTTON_STYLE_NORMAL, djui_panel_web_settings_open_folder);
    djui_button_create(body, "View web access history", DJUI_BUTTON_STYLE_NORMAL, djui_panel_web_settings_history_create);
    djui_button_create(body, DJUI_MENU_BACK, DJUI_BUTTON_STYLE_BACK, djui_panel_menu_back);

    djui_panel_add(caller, panel, NULL);
}
