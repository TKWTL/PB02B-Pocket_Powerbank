/* menu_anim.h - shared lightweight PID animation for menu and functions.
 * Header-only: no extra object file, heap, LVGL animation descriptors or timers.
 * A UI owner provides its own 10ms tick and per-axis state.
 * For simple label entrance/exit, use menu_anim_func_init() then
 * menu_anim_step()/menu_anim_step_limit() + lv_obj_set_x/y().
 * LVGL's lv_anim_t can also move objects, but this reuses the current menu
 * PID, avoids additional callbacks/descriptors and retains existing motion.
 */
#ifndef MENU_ANIM_H
#define MENU_ANIM_H

#include <stdbool.h>
#include <math.h>

#define MENU_ANIM_TICK_MS           10U
#define MENU_ANIM_SCROLL_UNITS      20U
#define MENU_ANIM_SCROLL_MIN_MS    300U
#define MENU_ANIM_SCROLL_MAX_MS  10000U

/* Position PID: integral remains 0 to avoid accumulated positional overshoot. */
#define MENU_ANIM_MENU_KP          0.25f
#define MENU_ANIM_MENU_KI          0.0f
#define MENU_ANIM_MENU_KD          0.05f
#define MENU_ANIM_FUNC_KP          0.25f   /* shared across all functions */
#define MENU_ANIM_FUNC_KI          0.0f
#define MENU_ANIM_FUNC_KD          0.05f
#define MENU_ANIM_INT_LIMIT       80.0f

typedef struct {
    float kp, ki, kd;
    float current, target, integral, last_error;
    bool active;
} menu_anim_pid_t;

static inline void menu_anim_init(menu_anim_pid_t *p, float from, float to)
{
    p->kp = MENU_ANIM_MENU_KP;
    p->ki = MENU_ANIM_MENU_KI;
    p->kd = MENU_ANIM_MENU_KD;
    p->current = from;
    p->target = to;
    p->integral = 0.0f;
    p->last_error = 0.0f;
    p->active = true;
}

static inline void menu_anim_func_init(menu_anim_pid_t *p, float from, float to)
{
    menu_anim_init(p, from, to);
    p->kp = MENU_ANIM_FUNC_KP;
    p->ki = MENU_ANIM_FUNC_KI;
    p->kd = MENU_ANIM_FUNC_KD;
}

/* max_step = 0: unrestricted; positive: pixel/frame displacement limit. */
static inline bool menu_anim_step_limit(menu_anim_pid_t *p, float max_step)
{
    float err, vel;
    if (!p->active) return false;
    err = p->target - p->current;
    p->integral += err;
    if (p->integral > MENU_ANIM_INT_LIMIT) p->integral = MENU_ANIM_INT_LIMIT;
    if (p->integral < -MENU_ANIM_INT_LIMIT) p->integral = -MENU_ANIM_INT_LIMIT;
    vel = p->kp * err + p->ki * p->integral +
          p->kd * (err - p->last_error);
    p->last_error = err;
    if (!isfinite(p->current) || !isfinite(p->target) || !isfinite(vel)) {
        p->current = p->target;
        p->active = false;
        return false;
    }
    if (max_step > 0.0f) {
        if (vel > max_step) vel = max_step;
        if (vel < -max_step) vel = -max_step;
    }
    p->current += vel;
    if (fabsf(err) < 0.5f && fabsf(vel) < 0.5f) {
        p->current = p->target;
        p->active = false;
    }
    return p->active;
}

static inline bool menu_anim_step(menu_anim_pid_t *p)
{
    return menu_anim_step_limit(p, 0.0f);
}

#endif /* MENU_ANIM_H */
