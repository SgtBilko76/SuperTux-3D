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

#include <algorithm>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

/** Describes how the flat, layered 2D scene is placed into a 3D
    world for stereoscopic rendering.

    The logical 2D screen (e.g. 1280x800 units) is mapped onto a
    "virtual screen" plane that floats in front of the viewer. Each
    drawing layer (see video/layer.hpp) is pushed to its own depth:
    background layers move away from the viewer, foreground/HUD
    layers move closer. Every plane is scaled so that, seen from the
    center between the eyes, it lines up exactly with the original 2D
    layout; the parallax comes purely from the eye offset. */
struct StereoLayerProjection final
{
  /** Combined projection * view matrix of the eye being rendered. */
  glm::mat4 view_projection = glm::mat4(1.0f);

  /** Pose of the virtual screen's center in tracking space (used for
      recentering). Identity places it straight ahead of the initial
      head pose. */
  glm::mat4 anchor = glm::mat4(1.0f);

  /** Distance from the viewer to the reference plane (layer 0), in meters. */
  float screen_distance = 3.0f;

  /** Width of the reference plane in meters. */
  float screen_width = 4.0f;

  /** Scales how strongly layers are separated in depth. 0 makes the
      scene flat, 1 is the default separation. */
  float depth_strength = 1.0f;

  /** Size of the logical 2D screen in units. */
  float logical_width = 1280.0f;
  float logical_height = 800.0f;

  /** Layer that is placed exactly on the reference plane. */
  int reference_layer = 0;

  /** Range of layers that maps onto the full depth range. */
  int min_layer = -300;
  int max_layer = 600;

  /** Depth (in meters) of the plane a given layer is drawn on. The
      mapping is linear in 1/depth (i.e. in stereo disparity), so
      equal layer steps produce equal on-screen parallax steps. */
  float depth_for_layer(int layer) const
  {
    const float span = static_cast<float>(max_layer - min_layer);
    // At min_layer the plane is twice as far as the reference plane,
    // at max_layer it is at half the reference distance.
    const float disparity_per_layer = (1.0f / screen_distance) / span * 2.0f * depth_strength;
    const float rel = static_cast<float>(layer - reference_layer);
    float inv_depth = 1.0f / screen_distance + rel * disparity_per_layer;
    inv_depth = std::clamp(inv_depth, 1.0f / 50.0f, 1.0f / 0.4f);
    return 1.0f / inv_depth;
  }

  /** Model matrix mapping logical 2D coordinates (origin top-left, y
      down) of the given layer into tracking space. */
  glm::mat4 model_for_layer(int layer) const
  {
    const float depth = depth_for_layer(layer);
    const float scale = depth / screen_distance;
    const float units_to_meters = screen_width / logical_width * scale;

    glm::mat4 m = anchor;
    m = glm::translate(m, glm::vec3(0.0f, 0.0f, -depth));
    m = glm::scale(m, glm::vec3(units_to_meters, -units_to_meters, 1.0f));
    m = glm::translate(m, glm::vec3(-logical_width / 2.0f, -logical_height / 2.0f, 0.0f));
    return m;
  }

  glm::mat4 mvp_for_layer(int layer) const
  {
    return view_projection * model_for_layer(layer);
  }
};
