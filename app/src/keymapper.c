#include <string.h>
#include "android/input.h"
#include "keymapper.h"
#include "control_msg.h"

// ---------------------------------------------------------
// PART A: Key String Translation
// ---------------------------------------------------------
static struct { const char *qt; SDL_Keycode sdl; } KEY_MAP[] = {
    {"Key_Space", SDLK_SPACE}, {"Key_Shift", SDLK_LSHIFT}, {"Key_Control", SDLK_LCTRL},
    {"Key_Alt", SDLK_LALT}, {"Key_QuoteLeft", SDLK_GRAVE}, {"Key_Escape", SDLK_ESCAPE},
    {"Key_Tab", SDLK_TAB}, {"Key_Equal", SDLK_EQUALS},
    {"Key_Up", SDLK_UP}, {"Key_Down", SDLK_DOWN}, {"Key_Left", SDLK_LEFT}, {"Key_Right", SDLK_RIGHT},
    {"Key_F1", SDLK_F1}, {"Key_F2", SDLK_F2}, {"Key_F3", SDLK_F3}, {"Key_F4", SDLK_F4}
};

static SDL_Keycode parse_qt_key(const char *qt_key) {
    if (strncmp(qt_key, "Key_", 4) == 0 && qt_key[4] != '\0' && qt_key[5] == '\0') {
        char c = qt_key[4];
        if (c >= 'A' && c <= 'Z') return (SDL_Keycode)(c + 32); 
        if (c >= '0' && c <= '9') return (SDL_Keycode)c;        
    }
    for (size_t i = 0; i < sizeof(KEY_MAP)/sizeof(KEY_MAP[0]); ++i) {
        if (strcmp(KEY_MAP[i].qt, qt_key) == 0) return KEY_MAP[i].sdl;
    }
    return SDLK_UNKNOWN;
}

// ---------------------------------------------------------
// PART B: Pointer Management & Touch Injection
// ---------------------------------------------------------
static int allocate_pointer_id(struct sc_keymapper *km) {
    for (int i = 0; i < 10; ++i) {
        if (!km->pointer_ids_in_use[i]) {
            km->pointer_ids_in_use[i] = true;
            return i;
        }
    }
    return -1; 
}

static void free_pointer_id(struct sc_keymapper *km, int id) {
    if (id >= 0 && id < 10) km->pointer_ids_in_use[id] = false;
}

static void inject_touch(struct sc_keymapper *km, int action, int pointer_id, float nx, float ny) {
    if (pointer_id < 0) return;
    if (km->frame_size.width == 0 || km->frame_size.height == 0) return;
    struct sc_position position = {
        .point = { .x = (int32_t)(nx * km->frame_size.width), 
                   .y = (int32_t)(ny * km->frame_size.height) },
        .screen_size = km->frame_size
    };
    struct sc_control_msg msg = {
        .type = SC_CONTROL_MSG_TYPE_INJECT_TOUCH_EVENT,
        .inject_touch_event = {
            .action = action,
            .pointer_id = pointer_id,
            .position = position,
            .pressure = (action == AMOTION_EVENT_ACTION_UP) ? 0.0f : 1.0f,
            .buttons = 0
        }
    };
    sc_controller_push_msg(km->controller, &msg);
}

// ---------------------------------------------------------
// PART C: Event Interceptor
// ---------------------------------------------------------
bool sc_keymapper_process_event(struct sc_keymapper *km, const SDL_Event *event) {
    if (!km->enabled) return false;

    // Toggle HUD
    if (event->type == SDL_EVENT_KEY_DOWN && event->key.key == SDLK_H && (event->key.mod & SDL_KMOD_ALT)) {
        km->show_hud = !km->show_hud;
        return true;
    }

    // Relative Mode Switch
    bool is_switch_key = (km->switch_key != SDLK_UNKNOWN) && 
                         (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) && 
                         event->key.key == km->switch_key;
    bool is_switch_mouse = (km->switch_mouse_btn != 0) && 
                           (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_UP) && 
                           event->button.button == km->switch_mouse_btn;

    if (is_switch_key || is_switch_mouse) {
        if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
            km->mouse_locked = !km->mouse_locked;
            SDL_SetWindowRelativeMouseMode(km->window, km->mouse_locked); 
            
            // Sweep all active pointers on unlock to prevent stuck touches
            if (!km->mouse_locked) {
                for (int i = 0; i < km->node_count; ++i) {
                    if (km->nodes[i].type == KMT_MOUSE_MOVE && km->nodes[i].aim_pointer_id != -1) {
                        inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].aim_pointer_id, km->nodes[i].current_aim_pos.x, km->nodes[i].current_aim_pos.y);
                        free_pointer_id(km, km->nodes[i].aim_pointer_id);
                        km->nodes[i].aim_pointer_id = -1;
                    }
                    if (km->nodes[i].active_pointer_id != -1) {
                        inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].active_pointer_id, km->nodes[i].pos.x, km->nodes[i].pos.y);
                        free_pointer_id(km, km->nodes[i].active_pointer_id);
                        km->nodes[i].active_pointer_id = -1;
                    }
                    if (km->nodes[i].steer_pointer_id != -1) {
                        inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].steer_pointer_id, km->nodes[i].center_pos.x, km->nodes[i].center_pos.y);
                        free_pointer_id(km, km->nodes[i].steer_pointer_id);
                        km->nodes[i].steer_pointer_id = -1;
                    }
                }
            }
        }
        return true; 
    }

    // Bypass gaming input if mouse is unlocked (allows normal typing/clicking)
    if (!km->mouse_locked) return false;

    // 360 Mouse Aiming
    if (event->type == SDL_EVENT_MOUSE_MOTION) {
        for (int i = 0; i < km->node_count; ++i) {
            if (km->nodes[i].type == KMT_MOUSE_MOVE) {
                if (km->nodes[i].aim_pointer_id == -1) {
                    km->nodes[i].aim_pointer_id = allocate_pointer_id(km);
                    const bool *state = SDL_GetKeyboardState(NULL);
                    if (state[km->nodes[i].small_eyes_scan]) {
                        km->nodes[i].locked_aim_origin = km->nodes[i].small_eyes_pos;
                    } else {
                        km->nodes[i].locked_aim_origin = km->nodes[i].pos;
                    }
                    km->nodes[i].current_aim_pos = km->nodes[i].locked_aim_origin;
                    inject_touch(km, AMOTION_EVENT_ACTION_DOWN, km->nodes[i].aim_pointer_id, 
                                 km->nodes[i].locked_aim_origin.x, km->nodes[i].locked_aim_origin.y);
                }

                float dx = (event->motion.xrel * km->nodes[i].speed_ratio_x) / km->frame_size.width;
                float dy = (event->motion.yrel * km->nodes[i].speed_ratio_y) / km->frame_size.height;
                
                km->nodes[i].current_aim_pos.x += dx;
                km->nodes[i].current_aim_pos.y += dy;

                float dist_x = km->nodes[i].current_aim_pos.x - km->nodes[i].locked_aim_origin.x;
                float dist_y = km->nodes[i].current_aim_pos.y - km->nodes[i].locked_aim_origin.y;
                
                // Reset threshold > 0.2 (0.2^2 = 0.04)
                if ((dist_x * dist_x + dist_y * dist_y) > 0.04f) { 
                    inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].aim_pointer_id, km->nodes[i].current_aim_pos.x, km->nodes[i].current_aim_pos.y);
                    km->nodes[i].current_aim_pos = km->nodes[i].locked_aim_origin; 
                    inject_touch(km, AMOTION_EVENT_ACTION_DOWN, km->nodes[i].aim_pointer_id, km->nodes[i].locked_aim_origin.x, km->nodes[i].locked_aim_origin.y);
                } else {
                    inject_touch(km, AMOTION_EVENT_ACTION_MOVE, km->nodes[i].aim_pointer_id, km->nodes[i].current_aim_pos.x, km->nodes[i].current_aim_pos.y);
                }
                return true;
            }
        }
    }

    // Keyboard bindings
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        SDL_Keycode k = event->key.key;
        int action = (event->type == SDL_EVENT_KEY_DOWN) ? AMOTION_EVENT_ACTION_DOWN : AMOTION_EVENT_ACTION_UP;

        for (int i = 0; i < km->node_count; ++i) {
            
            // Freelook modifier transition hook
            if (km->nodes[i].type == KMT_MOUSE_MOVE && km->nodes[i].small_eyes_key == k) {
                if (km->nodes[i].aim_pointer_id != -1) {
                    inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].aim_pointer_id, km->nodes[i].current_aim_pos.x, km->nodes[i].current_aim_pos.y);
                    free_pointer_id(km, km->nodes[i].aim_pointer_id);
                    km->nodes[i].aim_pointer_id = -1; 
                }
                return true; 
            }

            // Standard Click
            if (km->nodes[i].type == KMT_CLICK && km->nodes[i].key == k) {
                if (action == AMOTION_EVENT_ACTION_DOWN && km->nodes[i].active_pointer_id == -1) {
                    km->nodes[i].active_pointer_id = allocate_pointer_id(km);
                    inject_touch(km, AMOTION_EVENT_ACTION_DOWN, km->nodes[i].active_pointer_id, km->nodes[i].pos.x, km->nodes[i].pos.y);
                } else if (action == AMOTION_EVENT_ACTION_UP && km->nodes[i].active_pointer_id != -1) {
                    inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].active_pointer_id, km->nodes[i].pos.x, km->nodes[i].pos.y);
                    free_pointer_id(km, km->nodes[i].active_pointer_id);
                    km->nodes[i].active_pointer_id = -1;
                }
                return true;
            }

            // Drag / Swipe Gestures
            if (km->nodes[i].type == KMT_DRAG && km->nodes[i].key == k) {
                if (action == AMOTION_EVENT_ACTION_DOWN && km->nodes[i].active_pointer_id == -1) {
                    km->nodes[i].active_pointer_id = allocate_pointer_id(km);
                    inject_touch(km, AMOTION_EVENT_ACTION_DOWN, km->nodes[i].active_pointer_id, km->nodes[i].pos.x, km->nodes[i].pos.y);
                    
                    for (int j = 0; j < MAX_TICK_JOBS; ++j) {
                        if (!km->tick_jobs[j].active) {
                            km->tick_jobs[j] = (ScTickJob){
                                .active = true, .type = KMT_DRAG, 
                                .node_index = i,
                                .start_time_ms = SDL_GetTicks(),
                                .pointer_id = km->nodes[i].active_pointer_id,
                                .start_pos = km->nodes[i].pos,
                                .end_pos = km->nodes[i].end_pos,
                                .speed = 200.0f,
                                .is_auto_swipe = km->nodes[i].is_auto_swipe
                            };
                            break;
                        }
                    }
                } else if (action == AMOTION_EVENT_ACTION_UP && km->nodes[i].active_pointer_id != -1) {
                    if (km->nodes[i].is_auto_swipe) {
                        km->nodes[i].active_pointer_id = -1; // Unlink, let tick finish
                    } else {
                        // Hold-to-aim cancel
                        float release_x = km->nodes[i].end_pos.x;
                        float release_y = km->nodes[i].end_pos.y;

                        for (int j = 0; j < MAX_TICK_JOBS; ++j) {
                            if (km->tick_jobs[j].active && km->tick_jobs[j].type == KMT_DRAG && km->tick_jobs[j].node_index == i) {
                                km->tick_jobs[j].active = false;
                                uint64_t elapsed = SDL_GetTicks() - km->tick_jobs[j].start_time_ms;
                                float progress = (float)elapsed / km->tick_jobs[j].speed;
                                if (progress < 1.0f) {
                                    release_x = km->tick_jobs[j].start_pos.x + (km->tick_jobs[j].end_pos.x - km->tick_jobs[j].start_pos.x) * progress;
                                    release_y = km->tick_jobs[j].start_pos.y + (km->tick_jobs[j].end_pos.y - km->tick_jobs[j].start_pos.y) * progress;
                                }
                                break;
                            }
                        }
                        inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].active_pointer_id, release_x, release_y);
                        free_pointer_id(km, km->nodes[i].active_pointer_id);
                        km->nodes[i].active_pointer_id = -1;
                    }
                }
                return true;
            }

            // Click Twice Trigger
            if (km->nodes[i].type == KMT_CLICK_TWICE && km->nodes[i].key == k && action == AMOTION_EVENT_ACTION_DOWN) {
                for(int j=0; j<MAX_TICK_JOBS; ++j) {
                    if (!km->tick_jobs[j].active) {
                        km->tick_jobs[j] = (ScTickJob){
                            .active = true, .type = KMT_CLICK_TWICE, .step = 0,
                            .start_time_ms = SDL_GetTicks(),
                            .pointer_id = allocate_pointer_id(km),
                            .start_pos = km->nodes[i].pos
                        };
                        inject_touch(km, AMOTION_EVENT_ACTION_DOWN, km->tick_jobs[j].pointer_id, km->nodes[i].pos.x, km->nodes[i].pos.y);
                        break;
                    }
                }
                return true;
            }

            // Click Multi Trigger
            if (km->nodes[i].type == KMT_CLICK_MULTI && km->nodes[i].key == k && action == AMOTION_EVENT_ACTION_DOWN) {
                for (int j = 0; j < MAX_TICK_JOBS; ++j) {
                    if (!km->tick_jobs[j].active) {
                        km->tick_jobs[j] = (ScTickJob){
                            .active = true, .type = KMT_CLICK_MULTI, .step = 0,
                            .start_time_ms = SDL_GetTicks(),
                            .last_action_ms = SDL_GetTicks(),
                            .pointer_id = allocate_pointer_id(km),
                            .node_index = i
                        };
                        break;
                    }
                }
                return true;
            }

            // WASD Steering Wheel
            if (km->nodes[i].type == KMT_STEER_WHEEL && (k == km->nodes[i].up || k == km->nodes[i].down || k == km->nodes[i].left || k == km->nodes[i].right)) {
                if (action == AMOTION_EVENT_ACTION_DOWN && km->nodes[i].steer_pointer_id == -1) {
                    km->nodes[i].steer_pointer_id = allocate_pointer_id(km);
                    inject_touch(km, AMOTION_EVENT_ACTION_DOWN, km->nodes[i].steer_pointer_id, km->nodes[i].center_pos.x, km->nodes[i].center_pos.y);
                }
                
                const bool *state = SDL_GetKeyboardState(NULL);
                float vx = km->nodes[i].center_pos.x;
                float vy = km->nodes[i].center_pos.y;
                bool moving = false;

                if (state[km->nodes[i].up_scan])    { vy -= km->nodes[i].up_offset; moving = true; } 
                if (state[km->nodes[i].down_scan])  { vy += km->nodes[i].down_offset; moving = true; }
                if (state[km->nodes[i].left_scan])  { vx -= km->nodes[i].left_offset; moving = true; }
                if (state[km->nodes[i].right_scan]) { vx += km->nodes[i].right_offset; moving = true; }

                if (moving) {
                    inject_touch(km, AMOTION_EVENT_ACTION_MOVE, km->nodes[i].steer_pointer_id, vx, vy);
                } else {
                    inject_touch(km, AMOTION_EVENT_ACTION_UP, km->nodes[i].steer_pointer_id, vx, vy);
                    free_pointer_id(km, km->nodes[i].steer_pointer_id);
                    km->nodes[i].steer_pointer_id = -1;
                }
                return true;
            }
        }
    }
    return false;
}

// ---------------------------------------------------------
// PART D: Non-blocking Tick Engine
// ---------------------------------------------------------
void sc_keymapper_tick(struct sc_keymapper *km, uint64_t current_time_ms) {
    if (!km->enabled) return;

    for (int i = 0; i < MAX_TICK_JOBS; ++i) {
        ScTickJob *job = &km->tick_jobs[i];
        if (!job->active) continue;

        uint64_t elapsed = current_time_ms - job->start_time_ms;

        if (job->type == KMT_DRAG) {
            float progress = (float)elapsed / job->speed; 
            if (progress >= 1.0f) {
                inject_touch(km, AMOTION_EVENT_ACTION_MOVE, job->pointer_id, job->end_pos.x, job->end_pos.y);
                if (job->is_auto_swipe) {
                    inject_touch(km, AMOTION_EVENT_ACTION_UP, job->pointer_id, job->end_pos.x, job->end_pos.y);
                    free_pointer_id(km, job->pointer_id);
                }
                job->active = false; 
            } else {
                float cx = job->start_pos.x + (job->end_pos.x - job->start_pos.x) * progress;
                float cy = job->start_pos.y + (job->end_pos.y - job->start_pos.y) * progress;
                inject_touch(km, AMOTION_EVENT_ACTION_MOVE, job->pointer_id, cx, cy);
            }
        } 
        else if (job->type == KMT_CLICK_MULTI) {
            int idx = job->node_index;
            uint64_t step_elapsed = current_time_ms - job->last_action_ms; 
            
            if (job->step < km->nodes[idx].multi_click_count) {
                if (step_elapsed >= (uint64_t)km->nodes[idx].multi_clicks[job->step].delay_ms) {
                    ScNormPoint t_pos = km->nodes[idx].multi_clicks[job->step].pos;
                    inject_touch(km, AMOTION_EVENT_ACTION_DOWN, job->pointer_id, t_pos.x, t_pos.y);
                    inject_touch(km, AMOTION_EVENT_ACTION_UP, job->pointer_id, t_pos.x, t_pos.y);
                    
                    job->last_action_ms = current_time_ms; 
                    job->step++;
                }
            } else {
                free_pointer_id(km, job->pointer_id);
                job->active = false;
            }
        }
        else if (job->type == KMT_CLICK_TWICE) {
            if (job->step == 0 && elapsed > 50) {
                inject_touch(km, AMOTION_EVENT_ACTION_UP, job->pointer_id, job->start_pos.x, job->start_pos.y);
                job->step++;
            } else if (job->step == 1 && elapsed > 100) {
                inject_touch(km, AMOTION_EVENT_ACTION_DOWN, job->pointer_id, job->start_pos.x, job->start_pos.y);
                job->step++;
            } else if (job->step == 2 && elapsed > 150) {
                inject_touch(km, AMOTION_EVENT_ACTION_UP, job->pointer_id, job->start_pos.x, job->start_pos.y);
                free_pointer_id(km, job->pointer_id);
                job->active = false;
            }
        }
    }
}

// ---------------------------------------------------------
// PART E: HUD Renderer
// ---------------------------------------------------------
void sc_keymapper_render_hud(struct sc_keymapper *km, SDL_Renderer *renderer) {
    if (!km->show_hud || !km->enabled) return;

    int w, h;
    SDL_GetRenderOutputSize(renderer, &w, &h);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    for (int i = 0; i < km->node_count; ++i) {
        float px = km->nodes[i].pos.x * w;
        float py = km->nodes[i].pos.y * h;

        if (km->nodes[i].type == KMT_CLICK || km->nodes[i].type == KMT_DRAG) {
            SDL_SetRenderDrawColor(renderer, 255, 255, 255, 100);
            SDL_FRect rect = { px - 20, py - 20, 40, 40 };
            SDL_RenderFillRect(renderer, &rect);
            SDL_SetRenderDrawColor(renderer, 0, 255, 0, 200);
            SDL_RenderRect(renderer, &rect);
        } 
        else if (km->nodes[i].type == KMT_STEER_WHEEL) {
            float cx = km->nodes[i].center_pos.x * w;
            float cy = km->nodes[i].center_pos.y * h;
            SDL_SetRenderDrawColor(renderer, 100, 200, 255, 50);
            SDL_FRect wheel_rect = { cx - 60, cy - 60, 120, 120 };
            SDL_RenderFillRect(renderer, &wheel_rect);
            SDL_SetRenderDrawColor(renderer, 100, 200, 255, 200);
            SDL_RenderRect(renderer, &wheel_rect);
        }
    }
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_NONE);
}

// Note: Ensure your cJSON parser assigns km->nodes[i].small_eyes_scan = SDL_GetScancodeFromKey(...) 
// and ensures pointers like aim_pointer_id initialize to -1.
bool sc_keymapper_load_json(struct sc_keymapper *km, const char *json_path) {
    (void) km;
    (void) json_path;
    // Drop your specific cJSON mapping loop here, using parse_qt_key()
    return true; 
}
void sc_keymapper_init(struct sc_keymapper *km, struct sc_controller *controller) {
    memset(km, 0, sizeof(*km));
    km->controller = controller;
    km->enabled = true;
    for (int i = 0; i < MAX_NODES; ++i) {
        km->nodes[i].aim_pointer_id = -1;
        km->nodes[i].active_pointer_id = -1;
        km->nodes[i].steer_pointer_id = -1;
    }
}
void sc_keymapper_destroy(struct sc_keymapper *km) {
    (void) km;
}
