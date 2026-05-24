#ifndef SC_KEYMAPPER_H
#define SC_KEYMAPPER_H

#include <stdbool.h>
#include <SDL3/SDL.h>
#include "controller.h"
#include "coords.h"

// Keymap node types
typedef enum {
    KMT_UNKNOWN = 0,
    KMT_CLICK,
    KMT_CLICK_TWICE,
    KMT_CLICK_MULTI,
    KMT_DRAG,
    KMT_STEER_WHEEL,
    KMT_MOUSE_MOVE
} ScKeymapType;

typedef struct { float x; float y; } ScNormPoint;

#define MAX_NODES 32
#define MAX_TICK_JOBS 16
#define MAX_MULTI_CLICKS 5

typedef struct {
    int delay_ms;
    ScNormPoint pos;
} ScMultiClickNode;

// Internal active job for the Non-blocking Tick Engine
typedef struct ScTickJob {
    bool active;
    ScKeymapType type;
    uint64_t start_time_ms;
    uint64_t last_action_ms;
    int step; 
    int pointer_id;
    int node_index;
    bool is_auto_swipe;
    ScNormPoint start_pos;
    ScNormPoint end_pos;
    float speed;
} ScTickJob;

struct sc_keymapper {
    struct sc_controller *controller;
    
    // Opaque Properties Encapsulated
    struct sc_size frame_size; 
    SDL_Window *window;

    bool enabled;
    bool show_hud;
    bool mouse_locked;
    
    SDL_Keycode switch_key;
    uint8_t switch_mouse_btn;

    // Pointer ID Tracker
    bool pointer_ids_in_use[10];

    // Engine Nodes
    struct {
        ScKeymapType type;
        SDL_Keycode key;
        uint8_t mouse_btn;
        ScNormPoint pos;
        ScNormPoint end_pos;
        int active_pointer_id; 

        // Drag specific
        bool is_auto_swipe;

        // Steering Wheel precise offsets
        ScNormPoint center_pos;
        SDL_Keycode up, down, left, right;
        SDL_Scancode up_scan, down_scan, left_scan, right_scan;
        float up_offset, down_offset, left_offset, right_offset;
        int steer_pointer_id;
        
        // Mouse Move specific
        float speed_ratio_x, speed_ratio_y;
        SDL_Keycode small_eyes_key;
        SDL_Scancode small_eyes_scan;
        ScNormPoint small_eyes_pos;
        ScNormPoint locked_aim_origin;
        ScNormPoint current_aim_pos;
        int aim_pointer_id;

        // Multi-click
        ScMultiClickNode multi_clicks[MAX_MULTI_CLICKS];
        int multi_click_count;
    } nodes[MAX_NODES];
    
    int node_count;
    ScTickJob tick_jobs[MAX_TICK_JOBS];
    uint64_t last_tick_ms;
};

// Public API
void sc_keymapper_init(struct sc_keymapper *km, struct sc_controller *controller);
void sc_keymapper_destroy(struct sc_keymapper *km);
bool sc_keymapper_load_json(struct sc_keymapper *km, const char *json_path);

// Hooks
bool sc_keymapper_process_event(struct sc_keymapper *km, const SDL_Event *event);
void sc_keymapper_tick(struct sc_keymapper *km, uint64_t current_time_ms);
void sc_keymapper_render_hud(struct sc_keymapper *km, SDL_Renderer *renderer);

#endif // SC_KEYMAPPER_H
