#include "minesweeper_game_screen.h"
#include "minesweeper_icons.h"

#include <gui/elements.h>
#include <gui/icon_animation.h>
#include <input/input.h>

#include <furi.h>
#include <furi_hal.h>

static const Icon* tile_icons[13] = {
    &I_tile_empty_8x8,
    &I_tile_0_8x8,
    &I_tile_1_8x8,
    &I_tile_2_8x8,
    &I_tile_3_8x8,
    &I_tile_4_8x8,
    &I_tile_5_8x8,
    &I_tile_6_8x8,
    &I_tile_7_8x8,
    &I_tile_8_8x8,
    &I_tile_mine_8x8,
    &I_tile_flag_8x8,
    &I_tile_uncleared_8x8,
};

// They way this enum is set up allows us to index the Icon* array above for some mine types
typedef enum {
    MineSweeperGameScreenTileNone = 0,
    MineSweeperGameScreenTileZero,
    MineSweeperGameScreenTileOne,
    MineSweeperGameScreenTileTwo,
    MineSweeperGameScreenTileThree,
    MineSweeperGameScreenTileFour,
    MineSweeperGameScreenTileFive,
    MineSweeperGameScreenTileSix,
    MineSweeperGameScreenTileSeven,
    MineSweeperGameScreenTileEight,
    MineSweeperGameScreenTileMine,
    MineSweeperGameScreenTileTypeCount,
} MineSweeperGameScreenTileType;

typedef enum {
    MineSweeperGameScreenTileStateFlagged,
    MineSweeperGameScreenTileStateUncleared,
    MineSweeperGameScreenTileStateCleared,
} MineSweeperGameScreenTileState;

struct MineSweeperGameScreen {
    View* view;
    void* context;
    GameScreenInputCallback input_callback;
};

typedef struct {
    int16_t x_abs, y_abs;
} CurrentPosition;

typedef struct {
    uint16_t x_abs, y_abs;
    const Icon* icon;
} IconElement;

typedef struct {
    IconElement icon_element;
    MineSweeperGameScreenTileState tile_state;
    MineSweeperGameScreenTileType tile_type;
} MineSweeperTile;

typedef struct {
    MineSweeperTile board[ MINESWEEPER_BOARD_MAX_TILES ];
    CurrentPosition curr_pos;
    uint8_t right_boundary, bottom_boundary,
            board_width, board_height, board_difficulty;
    uint16_t mines_left;
    uint16_t flags_left;
    uint32_t start_tick;
    FuriString* info_str;
    bool is_making_first_move;
    bool is_holding_down_button;
} MineSweeperGameScreenModel;

static const float difficulty_multiplier[5] = {
    0.15f,
    0.17f,
    0.19f,
    0.23f,
    0.27f,
};

/****************************************************************
 * Function declarations
 *
 * Non public function declarations
 ***************************************************************/

// Static helper functions
static void setup_board(MineSweeperGameScreen* instance);
static Point bfs_to_closest_tile(MineSweeperGameScreenModel* model);
static inline void bfs_tile_clear(MineSweeperGameScreenModel* model);
static void mine_sweeper_game_screen_set_board_information(
        MineSweeperGameScreen* instance,
        uint8_t width,
        uint8_t height,
        uint8_t difficulty);

// Currently not using enter/exit callback
static void mine_sweeper_game_screen_view_enter(void* context);
static void mine_sweeper_game_screen_view_exit(void* context);

// Different input/draw callbacks for play/win/lose state
static void mine_sweeper_game_screen_view_lose_draw_callback(Canvas* canvas, void* _model);
static void mine_sweeper_game_screen_view_win_draw_callback(Canvas* canvas, void* _model);
static void mine_sweeper_game_screen_view_play_draw_callback(Canvas* canvas, void* _model);

static bool mine_sweeper_game_screen_view_end_input_callback(InputEvent* event, void* context);
static bool mine_sweeper_game_screen_view_play_input_callback(InputEvent* event, void* context);



/**************************************************************
 * Function definitions
 *************************************************************/

static void setup_board(MineSweeperGameScreen* instance) {
    furi_assert(instance);

    static double time = 0.0;
    static uint32_t iter = 0;
    iter++;

    uint32_t start_tick = furi_get_tick();

    uint16_t board_tile_count = 0;
    uint8_t board_width = 0, board_height = 0, board_difficulty = 0;

    with_view_model(
        instance->view,
        MineSweeperGameScreenModel * model,
        {
            board_width = model->board_width;
            board_height = model->board_height;
            board_tile_count =  (model->board_width*model->board_height);
            board_difficulty = model->board_difficulty;
        },
        false
    );

    uint16_t num_mines = board_tile_count * difficulty_multiplier[ board_difficulty ];

    /** We can use a temporary buffer to set the tile types initially
     * and manipulate then save to actual model
     */
    MineSweeperGameScreenTileType tiles[MINESWEEPER_BOARD_MAX_TILES];
    memset(&tiles, MineSweeperGameScreenTileNone, sizeof(tiles));

    // Randomly place tiles except in the corners to help guarantee solvability
    for (uint16_t i = 0; i < num_mines; i++) {

        uint16_t rand_pos;
        uint16_t x;
        uint16_t y;
        bool is_invalid_position;
        do {

            rand_pos = furi_hal_random_get() % board_tile_count;
            x = rand_pos / board_width;
            y = rand_pos % board_width;

            is_invalid_position = ((rand_pos == 0)                      ||
                                         (x==0 && y==1)                 ||
                                         (x==1 && y==0)                 ||
                                         rand_pos == board_tile_count-1 ||
                                         (x==0 && y==board_width-1)     ||
                                         (x==board_height-1 && y==0));


        } while (tiles[rand_pos] == MineSweeperGameScreenTileMine || is_invalid_position);

        tiles[rand_pos] = MineSweeperGameScreenTileMine;
    }

    /** All mines are set so we look at each tile for surrounding mines */
    for (uint16_t i = 0; i < board_tile_count; i++) {
        MineSweeperGameScreenTileType tile_type = tiles[i];

        if (tile_type == MineSweeperGameScreenTileMine) {
            continue;
        }

        int8_t offsets[8][2] = {
            {-1,1},
            {0,1},
            {1,1},
            {1,0},
            {1,-1},
            {0,-1},
            {-1,-1},
            {-1,0},
        };

        uint16_t mine_count = 0;

        uint16_t x = i / board_width;
        uint16_t y = i % board_width;

        for (uint8_t j = 0; j < 8; j++) {
            int16_t dx = x + (int16_t)offsets[j][0];
            int16_t dy = y + (int16_t)offsets[j][1];

            if (dx < 0 || dy < 0 || dx >= board_height || dy >= board_width) {
                continue;
            }

            uint16_t pos = dx * board_width + dy;
            if (tiles[pos] == MineSweeperGameScreenTileMine) {
                mine_count++;
            }

        }

        tiles[i] = (MineSweeperGameScreenTileType) mine_count+1;

    }

    // Save tiles to view model
    // Because of way tile enum and tile_icons array is set up we can
    // index tile_icons with the enum type to get the correct Icon*
    with_view_model(
        instance->view,
        MineSweeperGameScreenModel * model,
        {
            for (uint16_t i = 0; i < board_tile_count; i++) {
                model->board[i].tile_type = tiles[i];
                model->board[i].tile_state = MineSweeperGameScreenTileStateUncleared;
                model->board[i].icon_element.icon = tile_icons[ tiles[i] ];
                model->board[i].icon_element.x_abs = (i/model->board_width);
                model->board[i].icon_element.y_abs = (i%model->board_width);
            }

            model->mines_left = num_mines;
            model->flags_left = num_mines;
            model->curr_pos.x_abs = 0;
            model->curr_pos.y_abs = 0;
            model->right_boundary = MINESWEEPER_SCREEN_TILE_WIDTH;
            model->bottom_boundary = MINESWEEPER_SCREEN_TILE_HEIGHT;
            model->is_making_first_move = true;         
        },
        true
    );

    uint32_t ticks_elapsed = furi_get_tick() - start_tick;
    double sec = (double)ticks_elapsed / (double)furi_kernel_get_tick_frequency();
    double milliseconds = 1000.0L * sec;
    time += milliseconds;
    
    FURI_LOG_D(MS_DEBUG_TAG, "Setup board time: %.03f", time/iter);
}

static inline Point bfs_to_closest_tile(MineSweeperGameScreenModel* model) {
    furi_assert(model);

    static double bfs_time = 0.0L;
    static double time = 0.0L;
    static uint32_t iter = 0;
    iter++;

    uint32_t start_tick = furi_get_tick();

    // Init both the set and dequeue
    point_deq_t deq;
    point_set_t set;

    point_deq_init(deq);
    point_set_init(set);

    // Return the value in this point
    Point result;

    // Point_t pos will be used to keep track of the current point
    Point_t pos;
    pointobj_init(pos);

    // Starting position is current pos
    Point start_pos = (Point){.x = model->curr_pos.x_abs, .y = model->curr_pos.y_abs};
    pointobj_set_point(pos, start_pos);

    point_deq_push_back(deq, pos);

    uint32_t bfs_start_tick = furi_get_tick();

    uint16_t i = 0;
    
    while (point_deq_size(deq) > 0) {
        point_deq_pop_front(&pos, deq);
        Point curr_pos = pointobj_get_point(pos);
        uint16_t curr_pos_1d = curr_pos.x * model->board_width + curr_pos.y;

        // If the current tile is uncovered and not start_pos go to that position
        if (model->board[curr_pos_1d].tile_state == MineSweeperGameScreenTileStateUncleared &&
                !(start_pos.x ==  curr_pos.x && start_pos.y == curr_pos.y)) {

            result = curr_pos;
            break;
        }
        
        // If in visited set continue
        if (point_set_cget(set, pos) != NULL) {
            continue;
        } 
        
        // Add point to visited set
        point_set_push(set, pos);


        // Process all surrounding neighbors and add valid to dequeue
        int8_t offsets[8][2] = {
            {-1,1},
            {0,1},
            {1,1},
            {1,0},
            {1,-1},
            {0,-1},
            {-1,-1},
            {-1,0},
        };

        for (uint8_t i = 0; i < 8; i++) {
            int16_t dx = curr_pos.x + (int16_t)offsets[i][0];
            int16_t dy = curr_pos.y + (int16_t)offsets[i][1];

            if (dx < 0 || dy < 0 || dx >= model->board_height || dy >= model->board_width) {
                continue;
            }

            Point neighbor = (Point) {.x = dx, .y = dy};
            pointobj_set_point(pos, neighbor);
            point_deq_push_back(deq, pos);
        }
    }

    uint32_t bfs_end_tick = furi_get_tick();

    point_set_clear(set);
    point_deq_clear(deq);

    uint32_t ticks_elapsed = furi_get_tick() - start_tick;
    uint32_t bfs_ticks_elapsed = bfs_end_tick - bfs_start_tick;
    double sec = (double)ticks_elapsed / (double)furi_kernel_get_tick_frequency();
    double bfs_sec = (double)bfs_ticks_elapsed / (double)furi_kernel_get_tick_frequency();
    double milliseconds = 1000.0L * sec;
    double bfs_milliseconds = 1000.0L * bfs_sec;
    time += milliseconds;
    bfs_time += bfs_milliseconds;

    FURI_LOG_D(MS_DEBUG_TAG, "BFS specific time: %.03f, in %d iter", bfs_milliseconds, i);
    FURI_LOG_D(MS_DEBUG_TAG, "BFS Clear function time: %.03f", milliseconds);

    FURI_LOG_D(MS_DEBUG_TAG, "BFS specific time: %.03f, in %d iter", bfs_time/iter, i);
    FURI_LOG_D(MS_DEBUG_TAG, "BFS Jump function time: %.03f", time/iter);

    return result;
}

// Four way BFS 'Flood fill' to clear adjacent non-mine tiles
// We can use m*lib for a set and dequeue for BFS
static inline void bfs_tile_clear(MineSweeperGameScreenModel* model) {
    furi_assert(model);

    static double bfs_time = 0.0L;
    static double time = 0.0L;
    static uint32_t iter = 0;
    iter++;

    uint32_t start_tick = furi_get_tick();
    
    // Init both the set and dequeue
    point_deq_t deq;
    point_set_t set;

    point_deq_init(deq);
    point_set_init(set);

    // Point_t pos will be used to keep track of the current point
    Point_t pos;
    pointobj_init(pos);

    // Starting position is current pos
    Point start_pos = (Point){.x = model->curr_pos.x_abs, .y = model->curr_pos.y_abs};
    pointobj_set_point(pos, start_pos);

    point_deq_push_back(deq, pos);

    uint32_t bfs_start_tick = furi_get_tick();

    uint16_t i = 0;

    while (point_deq_size(deq) > 0) {
        point_deq_pop_front(&pos, deq);
        Point curr_pos = pointobj_get_point(pos);
        uint16_t curr_pos_1d = curr_pos.x * model->board_width + curr_pos.y;
        
        // If in visited set continue
        if (point_set_cget(set, pos) != NULL) {
            continue;
        } 
        
        // Set tile to cleared
        model->board[curr_pos_1d].tile_state = MineSweeperGameScreenTileStateCleared;

        // Add point to visited set
        point_set_push(set, pos);

        // If the current tile is not a zero tile we do not continue
        if (model->board[curr_pos_1d].tile_type != MineSweeperGameScreenTileZero) {
            continue;
        }

        // Process all surrounding neighbors and add valid to dequeue
        int8_t offsets[8][2] = {
            {-1,1},
            {0,1},
            {1,1},
            {1,0},
            {1,-1},
            {0,-1},
            {-1,-1},
            {-1,0},
        };

        for (uint8_t i = 0; i < 8; i++) {
            int16_t dx = curr_pos.x + (int16_t)offsets[i][0];
            int16_t dy = curr_pos.y + (int16_t)offsets[i][1];

            if (dx < 0 || dy < 0 || dx >= model->board_height || dy >= model->board_width) {
                continue;
            }

            Point neighbor = (Point) {.x = dx, .y = dy};
            pointobj_set_point(pos, neighbor);
            point_deq_push_back(deq, pos);
        }
        i++;
    }

    uint32_t bfs_end_tick = furi_get_tick();

    point_set_clear(set);
    point_deq_clear(deq);

    uint32_t ticks_elapsed = furi_get_tick() - start_tick;
    uint32_t bfs_ticks_elapsed = bfs_end_tick - bfs_start_tick;
    double sec = (double)ticks_elapsed / (double)furi_kernel_get_tick_frequency();
    double bfs_sec = (double)bfs_ticks_elapsed / (double)furi_kernel_get_tick_frequency();
    double milliseconds = 1000.0L * sec;
    double bfs_milliseconds = 1000.0L * bfs_sec;
    time += milliseconds;
    bfs_time += bfs_milliseconds;

    FURI_LOG_D(MS_DEBUG_TAG, "BFS specific time: %.03f, in %d iter", bfs_milliseconds, i);
    FURI_LOG_D(MS_DEBUG_TAG, "BFS Clear function time: %.03f", milliseconds);

    FURI_LOG_D(MS_DEBUG_TAG, "BFS specific time avg: %.03f, in %d iter", bfs_time/iter, i);
    FURI_LOG_D(MS_DEBUG_TAG, "BFS Clear function time avg: %.03f", time/iter);
}

static void mine_sweeper_game_screen_set_board_information(
        MineSweeperGameScreen* instance,
        uint8_t width,
        uint8_t height,
        uint8_t difficulty) {

    furi_assert(instance);

    // These are the min/max values that can actually be set
    if (width  > 146) {width = 146;}
    if (width  < 16 ) {width = 16;}
    if (height > 64 ) {height = 64;}
    if (height < 7  ) {height = 7;}
    if (difficulty > 4 ) {difficulty = 4;}
    
    with_view_model(
        instance->view,
        MineSweeperGameScreenModel * model,
        {
            model->board_width = width;
            model->board_height = height;
            model->board_difficulty = difficulty;
        },
        true
    );
}

static void mine_sweeper_game_screen_view_enter(void* context) {
    furi_assert(context);
    UNUSED(context);
}

static void mine_sweeper_game_screen_view_exit(void* context) {
    furi_assert(context);
    UNUSED(context);
}

static void mine_sweeper_game_screen_view_win_draw_callback(Canvas* canvas, void* _model) {
    furi_assert(canvas);
    furi_assert(_model);
    MineSweeperGameScreenModel* model = _model;

    canvas_clear(canvas);

    canvas_set_color(canvas, ColorBlack);
    
    uint16_t cursor_pos_1d = model->curr_pos.x_abs * model->board_width + model->curr_pos.y_abs;
    
    for (uint8_t x_rel = 0; x_rel < MINESWEEPER_SCREEN_TILE_HEIGHT; x_rel++) {
        uint16_t x_abs = (model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT) + x_rel;
        
        for (uint8_t y_rel = 0; y_rel < MINESWEEPER_SCREEN_TILE_WIDTH; y_rel++) {
            uint16_t y_abs = (model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH) + y_rel;

            uint16_t curr_rendering_tile_pos_1d = x_abs * model->board_width + y_abs;
            MineSweeperTile tile = model->board[curr_rendering_tile_pos_1d];

            if (cursor_pos_1d == curr_rendering_tile_pos_1d) {
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }

            canvas_draw_icon(
                canvas,
                y_rel * icon_get_width(tile.icon_element.icon),
                x_rel * icon_get_height(tile.icon_element.icon),
                tile.icon_element.icon);

        }
    }

    canvas_set_color(canvas, ColorBlack);
    // If any borders are at the limits of the game board we draw a border line
    
    // Right border 
    if (model->right_boundary == model->board_width) {
        canvas_draw_line(canvas, 127,0,127,63-8);
    }

    // Left border
    if ((model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH) == 0) {
        canvas_draw_line(canvas, 0,0,0,63-8);
    }

    // Bottom border
    if (model->bottom_boundary == model->board_height) {
        canvas_draw_line(canvas, 0,63-8,127,63-8);
    }

    // Top border
    if ((model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT) == 0) {
        canvas_draw_line(canvas, 0,0,127,0);
    }


    // Draw win text
    furi_string_printf(
            model->info_str,
            "YOU WIN!");

    canvas_draw_str_aligned(
            canvas,
            0,
            64-7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));

    // Draw time text
    uint32_t ticks_elapsed = furi_get_tick() - model->start_tick;
    uint32_t sec = ticks_elapsed / furi_kernel_get_tick_frequency();
    uint32_t minutes = sec / 60;
    sec = sec % 60;

    furi_string_printf(
             model->info_str,
             "%02ld:%02ld",
             minutes,
             sec);

    canvas_draw_str_aligned(
            canvas,
            126 - canvas_string_width(canvas, furi_string_get_cstr(model->info_str)),
            64 - 7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));
}

static void mine_sweeper_game_screen_view_lose_draw_callback(Canvas* canvas, void* _model) {
    furi_assert(canvas);
    furi_assert(_model);
    MineSweeperGameScreenModel* model = _model;

    canvas_clear(canvas);

    uint16_t cursor_pos_1d = model->curr_pos.x_abs * model->board_width + model->curr_pos.y_abs;
    
    for (uint8_t x_rel = 0; x_rel < MINESWEEPER_SCREEN_TILE_HEIGHT; x_rel++) {
        uint16_t x_abs = (model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT) + x_rel;
        
        for (uint8_t y_rel = 0; y_rel < MINESWEEPER_SCREEN_TILE_WIDTH; y_rel++) {
            uint16_t y_abs = (model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH) + y_rel;

            uint16_t curr_rendering_tile_pos_1d = x_abs * model->board_width + y_abs;
            MineSweeperTile tile = model->board[curr_rendering_tile_pos_1d];

            if (cursor_pos_1d == curr_rendering_tile_pos_1d) {
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }

            canvas_draw_icon(
                canvas,
                y_rel * icon_get_width(tile.icon_element.icon),
                x_rel * icon_get_height(tile.icon_element.icon),
                tile.icon_element.icon);

        }
    }

    canvas_set_color(canvas, ColorBlack);
    // If any borders are at the limits of the game board we draw a border line
    
    // Right border 
    if (model->right_boundary == model->board_width) {
        canvas_draw_line(canvas, 127,0,127,63-8);
    }

    // Left border
    if ((model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH) == 0) {
        canvas_draw_line(canvas, 0,0,0,63-8);
    }

    // Bottom border
    if (model->bottom_boundary == model->board_height) {
        canvas_draw_line(canvas, 0,63-8,127,63-8);
    }

    // Top border
    if ((model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT) == 0) {
        canvas_draw_line(canvas, 0,0,127,0);
    }

    // Draw lose text
    furi_string_printf(
            model->info_str,
            "YOU LOSE!");

    canvas_draw_str_aligned(
            canvas,
            0,
            64-7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));

    // Draw time text
    uint32_t ticks_elapsed = furi_get_tick() - model->start_tick;
    uint32_t sec = ticks_elapsed / furi_kernel_get_tick_frequency();
    uint32_t minutes = sec / 60;
    sec = sec % 60;

    furi_string_printf(
             model->info_str,
             "%02ld:%02ld",
             minutes,
             sec);

    canvas_draw_str_aligned(
            canvas,
            126 - canvas_string_width(canvas, furi_string_get_cstr(model->info_str)),
            64 - 7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));
}

static void mine_sweeper_game_screen_view_play_draw_callback(Canvas* canvas, void* _model) {
    furi_assert(canvas);
    furi_assert(_model);

    static double time = 0.0L;
    static uint32_t iter = 0;
    iter++;
    
    uint32_t start_tick = furi_get_tick();

    MineSweeperGameScreenModel* model = _model;

    canvas_clear(canvas);

    
    uint16_t cursor_pos_1d = model->curr_pos.x_abs * model->board_width + model->curr_pos.y_abs;
    
    for (uint8_t x_rel = 0; x_rel < MINESWEEPER_SCREEN_TILE_HEIGHT; x_rel++) {
        uint16_t x_abs = (model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT) + x_rel;
        
        for (uint8_t y_rel = 0; y_rel < MINESWEEPER_SCREEN_TILE_WIDTH; y_rel++) {
            uint16_t y_abs = (model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH) + y_rel;

            uint16_t curr_rendering_tile_pos_1d = x_abs * model->board_width + y_abs;
            MineSweeperTile tile = model->board[curr_rendering_tile_pos_1d];

            if (cursor_pos_1d == curr_rendering_tile_pos_1d) {
                canvas_set_color(canvas, ColorWhite);
            } else {
                canvas_set_color(canvas, ColorBlack);
            }

            switch (tile.tile_state) {

                case MineSweeperGameScreenTileStateFlagged :
                    canvas_draw_icon(
                        canvas,
                        y_rel * icon_get_width(tile.icon_element.icon),
                        x_rel * icon_get_height(tile.icon_element.icon),
                        tile_icons[11]);

                    break;
                case MineSweeperGameScreenTileStateUncleared :
                    canvas_draw_icon(
                        canvas,
                        y_rel * icon_get_width(tile.icon_element.icon),
                        x_rel * icon_get_height(tile.icon_element.icon),
                        tile_icons[12]);

                    break;
                case MineSweeperGameScreenTileStateCleared :
                    canvas_draw_icon(
                        canvas,
                        y_rel * icon_get_width(tile.icon_element.icon),
                        x_rel * icon_get_height(tile.icon_element.icon),
                        tile.icon_element.icon);
                    break;
                default:
                    break;
            }

        }
    }

    canvas_set_color(canvas, ColorBlack);
    // If any borders are at the limits of the game board we draw a border line
    
    // Right border 
    if (model->right_boundary == model->board_width) {
        canvas_draw_line(canvas, 127,0,127,63-8);
    }

    // Left border
    if ((model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH) == 0) {
        canvas_draw_line(canvas, 0,0,0,63-8);
    }

    // Bottom border
    if (model->bottom_boundary == model->board_height) {
        canvas_draw_line(canvas, 0,63-8,127,63-8);
    }

    // Top border
    if ((model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT) == 0) {
        canvas_draw_line(canvas, 0,0,127,0);
    }

    // Draw X Position Text 
    furi_string_printf(
            model->info_str,
            "X:%03hhd",
            model->curr_pos.x_abs);

    canvas_draw_str_aligned(
            canvas,
            0,
            64-7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));

    // Draw Y Position Text 
    furi_string_printf(
            model->info_str,
            "Y:%03hhd",
            model->curr_pos.y_abs);

    canvas_draw_str_aligned(
            canvas,
            33,
            64-7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));

    // Draw flag text
    furi_string_printf(
            model->info_str,
            "F:%03hd",
            model->flags_left);

    canvas_draw_str_aligned(
            canvas,
            66,
            64 - 7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));

    // Draw time text
    uint32_t ticks_elapsed = furi_get_tick() - model->start_tick;
    uint32_t sec = ticks_elapsed / furi_kernel_get_tick_frequency();
    uint32_t minutes = sec / 60;
    sec = sec % 60;

    furi_string_printf(
             model->info_str,
             "%02ld:%02ld",
             minutes,
             sec);

    canvas_draw_str_aligned(
            canvas,
            126 - canvas_string_width(canvas, furi_string_get_cstr(model->info_str)),
            64 - 7,
            AlignLeft,
            AlignTop,
            furi_string_get_cstr(model->info_str));

    uint32_t ticks_elapsed2 = furi_get_tick() - start_tick;
    double sec2 = (double) ticks_elapsed2 / (double) furi_kernel_get_tick_frequency();
    double milliseconds = sec2 * 1000;
    time += milliseconds;

    if (iter % 50 == 0)
        FURI_LOG_D(MS_DEBUG_TAG, "Draw callback avg time: %.03f", time/iter);

}

static bool mine_sweeper_game_screen_view_end_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    furi_assert(event);

    MineSweeperGameScreen* instance = context;
    bool consumed = false;

    if (event->type == InputTypePress || event->type == InputTypeRepeat) {

        with_view_model(
            instance->view,
            MineSweeperGameScreenModel * model,
            {
                bool is_outside_boundary;
                switch (event->key) {

                    case InputKeyUp :
                        model->curr_pos.x_abs = (model->curr_pos.x_abs-1 < 0) ? 0 : model->curr_pos.x_abs-1;

                        is_outside_boundary = model->curr_pos.x_abs <
                            (model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT);
                        
                        if (is_outside_boundary) {
                            model->bottom_boundary--;
                        }

                        consumed = true;
                        break;

                    case InputKeyDown :
                        model->curr_pos.x_abs = (model->curr_pos.x_abs+1 >= model->board_height) ?
                            model->board_height-1 : model->curr_pos.x_abs+1;

                        is_outside_boundary = model->curr_pos.x_abs >= model->bottom_boundary;

                        if (is_outside_boundary) {
                            model->bottom_boundary++;
                        }

                        consumed = true;
                        break;

                    case InputKeyLeft :
                        model->curr_pos.y_abs = (model->curr_pos.y_abs-1 < 0) ? 0 : model->curr_pos.y_abs-1;

                        is_outside_boundary = model->curr_pos.y_abs <
                            (model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH);
                        
                        if (is_outside_boundary) {
                            model->right_boundary--;
                        }

                        consumed = true;
                        break;

                    case InputKeyRight :
                        model->curr_pos.y_abs = (model->curr_pos.y_abs+1 >= model->board_width) ?
                            model->board_width-1 : model->curr_pos.y_abs+1;

                        is_outside_boundary = model->curr_pos.y_abs >= model->right_boundary;

                        if (is_outside_boundary) {
                            model->right_boundary++;
                        }

                        consumed = true;
                        break;

                    default: // Anything other than movement around the screen should restart game
                        
                        mine_sweeper_game_screen_reset_clock(instance);
                        view_set_draw_callback(
                                instance->view,
                                mine_sweeper_game_screen_view_play_draw_callback);
                        view_set_input_callback(
                                instance->view,
                                mine_sweeper_game_screen_view_play_input_callback);

                        setup_board(instance);
                        consumed = true;
                        break;
                }

            },
            false
        );

        consumed = true; 
    }

    return consumed;
}

static bool mine_sweeper_game_screen_view_play_input_callback(InputEvent* event, void* context) {
    furi_assert(context);
    furi_assert(event);

    static double time = 0.0L;
    static uint32_t iter = 0;
    iter++;
    
    uint32_t start_tick = furi_get_tick();


    MineSweeperGameScreen* instance = context;
    bool consumed = false;

    // Checking button types

    if (event->key == InputKeyOk) { // Attempt to Clear Space !! THIS CAN BE A LOSE CONDITION
        
         if (event->type == InputTypePress) { 

            with_view_model(
                instance->view,
                MineSweeperGameScreenModel * model,
                {
                    uint16_t curr_pos_1d = model->curr_pos.x_abs * model->board_width + model->curr_pos.y_abs;
                    MineSweeperGameScreenTileState state = model->board[curr_pos_1d].tile_state;
                    MineSweeperGameScreenTileType type = model->board[curr_pos_1d].tile_type;

                    // LOSE CONDITION OR TILE CLEAR
                    if (state == MineSweeperGameScreenTileStateUncleared && type == MineSweeperGameScreenTileMine) {
                        view_set_draw_callback(instance->view, mine_sweeper_game_screen_view_lose_draw_callback);
                        view_set_input_callback(instance->view, mine_sweeper_game_screen_view_end_input_callback);
                        model->board[curr_pos_1d].tile_state = MineSweeperGameScreenTileStateCleared;

                    } else if (state == MineSweeperGameScreenTileStateUncleared) {
                        model->board[curr_pos_1d].tile_state = MineSweeperGameScreenTileStateCleared;
                        
                        bfs_tile_clear(model);
                    }

                },
                true
            );

        }

        consumed = true;

    }

    if (!consumed && (event->key == InputKeyBack)) { // We can use holding the back button for either
                                                   // Setting a flag on a covered tile, or moving to
                                                   // the next closest covered tile on when on a uncovered
                                                   // tile

        if (event->type == InputTypeRelease) {
            with_view_model(
                instance->view,
                MineSweeperGameScreenModel * model,
                {
                    model->is_holding_down_button = false;
                },
                true
            );
            
            consumed = true;

        } else if (event->type == InputTypeLong || event->type == InputTypeRepeat) {    // Only process longer back keys;
                                                                                        // short presses should take
                                                                                        // us to the menu
            with_view_model(
                instance->view,
                MineSweeperGameScreenModel * model,
                {
                    uint16_t curr_pos_1d = model->curr_pos.x_abs * model->board_width + model->curr_pos.y_abs;
                    
                    MineSweeperGameScreenTileState state = model->board[curr_pos_1d].tile_state;

                    if (state == MineSweeperGameScreenTileStateCleared) {

                        // BFS to closest uncovered position
                        Point res = bfs_to_closest_tile(model);

                        // Save cursor to new closest tile position
                        // If the cursor moves outisde of the model boundaries we need to
                        // move the boundary appropriately
                        
                        model->curr_pos.x_abs = res.x;
                        model->curr_pos.y_abs = res.y;

                        bool is_outside_top_boundary = model->curr_pos.x_abs <
                            (model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT);

                        bool is_outside_bottom_boundary = model->curr_pos.x_abs >=
                            model->bottom_boundary;

                        bool is_outside_left_boundary = model->curr_pos.y_abs <
                            (model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH);

                        bool is_outside_right_boundary = model->curr_pos.y_abs >=
                            model->right_boundary;

                        if (is_outside_top_boundary) {
                            model->bottom_boundary = model->curr_pos.x_abs + MINESWEEPER_SCREEN_TILE_HEIGHT;
                        } else if (is_outside_bottom_boundary) {
                            model->bottom_boundary = model->curr_pos.x_abs+1;
                        }

                        if (is_outside_right_boundary) {
                            model->right_boundary = model->curr_pos.y_abs+1;
                        } else if (is_outside_left_boundary) {
                            model->right_boundary = model->curr_pos.y_abs + MINESWEEPER_SCREEN_TILE_WIDTH;
                        }
                        
                        model->is_holding_down_button = true;

                    // Flag or Unflag tile and check win condition 
                    } else if (!model->is_holding_down_button && (state == MineSweeperGameScreenTileStateUncleared || state == MineSweeperGameScreenTileStateFlagged)) { 

                        if (state == MineSweeperGameScreenTileStateFlagged) {
                            if (model->board[curr_pos_1d].tile_type == MineSweeperGameScreenTileMine) model->mines_left++;
                            model->board[curr_pos_1d].tile_state = MineSweeperGameScreenTileStateUncleared;
                            model->flags_left++;
                            model->is_holding_down_button = true;
                        
                        } else if (model->flags_left > 0) {
                            if (model->board[curr_pos_1d].tile_type == MineSweeperGameScreenTileMine) model->mines_left--;
                            model->board[curr_pos_1d].tile_state = MineSweeperGameScreenTileStateFlagged;
                            model->flags_left--;
                            model->is_holding_down_button = true;
                        }

                        // WIN CONDITION
                        if (model->flags_left == 0 && model->mines_left == 0) {
                            view_set_draw_callback(instance->view, mine_sweeper_game_screen_view_win_draw_callback);
                            view_set_input_callback(instance->view, mine_sweeper_game_screen_view_end_input_callback);
                        }

                    }
                },
                false
            );

            consumed = true;

        }
    }

    if (!consumed && (event->type == InputTypePress || event->type == InputTypeRepeat)) { // Finally handle move

        with_view_model(
            instance->view,
            MineSweeperGameScreenModel * model,
            {
                bool is_outside_boundary;
                switch (event->key) {

                    case InputKeyUp :
                        model->curr_pos.x_abs = (model->curr_pos.x_abs-1 < 0) ? 0 : model->curr_pos.x_abs-1;

                        is_outside_boundary = model->curr_pos.x_abs <
                            (model->bottom_boundary - MINESWEEPER_SCREEN_TILE_HEIGHT);
                        
                        if (is_outside_boundary) {
                            model->bottom_boundary--;
                        }

                        consumed = true;
                        break;

                    case InputKeyDown :
                        model->curr_pos.x_abs = (model->curr_pos.x_abs+1 >= model->board_height) ?
                            model->board_height-1 : model->curr_pos.x_abs+1;

                        is_outside_boundary = model->curr_pos.x_abs >= model->bottom_boundary;

                        if (is_outside_boundary) {
                            model->bottom_boundary++;
                        }

                        consumed = true;
                        break;

                    case InputKeyLeft :
                        model->curr_pos.y_abs = (model->curr_pos.y_abs-1 < 0) ? 0 : model->curr_pos.y_abs-1;

                        is_outside_boundary = model->curr_pos.y_abs <
                            (model->right_boundary - MINESWEEPER_SCREEN_TILE_WIDTH);
                        
                        if (is_outside_boundary) {
                            model->right_boundary--;
                        }

                        consumed = true;
                        break;

                    case InputKeyRight :
                        model->curr_pos.y_abs = (model->curr_pos.y_abs+1 >= model->board_width) ?
                            model->board_width-1 : model->curr_pos.y_abs+1;

                        is_outside_boundary = model->curr_pos.y_abs >= model->right_boundary;

                        if (is_outside_boundary) {
                            model->right_boundary++;
                        }

                        consumed = true;
                        break;

                    default:
                        break;
                }
            },
            true
        );
    }
    

    if (!consumed && instance->input_callback != NULL) {
        consumed = instance->input_callback(event, instance->context);
    }

    uint32_t ticks_elapsed = furi_get_tick() - start_tick;
    double sec = (double) ticks_elapsed / (double) furi_kernel_get_tick_frequency();
    double milliseconds = sec * 1000;
    time += milliseconds;

    if (iter%3==0)
        FURI_LOG_D(MS_DEBUG_TAG, "Input callback avg time: %.03f", time/iter);

    return consumed;
}

MineSweeperGameScreen* mine_sweeper_game_screen_alloc(uint8_t width, uint8_t height, uint8_t difficulty) {
    MineSweeperGameScreen* mine_sweeper_game_screen = (MineSweeperGameScreen*)malloc(sizeof(MineSweeperGameScreen));

    mine_sweeper_game_screen->view = view_alloc();

    view_set_context(mine_sweeper_game_screen->view, mine_sweeper_game_screen);
    view_allocate_model(mine_sweeper_game_screen->view, ViewModelTypeLocking, sizeof(MineSweeperGameScreenModel));

    view_set_draw_callback(mine_sweeper_game_screen->view, mine_sweeper_game_screen_view_play_draw_callback);
    view_set_input_callback(mine_sweeper_game_screen->view, mine_sweeper_game_screen_view_play_input_callback);
    
    // This are currently unused
    view_set_enter_callback(mine_sweeper_game_screen->view, mine_sweeper_game_screen_view_enter);
    view_set_exit_callback(mine_sweeper_game_screen->view, mine_sweeper_game_screen_view_exit);

    // Secondary Input callback can be used to simplify input/draw callback
    // for play/loss/win into one main callback funtion that calls the appropriate
    // secondary callback
    mine_sweeper_game_screen->input_callback = NULL;

    // Allocate strings in model
    with_view_model(
        mine_sweeper_game_screen->view,
        MineSweeperGameScreenModel * model,
        {
            model->info_str = furi_string_alloc();
        },
        true
    );

    // Reset the clock - This will set the start time at the allocation of the game screen
    // but this is a public api as well and can be called in a scene for more accurate start times
    mine_sweeper_game_screen_reset_clock(mine_sweeper_game_screen);

    // We need to initize board width and height before setup
    mine_sweeper_game_screen_set_board_information(mine_sweeper_game_screen, width, height, difficulty);

    setup_board(mine_sweeper_game_screen);
    
    return mine_sweeper_game_screen;
}

void mine_sweeper_game_screen_free(MineSweeperGameScreen* instance) {
    furi_assert(instance);

    // Dealloc strings in model
    with_view_model(
        instance->view,
        MineSweeperGameScreenModel * model,
        {
            furi_string_free(model->info_str);
        },
        false
    );

    // Free view and any dynamically allocated members in main struct
    view_free(instance->view);
    free(instance);
}

// This function should be called whenever you want to reset the game state
// This should NOT be called in the on_exit in the game scene
void mine_sweeper_game_screen_reset(MineSweeperGameScreen* instance, uint8_t width, uint8_t height, uint8_t difficulty) {
    furi_assert(instance);
    
    instance->input_callback = NULL;
    
    // We need to initize board width and height before setup
    mine_sweeper_game_screen_set_board_information(instance, width, height, difficulty);

    mine_sweeper_game_screen_reset_clock(instance);

    setup_board(instance);

}

// This function should be called when you want to reset the game clock
// Already called in reset and alloc function for game, but can be called from
// other scenes that need it like a start scene that plays after alloc
void mine_sweeper_game_screen_reset_clock(MineSweeperGameScreen* instance) {
    furi_assert(instance);

    with_view_model(
        instance->view,
        MineSweeperGameScreenModel * model,
        {
            model->start_tick = furi_get_tick();
        },
        true
    );
}

View* mine_sweeper_game_screen_get_view(MineSweeperGameScreen* instance) {
    furi_assert(instance);
    return instance->view;
}

void mine_sweeper_game_screen_set_context(MineSweeperGameScreen* instance, void* context) {
    furi_assert(instance);
    instance->context = context;
}
