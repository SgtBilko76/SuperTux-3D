//  SuperTux
//  Copyright (C) 2026 SuperTux Development Team
//
//  This program is free software: you can redistribute it and/or modify
//  it under the terms of the GNU General Public License as published by
//  the Free Software Foundation, either version 3 of the License, or
//  (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program.  If not, see <http://www.gnu.org/licenses/>.

#pragma once

#include <config.h>

#ifdef ENABLE_OPENXR

#include <initializer_list>
#include <utility>
#include <vector>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>

#include "control/controller.hpp"

/** Maps OpenXR controller actions (Meta Quest Touch controllers and
    the generic Khronos simple controller) onto SuperTux's Control
    values.

    Mapping (Touch controllers):

      Left thumbstick        move (LEFT/RIGHT/UP/DOWN)
      Right thumbstick       peek (PEEK_*)
      A / left trigger       JUMP (also MENU_SELECT in menus)
      B                      MENU_BACK
      X / right trigger      ACTION (run, grab, shoot)
      Y                      ITEM
      Left grip / right grip PEEK_LEFT / PEEK_RIGHT
      Menu button (left)     START (pause / open menu)
      Left thumbstick click  recenter the virtual screen
      Right thumbstick click CHEAT_MENU (only useful in debug builds)

    Controls are only updated on state changes so that a paired
    bluetooth gamepad or keyboard can still be used at the same
    time. */
class VRInput final
{
public:
  VRInput(XrInstance instance, XrSession session);
  ~VRInput();

  /** Attach the action set to the session, must be called once
      before the first sync. */
  void attach();

  /** Sync the actions and forward state changes to the controller.
      Returns true if a recenter was requested this frame. */
  bool poll(XrTime time, Controller& controller);

private:
  struct BoolAction
  {
    XrAction action = XR_NULL_HANDLE;
    bool state = false;
  };

  struct Vec2Action
  {
    XrAction action = XR_NULL_HANDLE;
    XrVector2f state = {0.0f, 0.0f};
  };

  struct AxisButtons
  {
    bool left = false;
    bool right = false;
    bool up = false;
    bool down = false;
  };

  XrAction create_action(const char* name, const char* localized, XrActionType type);
  bool read_bool(XrAction action) const;
  XrVector2f read_vec2(XrAction action) const;
  void suggest_bindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings);

  /** Push a state change of a button to the given controls. */
  void update_button(BoolAction& action, Controller& controller,
                     std::initializer_list<Control> controls);
  void update_axis(Vec2Action& action, AxisButtons& buttons, Controller& controller,
                   Control left, Control right, Control up, Control down);

private:
  XrInstance m_instance;
  XrSession m_session;
  XrActionSet m_action_set;

  Vec2Action m_move;
  Vec2Action m_peek;
  BoolAction m_jump;
  BoolAction m_action;
  BoolAction m_item;
  BoolAction m_back;
  BoolAction m_start;
  BoolAction m_peek_left;
  BoolAction m_peek_right;
  BoolAction m_recenter;
  BoolAction m_cheat_menu;

  AxisButtons m_move_buttons;
  AxisButtons m_peek_buttons;

private:
  VRInput(const VRInput&) = delete;
  VRInput& operator=(const VRInput&) = delete;
};

#endif // ENABLE_OPENXR
