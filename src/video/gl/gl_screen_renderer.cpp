//  SuperTux
//  Copyright (C) 2006 Matthias Braun <matze@braunis.de>
//	Updated by GiBy 2013 for SDL3 <giby_the_kid@yahoo.fr>
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

#include "video/gl/gl_screen_renderer.hpp"

#include "math/rect.hpp"
#include "video/color.hpp"
#include "supertux/gameconfig.hpp"
#include "supertux/globals.hpp"
#include "util/log.hpp"
#include "video/gl/gl_context.hpp"
#include "video/gl/gl_program.hpp"
#include "video/gl/gl_vertex_arrays.hpp"
#include "video/gl/gl_video_system.hpp"
#include "video/glutil.hpp"
#include "vr/vr_system.hpp"

GLScreenRenderer::GLScreenRenderer(GLVideoSystem& video_system) :
  GLRenderer(video_system)
{
}

GLScreenRenderer::~GLScreenRenderer()
{
}

void
GLScreenRenderer::start_draw()
{
  assert_gl();

  GLContext& context = m_video_system.get_context();
  context.bind();

  context.blend_func(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

#ifdef ENABLE_OPENXR
  if (VRSystem* vr = VRSystem::current())
  {
    // Draw into the eye's swapchain image with a per-layer perspective
    // projection instead of the window with a flat orthographic one.
    vr->bind_current_view();
    context.set_stereo_projection(vr->get_layer_projection());

    glClearColor(0, 0, 0, 1);
    glClearStencil(0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // Layers farther away than the reference plane are drawn larger than
    // the virtual screen and would spill out past its edges. Mark the
    // outline of the screen (as seen from this eye) in the stencil buffer
    // and only draw inside it, so the scene looks like a window.
    const Viewport& viewport = m_video_system.get_viewport();
    const float width = static_cast<float>(viewport.get_screen_width());
    const float height = static_cast<float>(viewport.get_screen_height());
    const float vertices[] = {
      0.0f, 0.0f,
      width, 0.0f,
      0.0f, height,
      width, height,
    };

    glEnable(GL_STENCIL_TEST);
    glStencilMask(0xFF);
    glStencilFunc(GL_ALWAYS, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);

    context.set_layer(vr->get_layer_projection().reference_layer);
    context.bind_no_texture();
    context.set_texcoord(0.0f, 0.0f);
    context.set_color(Color::WHITE);
    context.set_positions(vertices, sizeof(vertices));
    context.draw_arrays(GL_TRIANGLE_STRIP, 0, 4);

    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilFunc(GL_EQUAL, 1, 0xFF);
    glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
    glStencilMask(0x00);

    assert_gl();
    return;
  }
#endif

  const Viewport& viewport = m_video_system.get_viewport();
  const Rect& rect = viewport.get_rect();

  glViewport(rect.left, rect.top, rect.get_width(), rect.get_height());

  context.ortho(static_cast<float>(viewport.get_screen_width()),
                static_cast<float>(viewport.get_screen_height()),
                true);

  // Clear the screen to get rid of lightmap remains.
  glClearColor(0, 0, 0, 1);
  glClear(GL_COLOR_BUFFER_BIT);

  assert_gl();
}

void
GLScreenRenderer::end_draw()
{
#ifdef ENABLE_OPENXR
  if (VRSystem* vr = VRSystem::current())
  {
    glStencilMask(0xFF);
    glDisable(GL_STENCIL_TEST);
    vr->release_current_view();
  }
#endif
}

Rect
GLScreenRenderer::get_rect() const
{
#ifdef ENABLE_OPENXR
  if (VRSystem* vr = VRSystem::current())
    return Rect(0, 0, vr->get_view_size());
#endif

  const Viewport& viewport = m_video_system.get_viewport();
  return viewport.get_rect();
}

Size
GLScreenRenderer::get_logical_size() const
{
  const Viewport& viewport = m_video_system.get_viewport();
  return Size(viewport.get_screen_width(),
              viewport.get_screen_height());
}
