/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2010-2018 - Michael Lelli
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <boolean.h>
#include <retro_miscellaneous.h>
#include <encodings/crc32.h>
#include <encodings/utf.h>

#include <emscripten/emscripten.h>
#include <emscripten/html5.h>
#include "../../frontend/drivers/platform_emscripten.h"

#include "../input_driver.h"
#include "../input_types.h"
#include "../input_keymaps.h"

#include "../../tasks/tasks_internal.h"
#include "../../configuration.h"
#include "../../retroarch.h"
#include "../../verbosity.h"
#include "../../command.h"

/* https://developer.mozilla.org/en-US/docs/Web/API/MouseEvent/button */
#define RWEBINPUT_MOUSE_BTNL 0
#define RWEBINPUT_MOUSE_BTNM 1
#define RWEBINPUT_MOUSE_BTNR 2
#define RWEBINPUT_MOUSE_BTN4 3
#define RWEBINPUT_MOUSE_BTN5 4

#define MAX_TOUCH 32

typedef struct rwebinput_key_to_code_map_entry
{
   const char *key;
   enum retro_key rk;
} rwebinput_key_to_code_map_entry_t;

typedef struct rwebinput_keyboard_event
{
   int type;
   EmscriptenKeyboardEvent event;
} rwebinput_keyboard_event_t;

typedef struct rwebinput_keyboard_event_queue
{
   rwebinput_keyboard_event_t *events;
   size_t count;
   size_t max_size;
} rwebinput_keyboard_event_queue_t;

typedef struct rwebinput_pointer_states
{
   int x;
   int y;
   int id;
} rwebinput_pointer_state_t;

typedef struct rwebinput_mouse_states
{
   double pending_scroll_x;
   double pending_scroll_y;
   double scroll_x;
   double scroll_y;
   int x;
   int y;
   int pending_delta_x;
   int pending_delta_y;
   int delta_x;
   int delta_y;
   uint8_t buttons;
} rwebinput_mouse_state_t;

typedef struct rwebinput_motion_states
{
   float x;
   float y;
   float z;
   bool enabled;
} rwebinput_motion_state_t;

typedef struct rwebinput_input
{
   rwebinput_mouse_state_t mouse;                /* double alignment */
   rwebinput_keyboard_event_queue_t keyboard;    /* ptr alignment */
   rwebinput_pointer_state_t pointer[MAX_TOUCH]; /* int alignment */
   rwebinput_motion_state_t accelerometer;       /* float alignment */
   rwebinput_motion_state_t gyroscope;           /* float alignment */
   unsigned pointer_count;
   bool keys[RETROK_LAST];
   bool pointerlock_active;
} rwebinput_input_t;

/* KeyboardEvent.keyCode has been deprecated for a while and doesn't have
 * separate left/right modifier codes, so we have to map string labels from
 * KeyboardEvent.code to retro keys */
static const rwebinput_key_to_code_map_entry_t rwebinput_key_to_code_map[] =
{
   { "KeyA", RETROK_a },
   { "KeyB", RETROK_b },
   { "KeyC", RETROK_c },
   { "KeyD", RETROK_d },
   { "KeyE", RETROK_e },
   { "KeyF", RETROK_f },
   { "KeyG", RETROK_g },
   { "KeyH", RETROK_h },
   { "KeyI", RETROK_i },
   { "KeyJ", RETROK_j },
   { "KeyK", RETROK_k },
   { "KeyL", RETROK_l },
   { "KeyM", RETROK_m },
   { "KeyN", RETROK_n },
   { "KeyO", RETROK_o },
   { "KeyP", RETROK_p },
   { "KeyQ", RETROK_q },
   { "KeyR", RETROK_r },
   { "KeyS", RETROK_s },
   { "KeyT", RETROK_t },
   { "KeyU", RETROK_u },
   { "KeyV", RETROK_v },
   { "KeyW", RETROK_w },
   { "KeyX", RETROK_x },
   { "KeyY", RETROK_y },
   { "KeyZ", RETROK_z },
   { "ArrowLeft", RETROK_LEFT },
   { "ArrowRight", RETROK_RIGHT },
   { "ArrowUp", RETROK_UP },
   { "ArrowDown", RETROK_DOWN },
   { "Enter", RETROK_RETURN },
   { "NumpadEnter", RETROK_KP_ENTER },
   { "Tab", RETROK_TAB },
   { "Insert", RETROK_INSERT },
   { "Delete", RETROK_DELETE },
   { "End", RETROK_END },
   { "Home", RETROK_HOME },
   { "ShiftRight", RETROK_RSHIFT },
   { "ShiftLeft", RETROK_LSHIFT },
   { "ControlLeft", RETROK_LCTRL },
   { "AltLeft", RETROK_LALT },
   { "Space", RETROK_SPACE },
   { "Escape", RETROK_ESCAPE },
   { "NumpadAdd", RETROK_KP_PLUS },
   { "NumpadSubtract", RETROK_KP_MINUS },
   { "F1", RETROK_F1 },
   { "F2", RETROK_F2 },
   { "F3", RETROK_F3 },
   { "F4", RETROK_F4 },
   { "F5", RETROK_F5 },
   { "F6", RETROK_F6 },
   { "F7", RETROK_F7 },
   { "F8", RETROK_F8 },
   { "F9", RETROK_F9 },
   { "F10", RETROK_F10 },
   { "F11", RETROK_F11 },
   { "F12", RETROK_F12 },
   { "Digit0", RETROK_0 },
   { "Digit1", RETROK_1 },
   { "Digit2", RETROK_2 },
   { "Digit3", RETROK_3 },
   { "Digit4", RETROK_4 },
   { "Digit5", RETROK_5 },
   { "Digit6", RETROK_6 },
   { "Digit7", RETROK_7 },
   { "Digit8", RETROK_8 },
   { "Digit9", RETROK_9 },
   { "PageUp", RETROK_PAGEUP },
   { "PageDown", RETROK_PAGEDOWN },
   { "Numpad0", RETROK_KP0 },
   { "Numpad1", RETROK_KP1 },
   { "Numpad2", RETROK_KP2 },
   { "Numpad3", RETROK_KP3 },
   { "Numpad4", RETROK_KP4 },
   { "Numpad5", RETROK_KP5 },
   { "Numpad6", RETROK_KP6 },
   { "Numpad7", RETROK_KP7 },
   { "Numpad8", RETROK_KP8 },
   { "Numpad9", RETROK_KP9 },
   { "Period", RETROK_PERIOD },
   { "CapsLock", RETROK_CAPSLOCK },
   { "NumLock", RETROK_NUMLOCK },
   { "Backspace", RETROK_BACKSPACE },
   { "NumpadMultiply", RETROK_KP_MULTIPLY },
   { "NumpadDivide", RETROK_KP_DIVIDE },
   { "PrintScreen", RETROK_PRINT },
   { "ScrollLock", RETROK_SCROLLOCK },
   { "Backquote", RETROK_BACKQUOTE },
   { "Pause", RETROK_PAUSE },
   { "Quote", RETROK_QUOTE },
   { "Comma", RETROK_COMMA },
   { "Minus", RETROK_MINUS },
   { "Slash", RETROK_SLASH },
   { "Semicolon", RETROK_SEMICOLON },
   { "Equal", RETROK_EQUALS },
   { "BracketLeft", RETROK_LEFTBRACKET },
   { "Backslash", RETROK_BACKSLASH },
   { "BracketRight", RETROK_RIGHTBRACKET },
   { "NumpadDecimal", RETROK_KP_PERIOD },
   { "NumpadEqual", RETROK_KP_EQUALS },
   { "ControlRight", RETROK_RCTRL },
   { "AltRight", RETROK_RALT },
   { "F13", RETROK_F13 },
   { "F14", RETROK_F14 },
   { "F15", RETROK_F15 },
   { "MetaRight", RETROK_RMETA },
   { "MetaLeft", RETROK_LMETA },
   { "Help", RETROK_HELP },
   { "ContextMenu", RETROK_MENU },
   { "Power", RETROK_POWER },
};

/* to make the string labels for codes from JavaScript work, we convert them
 * to CRC32 hashes for the LUT */
static void rwebinput_generate_lut(void)
{
   int i;
   struct rarch_key_map *key_map;

   for (i = 0; i < ARRAY_SIZE(rwebinput_key_to_code_map); i++)
   {
      uint32_t crc;
      const rwebinput_key_to_code_map_entry_t *key_to_code =
         &rwebinput_key_to_code_map[i];
      key_map = &rarch_key_map_rwebinput[i];
      crc = encoding_crc32(0, (const uint8_t *)key_to_code->key,
         strlen(key_to_code->key));

      key_map->rk  = key_to_code->rk;
      key_map->sym = crc;
   }

   /* set terminating entry */
   key_map      = &rarch_key_map_rwebinput[
      ARRAY_SIZE(rarch_key_map_rwebinput) - 1];
   key_map->rk  = RETROK_UNKNOWN;
   key_map->sym = 0;
}

static EM_BOOL rwebinput_keyboard_cb(int event_type,
   const EmscriptenKeyboardEvent *key_event, void *user_data)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)user_data;

   if (event_type == EMSCRIPTEN_EVENT_KEYPRESS)
      return EM_TRUE;

   if (rwebinput->keyboard.count >= rwebinput->keyboard.max_size)
   {
      size_t new_max = MAX(1, rwebinput->keyboard.max_size << 1);
      rwebinput->keyboard.events = realloc(rwebinput->keyboard.events,
         new_max * sizeof(rwebinput->keyboard.events[0]));
      rwebinput->keyboard.max_size = new_max;
   }

   rwebinput->keyboard.events[rwebinput->keyboard.count].type = event_type;
   memcpy(&rwebinput->keyboard.events[rwebinput->keyboard.count].event,
      key_event, sizeof(*key_event));
   rwebinput->keyboard.count++;

   return EM_TRUE;
}

static EM_BOOL rwebinput_mouse_cb(int event_type,
   const EmscriptenMouseEvent *mouse_event, void *user_data)
{
   rwebinput_input_t *rwebinput      = (rwebinput_input_t*)user_data;

   uint8_t mask                      = 1 << mouse_event->button;

   // note: movementX/movementY are pre-scaled in chromium (but not firefox)
   // see https://github.com/w3c/pointerlock/issues/42

   rwebinput->mouse.pending_delta_x += mouse_event->movementX;
   rwebinput->mouse.pending_delta_y += mouse_event->movementY;

   if (rwebinput->pointerlock_active)
   {
      unsigned video_width, video_height;
      video_driver_get_size(&video_width, &video_height);

      rwebinput->mouse.x += mouse_event->movementX;
      rwebinput->mouse.y += mouse_event->movementY;

      /* Clamp X */
      if (rwebinput->mouse.x < 0)
         rwebinput->mouse.x = 0;
      if (rwebinput->mouse.x >= video_width)
         rwebinput->mouse.x = (int)(video_width - 1);

      /* Clamp Y */
      if (rwebinput->mouse.y < 0)
         rwebinput->mouse.y = 0;
      if (rwebinput->mouse.y >= video_height)
         rwebinput->mouse.y = (int)(video_height - 1);
   }
   else
   {
      double dpr = platform_emscripten_get_dpr();
      rwebinput->mouse.x = (int)(mouse_event->targetX * dpr);
      rwebinput->mouse.y = (int)(mouse_event->targetY * dpr);
   }

   if (event_type ==  EMSCRIPTEN_EVENT_MOUSEDOWN)
      rwebinput->mouse.buttons |= mask;
   else if (event_type == EMSCRIPTEN_EVENT_MOUSEUP)
      rwebinput->mouse.buttons &= ~mask;

   return EM_TRUE;
}

static EM_BOOL rwebinput_wheel_cb(int event_type,
   const EmscriptenWheelEvent *wheel_event, void *user_data)
{
   rwebinput_input_t       *rwebinput = (rwebinput_input_t*)user_data;

   double dpr = platform_emscripten_get_dpr();
   rwebinput->mouse.pending_scroll_x += wheel_event->deltaX * dpr;
   rwebinput->mouse.pending_scroll_y += wheel_event->deltaY * dpr;

   return EM_TRUE;
}

static EM_BOOL rwebinput_touch_cb(int event_type,
   const EmscriptenTouchEvent *touch_event, void *user_data)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)user_data;

   unsigned touches_max      = MIN(touch_event->numTouches, MAX_TOUCH);
   unsigned touches_released = 0;

   switch (event_type)
   {
      case EMSCRIPTEN_EVENT_TOUCHSTART:
      case EMSCRIPTEN_EVENT_TOUCHMOVE:
         for (unsigned touch = 0; touch < touches_max; touch++)
         {
            if (!(touch_event->touches[touch].isChanged) && rwebinput->pointer[touch].id == touch_event->touches[touch].identifier)
               continue;

            double dpr = platform_emscripten_get_dpr();
            rwebinput->pointer[touch].x  = (int)(touch_event->touches[touch].targetX * dpr);
            rwebinput->pointer[touch].y  = (int)(touch_event->touches[touch].targetY * dpr);
            rwebinput->pointer[touch].id = touch_event->touches[touch].identifier;
         }
         break;
      case EMSCRIPTEN_EVENT_TOUCHEND:
      case EMSCRIPTEN_EVENT_TOUCHCANCEL:
         // note: touches_max/numTouches is out of date here - it uses the old value from before the release
         // note 2: I'm unsure if multiple touches can trigger the same touchend anyway...
         if (touches_max > 1)
         {
            for (unsigned touch_up = 0; touch_up < touches_max; touch_up++)
            {
               if (touch_event->touches[touch_up].isChanged)
               {
                  memmove(rwebinput->pointer + touch_up - touches_released,
                          rwebinput->pointer + touch_up - touches_released + 1,
                          (touches_max - touch_up - 1) * sizeof(rwebinput_pointer_state_t));
                  touches_released++;
               }
            }
         }
         else
            touches_released = 1;

         if (touches_max > touches_released)
            touches_max -= touches_released;
         else
            touches_max = 0;
         break;
   }

   rwebinput->pointer_count = touches_max;

   return EM_TRUE;
}

static EM_BOOL rwebinput_pointerlockchange_cb(int event_type,
   const EmscriptenPointerlockChangeEvent *pointerlock_change_event, void *user_data)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)user_data;

   rwebinput->pointerlock_active = pointerlock_change_event->isActive;

   if (!pointerlock_change_event->isActive)
   {
      input_driver_state_t *input_st = input_state_get_ptr();

      if (input_st->game_focus_state.enabled)
      {
         enum input_game_focus_cmd_type game_focus_cmd = GAME_FOCUS_CMD_OFF;
         command_event(CMD_EVENT_GAME_FOCUS_TOGGLE, &game_focus_cmd);
      }

      if (input_st->flags & INP_FLAG_GRAB_MOUSE_STATE)
      {
         command_event(CMD_EVENT_GRAB_MOUSE_TOGGLE, NULL);
      }
   }

   return EM_TRUE;
}

static EM_BOOL rwebinput_devicemotion_cb(int event_type,
   const EmscriptenDeviceMotionEvent *device_motion_event, void *user_data)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)user_data;

   /* TODO: what units does mGBA want? does something need to be changed on the core side? */
   /* given in m/s^2 (inverted) */
   rwebinput->accelerometer.x = device_motion_event->accelerationIncludingGravityX / -9.8;
   rwebinput->accelerometer.y = device_motion_event->accelerationIncludingGravityY / -9.8;
   rwebinput->accelerometer.z = device_motion_event->accelerationIncludingGravityZ / -9.8;
   /* XYZ == BetaGammaAlpha according to W3C? in my testing it is AlphaBetaGamma... */
   /* libretro wants radians/s but it is too fast in mGBA, see above comment */
   /* given in degrees/s */
   rwebinput->gyroscope.x = device_motion_event->rotationRateAlpha / 180;
   rwebinput->gyroscope.y = device_motion_event->rotationRateBeta  / 180;
   rwebinput->gyroscope.z = device_motion_event->rotationRateGamma / 180;

   return EM_TRUE;
}

static void *rwebinput_input_init(const char *joypad_driver)
{
   EMSCRIPTEN_RESULT r;
   rwebinput_input_t *rwebinput =
      (rwebinput_input_t*)calloc(1, sizeof(*rwebinput));

   if (!rwebinput)
      return NULL;

   rwebinput_generate_lut();

   input_keymaps_init_keyboard_lut(rarch_key_map_rwebinput);

   r = emscripten_set_keydown_callback("#canvas", rwebinput, false,
         rwebinput_keyboard_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create keydown callback: %d.\n", r);
   }

   r = emscripten_set_keyup_callback("#canvas", rwebinput, false,
         rwebinput_keyboard_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create keyup callback: %d.\n", r);
   }

   r = emscripten_set_keypress_callback("#canvas", rwebinput, false,
         rwebinput_keyboard_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create keypress callback: %d.\n", r);
   }

   r = emscripten_set_mousedown_callback("#canvas", rwebinput, false,
         rwebinput_mouse_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create mousedown callback: %d.\n", r);
   }

   r = emscripten_set_mouseup_callback("#canvas", rwebinput, false,
         rwebinput_mouse_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create mouseup callback: %d.\n", r);
   }

   r = emscripten_set_mousemove_callback("#canvas", rwebinput, false,
         rwebinput_mouse_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create mousemove callback: %d.\n", r);
   }

   r = emscripten_set_wheel_callback("#canvas", rwebinput, false,
         rwebinput_wheel_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create wheel callback: %d.\n", r);
   }

   r = emscripten_set_touchstart_callback("#canvas", rwebinput, false,
         rwebinput_touch_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create touchstart callback: %d.\n", r);
   }

   r = emscripten_set_touchend_callback("#canvas", rwebinput, false,
         rwebinput_touch_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create touchend callback: %d.\n", r);
   }

   r = emscripten_set_touchmove_callback("#canvas", rwebinput, false,
         rwebinput_touch_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create touchmove callback: %d.\n", r);
   }

   r = emscripten_set_touchcancel_callback("#canvas", rwebinput, false,
         rwebinput_touch_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create touchcancel callback: %d.\n", r);
   }

   r = emscripten_set_pointerlockchange_callback(
         EMSCRIPTEN_EVENT_TARGET_DOCUMENT, rwebinput, false,
         rwebinput_pointerlockchange_cb);
   if (r != EMSCRIPTEN_RESULT_SUCCESS)
   {
      RARCH_ERR(
         "[EMSCRIPTEN/INPUT] Failed to create pointerlockchange callback: %d.\n", r);
   }

   return rwebinput;
}

static bool rwebinput_key_pressed(rwebinput_input_t *rwebinput, int key)
{
   if (key >= RETROK_LAST)
      return false;

   return rwebinput->keys[key];
}

static int16_t rwebinput_mouse_state(
      rwebinput_mouse_state_t *mouse,
      unsigned id, bool screen)
{
   switch (id)
   {
      case RETRO_DEVICE_ID_MOUSE_X:
         return (int16_t)(screen ? mouse->x : mouse->delta_x);
      case RETRO_DEVICE_ID_MOUSE_Y:
         return (int16_t)(screen ? mouse->y : mouse->delta_y);
      case RETRO_DEVICE_ID_MOUSE_LEFT:
         return !!(mouse->buttons & (1 << RWEBINPUT_MOUSE_BTNL));
      case RETRO_DEVICE_ID_MOUSE_RIGHT:
         return !!(mouse->buttons & (1 << RWEBINPUT_MOUSE_BTNR));
      case RETRO_DEVICE_ID_MOUSE_MIDDLE:
         return !!(mouse->buttons & (1 << RWEBINPUT_MOUSE_BTNM));
      case RETRO_DEVICE_ID_MOUSE_BUTTON_4:
         return !!(mouse->buttons & (1 << RWEBINPUT_MOUSE_BTN4));
      case RETRO_DEVICE_ID_MOUSE_BUTTON_5:
         return !!(mouse->buttons & (1 << RWEBINPUT_MOUSE_BTN5));
      case RETRO_DEVICE_ID_MOUSE_WHEELUP:
         return mouse->scroll_y < 0.0f;
      case RETRO_DEVICE_ID_MOUSE_WHEELDOWN:
         return mouse->scroll_y > 0.0f;
      case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELUP:
         return mouse->scroll_x < 0.0f;
      case RETRO_DEVICE_ID_MOUSE_HORIZ_WHEELDOWN:
         return mouse->scroll_x > 0.0f;
   }

   return 0;
}

static int16_t rwebinput_is_pressed(
      rwebinput_input_t *rwebinput,
      const struct retro_keybind *binds,
      unsigned port, unsigned id,
      bool keyboard_mapping_blocked)
{
   const struct retro_keybind *bind = &binds[id];
   int key                          = bind->key;

   if (     (key && key < RETROK_LAST)
         && rwebinput_key_pressed(rwebinput, key)
         && (id == RARCH_GAME_FOCUS_TOGGLE || !keyboard_mapping_blocked)
      )
      return 1;
   if (port == 0 && !!rwebinput_mouse_state(&rwebinput->mouse, bind->mbutton, false))
      return 1;
   return 0;
}

static int16_t rwebinput_input_state(
      void *data,
      const input_device_driver_t *joypad,
      const input_device_driver_t *sec_joypad,
      rarch_joypad_info_t *joypad_info,
      const retro_keybind_set *binds,
      bool keyboard_mapping_blocked,
      unsigned port,
      unsigned device,
      unsigned idx,
      unsigned id)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)data;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (id == RETRO_DEVICE_ID_JOYPAD_MASK)
         {
            unsigned i;
            int16_t ret = 0;
            for (i = 0; i < RARCH_FIRST_CUSTOM_BIND; i++)
            {
               if (binds[port][i].valid)
               {
                  if (rwebinput_is_pressed(
                           rwebinput, binds[port], port, i,
                           keyboard_mapping_blocked))
                     ret |= (1 << i);
               }
            }

            return ret;
         }

         if (id < RARCH_BIND_LIST_END)
         {
            if (binds[port][id].valid)
            {
               if (rwebinput_is_pressed(rwebinput,
                        binds[port],
                        port, id,
                        keyboard_mapping_blocked))
                  return 1;
            }
         }
         break;
      case RETRO_DEVICE_ANALOG:
         if (binds[port])
         {
            int id_minus_key      = 0;
            int id_plus_key       = 0;
            unsigned id_minus     = 0;
            unsigned id_plus      = 0;
            int16_t ret           = 0;
            bool id_plus_valid    = false;
            bool id_minus_valid   = false;

            input_conv_analog_id_to_bind_id(idx, id, id_minus, id_plus);

            id_minus_valid        = binds[port][id_minus].valid;
            id_plus_valid         = binds[port][id_plus].valid;
            id_minus_key          = binds[port][id_minus].key;
            id_plus_key           = binds[port][id_plus].key;

            if (id_plus_valid && id_plus_key && id_plus_key < RETROK_LAST)
            {
               if (rwebinput_is_pressed(rwebinput,
                        binds[port], idx, id_plus,
                        keyboard_mapping_blocked))
                  ret = 0x7fff;
            }
            if (id_minus_valid && id_minus_key && id_minus_key < RETROK_LAST)
            {
               if (rwebinput_is_pressed(rwebinput,
                        binds[port], idx, id_minus,
                        keyboard_mapping_blocked))
                  ret += -0x7fff;
            }

            return ret;
         }
         break;
      case RETRO_DEVICE_KEYBOARD:
         return (id && id < RETROK_LAST) && rwebinput->keys[id];
      case RETRO_DEVICE_MOUSE:
      case RARCH_DEVICE_MOUSE_SCREEN:
         return rwebinput_mouse_state(&rwebinput->mouse, id, device == RARCH_DEVICE_MOUSE_SCREEN);
      case RETRO_DEVICE_POINTER:
      case RARCH_DEVICE_POINTER_SCREEN:
         {
            struct video_viewport vp    = {0};
            rwebinput_mouse_state_t
               *mouse                   = &rwebinput->mouse;
            bool pointer_down           = false;
            unsigned pointer_count      = rwebinput->pointer_count;
            int x                       = 0;
            int y                       = 0;
            int16_t res_x               = 0;
            int16_t res_y               = 0;
            int16_t res_screen_x        = 0;
            int16_t res_screen_y        = 0;

            if (pointer_count && idx < pointer_count)
            {
               x = rwebinput->pointer[idx].x;
               y = rwebinput->pointer[idx].y;
               pointer_down = true;
            }
            else if (idx == 0)
            {
               x = mouse->x;
               y = mouse->y;
               pointer_down = !!(mouse->buttons & (1 << RWEBINPUT_MOUSE_BTNL));
               pointer_count = 1;
            }
            else
               return 0;

            if (!(video_driver_translate_coord_viewport_confined_wrap(
                        &vp, x, y,
                        &res_x, &res_y, &res_screen_x, &res_screen_y)))
               return 0;

            if (device == RARCH_DEVICE_POINTER_SCREEN)
            {
               res_x = res_screen_x;
               res_y = res_screen_y;
            }

            switch (id)
            {
               case RETRO_DEVICE_ID_POINTER_X:
                  return res_x;
               case RETRO_DEVICE_ID_POINTER_Y:
                  return res_y;
               case RETRO_DEVICE_ID_POINTER_PRESSED:
                  return (pointer_down && !input_driver_pointer_is_offscreen(res_x, res_y));
               case RETRO_DEVICE_ID_POINTER_COUNT:
                  return pointer_count;
               case RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN:
                  return input_driver_pointer_is_offscreen(res_x, res_y);
               default:
                  break;
            }
         }
         break;
   }

   return 0;
}

static void rwebinput_remove_event_listeners(void *data)
{
   /* *currently* not automatically proxied in the case of PROXY_TO_PTHREAD */
   emscripten_html5_remove_all_event_listeners();
}

static void rwebinput_input_free(void *data)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)data;

   platform_emscripten_run_on_browser_thread_sync(rwebinput_remove_event_listeners, NULL);
   free(rwebinput->keyboard.events);
   free(data);
}

static bool rwebinput_set_sensor_state(void *data, unsigned port,
      enum retro_sensor_action action, unsigned rate)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)data;
   EMSCRIPTEN_RESULT r;
   bool old_state = rwebinput->accelerometer.enabled || rwebinput->gyroscope.enabled;
   bool new_state;

   switch (action)
   {
      case RETRO_SENSOR_ACCELEROMETER_ENABLE:
         rwebinput->accelerometer.enabled = true;
         break;
      case RETRO_SENSOR_ACCELEROMETER_DISABLE:
         rwebinput->accelerometer.enabled = false;
         break;
      case RETRO_SENSOR_GYROSCOPE_ENABLE:
         rwebinput->gyroscope.enabled = true;
         break;
      case RETRO_SENSOR_GYROSCOPE_DISABLE:
         rwebinput->gyroscope.enabled = false;
         break;
      case RETRO_SENSOR_ILLUMINANCE_ENABLE:
      case RETRO_SENSOR_ILLUMINANCE_DISABLE:
         return false; /* not supported (browsers removed support for now) */
      default:
         return false;
   }

   new_state = rwebinput->accelerometer.enabled || rwebinput->gyroscope.enabled;

   if (!old_state && new_state)
   {
      r = emscripten_set_devicemotion_callback(rwebinput, false, rwebinput_devicemotion_cb);
      if (r != EMSCRIPTEN_RESULT_SUCCESS)
      {
         RARCH_ERR(
            "[EMSCRIPTEN/INPUT] Failed to create devicemotion callback: %d.\n", r);
         return false;
      }
   }
   else if (old_state && !new_state)
   {
      r = emscripten_set_devicemotion_callback(rwebinput, false, NULL);
      if (r != EMSCRIPTEN_RESULT_SUCCESS)
      {
         RARCH_ERR(
            "[EMSCRIPTEN/INPUT] Failed to remove devicemotion callback: %d.\n", r);
         return false;
      }
   }

   return true;
}

static float rwebinput_get_sensor_input(void *data, unsigned port, unsigned id)
{
   rwebinput_input_t *rwebinput = (rwebinput_input_t*)data;

   switch (id)
   {
      case RETRO_SENSOR_ACCELEROMETER_X:
         return rwebinput->accelerometer.x;
      case RETRO_SENSOR_ACCELEROMETER_Y:
         return rwebinput->accelerometer.y;
      case RETRO_SENSOR_ACCELEROMETER_Z:
         return rwebinput->accelerometer.z;
      case RETRO_SENSOR_GYROSCOPE_X:
         return rwebinput->gyroscope.x;
      case RETRO_SENSOR_GYROSCOPE_Y:
         return rwebinput->gyroscope.y;
      case RETRO_SENSOR_GYROSCOPE_Z:
         return rwebinput->gyroscope.z;
   }

   return 0.0f;
}

static void rwebinput_process_keyboard_events(
      rwebinput_input_t *rwebinput,
      rwebinput_keyboard_event_t *event)
{
   uint32_t keycode;
   unsigned translated_keycode;
   uint32_t character                       = 0;
   uint16_t mod                             = 0;
   const EmscriptenKeyboardEvent *key_event = &event->event;
   bool keydown                             =
      event->type == EMSCRIPTEN_EVENT_KEYDOWN;

   /* a printable key: populate character field */
   if (utf8len(key_event->key) == 1)
   {
      const char *key_ptr = &key_event->key[0];
      character           = utf8_walk(&key_ptr);
   }

   if (key_event->ctrlKey)
      mod |= RETROKMOD_CTRL;
   if (key_event->altKey)
      mod |= RETROKMOD_ALT;
   if (key_event->shiftKey)
      mod |= RETROKMOD_SHIFT;
   if (key_event->metaKey)
      mod |= RETROKMOD_META;

   keycode = encoding_crc32(0, (const uint8_t *)key_event->code,
      strnlen(key_event->code, sizeof(key_event->code)));
   translated_keycode = input_keymaps_translate_keysym_to_rk(keycode);

   if (     translated_keycode == RETROK_BACKSPACE)
      character = '\b';
   else if (translated_keycode == RETROK_RETURN ||
            translated_keycode == RETROK_KP_ENTER)
      character = '\n';
   else if (translated_keycode == RETROK_TAB)
      character = '\t';

   if (translated_keycode != RETROK_UNKNOWN)
      input_keyboard_event(keydown, translated_keycode, character, mod,
         RETRO_DEVICE_KEYBOARD);

   if (     translated_keycode  < RETROK_LAST
         && translated_keycode != RETROK_UNKNOWN)
      rwebinput->keys[translated_keycode] = keydown;
}

static void rwebinput_input_poll(void *data)
{
   size_t i;
   rwebinput_input_t *rwebinput      = (rwebinput_input_t*)data;

   for (i = 0; i < rwebinput->keyboard.count; i++)
      rwebinput_process_keyboard_events(rwebinput,
         &rwebinput->keyboard.events[i]);

   rwebinput->keyboard.count         = 0;

   rwebinput->mouse.delta_x          = rwebinput->mouse.pending_delta_x;
   rwebinput->mouse.delta_y          = rwebinput->mouse.pending_delta_y;
   rwebinput->mouse.pending_delta_x  = 0;
   rwebinput->mouse.pending_delta_y  = 0;

   rwebinput->mouse.scroll_x         = rwebinput->mouse.pending_scroll_x;
   rwebinput->mouse.scroll_y         = rwebinput->mouse.pending_scroll_y;
   rwebinput->mouse.pending_scroll_x = 0;
   rwebinput->mouse.pending_scroll_y = 0;
}

static void rwebinput_grab_mouse(void *data, bool state)
{
   if (state)
      emscripten_request_pointerlock("#canvas", EM_TRUE);
   else
      emscripten_exit_pointerlock();
}

static uint64_t rwebinput_get_capabilities(void *data)
{
   return
           (1 << RETRO_DEVICE_JOYPAD)
         | (1 << RETRO_DEVICE_ANALOG)
         | (1 << RETRO_DEVICE_KEYBOARD)
         | (1 << RETRO_DEVICE_MOUSE)
         | (1 << RETRO_DEVICE_POINTER);
}

input_driver_t input_rwebinput = {
   rwebinput_input_init,
   rwebinput_input_poll,
   rwebinput_input_state,
   rwebinput_input_free,
   rwebinput_set_sensor_state,
   rwebinput_get_sensor_input,
   rwebinput_get_capabilities,
   "rwebinput",
   rwebinput_grab_mouse,
   NULL,
   NULL
};
