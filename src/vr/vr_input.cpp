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

#include "vr/vr_input.hpp"

#ifdef ENABLE_OPENXR

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "util/log.hpp"

namespace {

/** Thumbstick deflection at which a direction is considered pressed
    and released (with hysteresis so the direction doesn't flicker). */
constexpr float AXIS_PRESS = 0.5f;
constexpr float AXIS_RELEASE = 0.3f;

void check(XrResult result, const char* what)
{
  if (XR_FAILED(result))
  {
    throw std::runtime_error(std::string("OpenXR: ") + what + " failed with result " + std::to_string(result));
  }
}

} // namespace

VRInput::VRInput(XrInstance instance, XrSession session, bool bd_controller) :
  m_instance(instance),
  m_session(session),
  m_action_set(XR_NULL_HANDLE),
  m_move(),
  m_peek(),
  m_jump(),
  m_action(),
  m_item(),
  m_back(),
  m_start(),
  m_peek_left(),
  m_peek_right(),
  m_recenter(),
  m_cheat_menu(),
  m_move_buttons(),
  m_peek_buttons()
{
  XrActionSetCreateInfo set_info{XR_TYPE_ACTION_SET_CREATE_INFO};
  std::strncpy(set_info.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
  std::strncpy(set_info.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
  set_info.priority = 0;
  check(xrCreateActionSet(m_instance, &set_info, &m_action_set), "xrCreateActionSet");

  m_move.action = create_action("move", "Move", XR_ACTION_TYPE_VECTOR2F_INPUT);
  m_peek.action = create_action("peek", "Peek", XR_ACTION_TYPE_VECTOR2F_INPUT);
  m_jump.action = create_action("jump", "Jump / Select", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_action.action = create_action("action", "Action", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_item.action = create_action("item", "Item", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_back.action = create_action("back", "Back", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_start.action = create_action("start", "Pause / Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_peek_left.action = create_action("peek_left", "Peek Left", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_peek_right.action = create_action("peek_right", "Peek Right", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_recenter.action = create_action("recenter", "Recenter Screen", XR_ACTION_TYPE_BOOLEAN_INPUT);
  m_cheat_menu.action = create_action("cheat_menu", "Cheat Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);

  // Meta Quest Touch controllers.
  suggest_bindings("/interaction_profiles/oculus/touch_controller", {
    { m_move.action,       "/user/hand/left/input/thumbstick" },
    { m_peek.action,       "/user/hand/right/input/thumbstick" },
    { m_jump.action,       "/user/hand/right/input/a/click" },
    { m_jump.action,       "/user/hand/left/input/trigger/value" },
    { m_action.action,     "/user/hand/left/input/x/click" },
    { m_action.action,     "/user/hand/right/input/trigger/value" },
    { m_item.action,       "/user/hand/left/input/y/click" },
    { m_back.action,       "/user/hand/right/input/b/click" },
    { m_start.action,      "/user/hand/left/input/menu/click" },
    { m_peek_left.action,  "/user/hand/left/input/squeeze/value" },
    { m_peek_right.action, "/user/hand/right/input/squeeze/value" },
    { m_recenter.action,   "/user/hand/left/input/thumbstick/click" },
    { m_cheat_menu.action, "/user/hand/right/input/thumbstick/click" },
  });

  // PICO Touch-style controllers (PICO 4 / 4 Ultra and Neo 3). Only
  // available when the runtime enabled XR_BD_controller_interaction; the
  // button layout matches the Quest controllers.
  if (bd_controller)
  {
    const std::vector<std::pair<XrAction, const char*>> pico_bindings = {
      { m_move.action,       "/user/hand/left/input/thumbstick" },
      { m_peek.action,       "/user/hand/right/input/thumbstick" },
      { m_jump.action,       "/user/hand/right/input/a/click" },
      { m_jump.action,       "/user/hand/left/input/trigger/value" },
      { m_action.action,     "/user/hand/left/input/x/click" },
      { m_action.action,     "/user/hand/right/input/trigger/value" },
      { m_item.action,       "/user/hand/left/input/y/click" },
      { m_back.action,       "/user/hand/right/input/b/click" },
      { m_start.action,      "/user/hand/left/input/menu/click" },
      { m_peek_left.action,  "/user/hand/left/input/squeeze/value" },
      { m_peek_right.action, "/user/hand/right/input/squeeze/value" },
      { m_recenter.action,   "/user/hand/left/input/thumbstick/click" },
      { m_cheat_menu.action, "/user/hand/right/input/thumbstick/click" },
    };
    suggest_bindings("/interaction_profiles/bytedance/pico4_controller", pico_bindings);
    suggest_bindings("/interaction_profiles/bytedance/pico_neo3_controller", pico_bindings);
  }

  // Minimal fallback profile every runtime supports.
  suggest_bindings("/interaction_profiles/khr/simple_controller", {
    { m_jump.action,   "/user/hand/right/input/select/click" },
    { m_action.action, "/user/hand/left/input/select/click" },
    { m_start.action,  "/user/hand/left/input/menu/click" },
    { m_back.action,   "/user/hand/right/input/menu/click" },
  });
}

VRInput::~VRInput()
{
  for (XrAction action : { m_move.action, m_peek.action, m_jump.action, m_action.action,
                           m_item.action, m_back.action, m_start.action, m_peek_left.action,
                           m_peek_right.action, m_recenter.action, m_cheat_menu.action })
  {
    if (action != XR_NULL_HANDLE)
      xrDestroyAction(action);
  }

  if (m_action_set != XR_NULL_HANDLE)
    xrDestroyActionSet(m_action_set);
}

XrAction
VRInput::create_action(const char* name, const char* localized, XrActionType type)
{
  XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
  info.actionType = type;
  std::strncpy(info.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
  std::strncpy(info.localizedActionName, localized, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
  info.countSubactionPaths = 0;
  info.subactionPaths = nullptr;

  XrAction action = XR_NULL_HANDLE;
  check(xrCreateAction(m_action_set, &info, &action), "xrCreateAction");
  return action;
}

void
VRInput::suggest_bindings(const char* profile, const std::vector<std::pair<XrAction, const char*>>& bindings)
{
  XrPath profile_path = XR_NULL_PATH;
  if (XR_FAILED(xrStringToPath(m_instance, profile, &profile_path)))
  {
    log_warning << "OpenXR: unknown interaction profile " << profile << std::endl;
    return;
  }

  std::vector<XrActionSuggestedBinding> suggested;
  suggested.reserve(bindings.size());
  for (const auto& binding : bindings)
  {
    XrPath path = XR_NULL_PATH;
    if (XR_FAILED(xrStringToPath(m_instance, binding.second, &path)))
    {
      log_warning << "OpenXR: invalid binding path " << binding.second << std::endl;
      continue;
    }
    suggested.push_back({ binding.first, path });
  }

  XrInteractionProfileSuggestedBinding info{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
  info.interactionProfile = profile_path;
  info.countSuggestedBindings = static_cast<uint32_t>(suggested.size());
  info.suggestedBindings = suggested.data();

  const XrResult result = xrSuggestInteractionProfileBindings(m_instance, &info);
  if (XR_FAILED(result))
  {
    log_warning << "OpenXR: suggesting bindings for " << profile << " failed with result " << result << std::endl;
  }
}

void
VRInput::attach()
{
  XrSessionActionSetsAttachInfo info{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
  info.countActionSets = 1;
  info.actionSets = &m_action_set;
  check(xrAttachSessionActionSets(m_session, &info), "xrAttachSessionActionSets");
}

bool
VRInput::read_bool(XrAction action) const
{
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  info.subactionPath = XR_NULL_PATH;

  XrActionStateBoolean state{XR_TYPE_ACTION_STATE_BOOLEAN};
  if (XR_FAILED(xrGetActionStateBoolean(m_session, &info, &state)))
    return false;

  return state.isActive && state.currentState;
}

XrVector2f
VRInput::read_vec2(XrAction action) const
{
  XrActionStateGetInfo info{XR_TYPE_ACTION_STATE_GET_INFO};
  info.action = action;
  info.subactionPath = XR_NULL_PATH;

  XrActionStateVector2f state{XR_TYPE_ACTION_STATE_VECTOR2F};
  if (XR_FAILED(xrGetActionStateVector2f(m_session, &info, &state)) || !state.isActive)
    return {0.0f, 0.0f};

  return state.currentState;
}

void
VRInput::update_button(BoolAction& action, Controller& controller,
                       std::initializer_list<Control> controls)
{
  const bool now = read_bool(action.action);
  if (now == action.state)
    return;

  action.state = now;
  for (Control control : controls)
    controller.set_control(control, now);
}

void
VRInput::update_axis(Vec2Action& action, AxisButtons& buttons, Controller& controller,
                     Control left, Control right, Control up, Control down)
{
  const XrVector2f value = read_vec2(action.action);
  action.state = value;

  auto update = [&controller](bool& current, float deflection, Control control)
  {
    const bool now = current ? (deflection > AXIS_RELEASE) : (deflection > AXIS_PRESS);
    if (now != current)
    {
      current = now;
      controller.set_control(control, now);
    }
  };

  update(buttons.left, -value.x, left);
  update(buttons.right, value.x, right);
  update(buttons.up, value.y, up);
  update(buttons.down, -value.y, down);
}

bool
VRInput::poll(XrTime time, Controller& controller)
{
  (void)time;

  XrActiveActionSet active{};
  active.actionSet = m_action_set;
  active.subactionPath = XR_NULL_PATH;

  XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO};
  sync.countActiveActionSets = 1;
  sync.activeActionSets = &active;

  const XrResult result = xrSyncActions(m_session, &sync);
  if (XR_FAILED(result))
    return false;

  // When the session isn't focused (system menu open), xrSyncActions
  // succeeds with XR_SESSION_NOT_FOCUSED and all actions report
  // inactive, which releases every held control below.

  update_axis(m_move, m_move_buttons, controller,
              Control::LEFT, Control::RIGHT, Control::UP, Control::DOWN);
  update_axis(m_peek, m_peek_buttons, controller,
              Control::PEEK_LEFT, Control::PEEK_RIGHT, Control::PEEK_UP, Control::PEEK_DOWN);

  update_button(m_jump, controller, { Control::JUMP, Control::MENU_SELECT });
  update_button(m_action, controller, { Control::ACTION });
  update_button(m_item, controller, { Control::ITEM });
  update_button(m_back, controller, { Control::MENU_BACK });
  update_button(m_start, controller, { Control::START });
  update_button(m_peek_left, controller, { Control::PEEK_LEFT });
  update_button(m_peek_right, controller, { Control::PEEK_RIGHT });
  update_button(m_cheat_menu, controller, { Control::CHEAT_MENU });

  const bool recenter_was = m_recenter.state;
  update_button(m_recenter, controller, {});
  return m_recenter.state && !recenter_was;
}

#endif // ENABLE_OPENXR
