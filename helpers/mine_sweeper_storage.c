#include "mine_sweeper_storage.h"

static inline uint32_t clamp(uint32_t min, uint32_t max, uint32_t val) {
    return (val > max) ? max : (val < min) ? min : val;
}

static Storage* mine_sweeper_open_storage() {
    return furi_record_open(RECORD_STORAGE);
}

static void mine_sweeper_close_storage() {
    furi_record_close(RECORD_STORAGE);
}

static void mine_sweeper_close_config_file(FlipperFormat* file) {
    if (file == NULL) return;
    flipper_format_file_close(file);
    flipper_format_free(file);
}

void mine_sweeper_save_settings(void* context) {
    MineSweeperApp* app = context;

    Storage* storage = mine_sweeper_open_storage();
    FlipperFormat* fff_file = flipper_format_file_alloc(storage);
    
    // Overwrite wont work, so delete first
    if(storage_file_exists(storage, MINESWEEPER_SETTINGS_SAVE_PATH)) {
        storage_simply_remove(storage, MINESWEEPER_SETTINGS_SAVE_PATH);
    }

    // Open File, create if not exists
    if(!storage_common_stat(storage, MINESWEEPER_SETTINGS_SAVE_PATH, NULL) == FSE_OK) {
        FURI_LOG_D(TAG, "Config file %s is not found. Will create new.", MINESWEEPER_SETTINGS_SAVE_PATH);
        if(storage_common_stat(storage, CONFIG_FILE_DIRECTORY_PATH, NULL) == FSE_NOT_EXIST) {
            FURI_LOG_D(
                TAG,
                "Directory %s doesn't exist. Will create new.",
                CONFIG_FILE_DIRECTORY_PATH);
            if(!storage_simply_mkdir(storage, CONFIG_FILE_DIRECTORY_PATH)) {
                FURI_LOG_E(TAG, "Error creating directory %s", CONFIG_FILE_DIRECTORY_PATH);
            }
        }
    }

    if(!flipper_format_file_open_new(fff_file, MINESWEEPER_SETTINGS_SAVE_PATH)) {
        FURI_LOG_E(TAG, "Error creating new file %s", MINESWEEPER_SETTINGS_SAVE_PATH);
        mine_sweeper_close_storage();
        return;
    }
    
    // Store Settings
    flipper_format_write_header_cstr(
        fff_file, MINESWEEPER_SETTINGS_HEADER, MINESWEEPER_SETTINGS_FILE_VERSION);

    uint32_t w =  app->settings_info.board_width,
             h =  app->settings_info.board_height,
             d =  app->settings_info.difficulty,
             f =  app->feedback_enabled,
             wr = app->wrap_enabled,
             s =  app->ensure_map_solvable ? 1 : 0;

    flipper_format_write_uint32(
        fff_file, MINESWEEPER_SETTINGS_KEY_WIDTH, &w, 1);
    flipper_format_write_uint32(
        fff_file, MINESWEEPER_SETTINGS_KEY_HEIGHT, &h, 1);
    flipper_format_write_uint32(
        fff_file, MINESWEEPER_SETTINGS_KEY_DIFFICULTY, &d, 1);
    flipper_format_write_uint32(
        fff_file, MINESWEEPER_SETTINGS_KEY_FEEDBACK, &f, 1);
    flipper_format_write_uint32(
        fff_file, MINESWEEPER_SETTINGS_KEY_WRAP, &wr, 1);
    flipper_format_write_uint32(
        fff_file, MINESWEEPER_SETTINGS_KEY_SOLVABLE, &s, 1);
    
    if(!flipper_format_rewind(fff_file)) {
        FURI_LOG_E(TAG, "Rewind error");
        mine_sweeper_close_config_file(fff_file);
        mine_sweeper_close_storage();
        return;
    }

    mine_sweeper_close_config_file(fff_file);
    mine_sweeper_close_storage();
    return;
}

bool mine_sweeper_read_settings(void* context) {
    MineSweeperApp* app = context;

    Storage* storage = mine_sweeper_open_storage();
    FlipperFormat* fff_file = flipper_format_file_alloc(storage);

    if(storage_common_stat(storage, MINESWEEPER_SETTINGS_SAVE_PATH, NULL) != FSE_OK) {
        mine_sweeper_close_config_file(fff_file);
        mine_sweeper_close_storage();
        return false;
    }

    uint32_t file_version;
    FuriString* temp_str = furi_string_alloc();

    if (!flipper_format_file_open_existing(fff_file, MINESWEEPER_SETTINGS_SAVE_PATH)) {
        FURI_LOG_E(TAG, "Cannot open file %s", MINESWEEPER_SETTINGS_SAVE_PATH);
        mine_sweeper_close_config_file(fff_file);
        mine_sweeper_close_storage();
        return false;
    }

    if(!flipper_format_read_header(fff_file, temp_str, &file_version)) {
        FURI_LOG_E(TAG, "Missing Header Data");
        mine_sweeper_close_config_file(fff_file);
        mine_sweeper_close_storage();
        return false;
    }

    furi_string_free(temp_str);

    if(file_version < MINESWEEPER_SETTINGS_FILE_VERSION) {
        FURI_LOG_D(TAG, "old config version, will be removed.");
        mine_sweeper_close_config_file(fff_file);
        mine_sweeper_close_storage();
        return false;
    }

    uint32_t w = 7, h = 16, d = 0, f = 1, wr = 1, s = 0;
    flipper_format_read_uint32(fff_file, MINESWEEPER_SETTINGS_KEY_WIDTH, &w, 1);
    flipper_format_read_uint32(fff_file, MINESWEEPER_SETTINGS_KEY_HEIGHT, &h, 1);
    flipper_format_read_uint32(fff_file, MINESWEEPER_SETTINGS_KEY_DIFFICULTY, &d, 1);
    flipper_format_read_uint32(fff_file, MINESWEEPER_SETTINGS_KEY_FEEDBACK, &f, 1);
    flipper_format_read_uint32(fff_file, MINESWEEPER_SETTINGS_KEY_WRAP, &wr, 1);
    flipper_format_read_uint32(fff_file, MINESWEEPER_SETTINGS_KEY_SOLVABLE, &s, 1);

    w  = clamp(16, 32, w);
    h  = clamp(7, 32, h);
    d  = clamp(0, 2, d);
    f  = clamp(0, 1, f);
    wr = clamp(0, 1, wr);
    s  = clamp(0, 1, s);

    app->settings_info.board_width = (uint8_t) w;
    app->settings_info.board_height = (uint8_t) h;
    app->settings_info.difficulty = (uint8_t) d;
    app->feedback_enabled = (uint8_t) f;
    app->wrap_enabled = (uint8_t) wr;
    app->ensure_map_solvable = s == 1 ? true : false;

    flipper_format_rewind(fff_file);

    mine_sweeper_close_config_file(fff_file);
    mine_sweeper_close_storage();

    return true;
}
