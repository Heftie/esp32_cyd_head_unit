#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// A small registry + back stack, so a screen enters another one by name
// (screen_push()) instead of calling its constructor directly — this is
// the piece that lets touch_test_screen's Back button just call
// screen_pop() and correctly land wherever it was actually entered from
// (tiles directly, or settings, or anything else later), without knowing
// or caring who that was. Private to the ui component: every screen file
// includes this, nothing outside components/ui needs to.

typedef void (*screen_create_fn_t)(void);

#define SCREEN_HOME "tiles"

// Registers `name` -> `create`. Call once per screen from ui_init(),
// before any screen_push()/screen_activate() can reach it.
void screen_register(const char *name, screen_create_fn_t create);

// Enters `name` directly, with no back-stack entry. Only meant for the
// very first screen shown at boot — every other transition should go
// through screen_push() so Back has somewhere to return to.
void screen_activate(const char *name);

// Enters `name`, remembering the current screen so screen_pop() can
// return to it. Use this from a screen that leads somewhere new (e.g. a
// Settings button) instead of calling the target screen's create
// function directly.
void screen_push(const char *name);

// Returns to whichever screen called screen_push() to get here. Falls
// back to SCREEN_HOME if the back stack is empty.
void screen_pop(void);

#ifdef __cplusplus
}
#endif
