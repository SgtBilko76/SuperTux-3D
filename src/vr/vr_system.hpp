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

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "math/size.hpp"
#include "util/currenton.hpp"
#include "video/layer_projection.hpp"

class Controller;
class VRInput;

/** Stereoscopic rendering and tracked-controller input through
    OpenXR (currently the OpenGL ES / Android binding used by the
    Meta Quest headsets).

    The flat 2D game is presented on a virtual screen floating in
    front of the player; drawing layers are spread out in depth so
    that backgrounds are farther away than the tiles, and HUD/menus
    are closer (see StereoLayerProjection).

    Frame flow, driven by Compositor / GLVideoSystem:

      begin_frame()            -> number of views to render (0 = skip)
      for each view:
        set_current_view(i)
        bind_current_view()    -> render into the eye framebuffer
        release_current_view()
      end_frame()

    Input flow, driven by ScreenManager:

      poll_input(controller)   -> maps controller actions to Control */
class VRSystem final : public Currenton<VRSystem>
{
public:
  /** Initializes OpenXR on top of the currently bound OpenGL ES
      context. Throws std::runtime_error if no runtime/headset is
      available, in which case the game keeps running flat. */
  VRSystem();
  ~VRSystem() override;

  /** Process runtime events and wait for the next frame. Returns the
      number of views (eyes) to render, or 0 if nothing should be
      rendered this frame. */
  int begin_frame();

  void set_current_view(int view);
  int get_current_view() const { return m_current_view; }

  /** Acquire the swapchain image of the current view and bind it as
      the active framebuffer. */
  void bind_current_view();
  void release_current_view();

  /** Submit the rendered views to the compositor. Safe to call even
      when begin_frame() returned 0. */
  void end_frame();

  /** Pixel size of a single view's render target. */
  Size get_view_size() const;

  /** Projection description for the current view. */
  const StereoLayerProjection& get_layer_projection() const { return m_layer_projection; }

  /** Logical 2D screen size the game should render at in VR. */
  Size get_logical_size() const;

  /** Update the controller state from the tracked controllers. */
  void poll_input(Controller& controller);

  /** Move the virtual screen so it is centered in front of the
      current head pose. */
  void recenter();

  bool is_session_running() const { return m_session_running; }
  bool is_focused() const { return m_session_focused; }

  /** Re-read the screen placement settings from the config. */
  void apply_config();

  std::string get_runtime_name() const { return m_runtime_name; }

private:
  struct Impl;

  void poll_events();
  void update_layer_projection();

private:
  std::unique_ptr<Impl> m_impl;
  std::unique_ptr<VRInput> m_input;

  std::string m_runtime_name;

  bool m_session_running;
  bool m_session_focused;
  bool m_frame_begun;
  bool m_should_render;

  int m_current_view;
  StereoLayerProjection m_layer_projection;

  /** Anchor transform of the virtual screen in LOCAL space. */
  glm::mat4 m_screen_anchor;

private:
  VRSystem(const VRSystem&) = delete;
  VRSystem& operator=(const VRSystem&) = delete;
};

#endif // ENABLE_OPENXR
