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

#include "vr/vr_system.hpp"

#ifdef ENABLE_OPENXR

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <EGL/egl.h>
#include <jni.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "control/controller.hpp"
#include "supertux/gameconfig.hpp"
#include "supertux/globals.hpp"
#include "util/log.hpp"
#include "video/gl.hpp"
#include "video/glutil.hpp"
#include "vr/vr_input.hpp"

#ifndef GL_FRAMEBUFFER_SRGB_EXT
#  define GL_FRAMEBUFFER_SRGB_EXT 0x8DB9
#endif
#ifndef GL_SRGB8_ALPHA8
#  define GL_SRGB8_ALPHA8 0x8C43
#endif
#ifndef GL_RGBA8
#  define GL_RGBA8 0x8058
#endif

namespace {

constexpr XrViewConfigurationType VIEW_CONFIG_TYPE = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
constexpr float NEAR_PLANE = 0.05f;
constexpr float FAR_PLANE = 100.0f;

/** Logical 2D resolution used in VR; the widest 16:10 layout SuperTux supports. */
const Size VR_LOGICAL_SIZE(1280, 800);

void check(XrResult result, const char* what)
{
  if (XR_FAILED(result))
  {
    throw std::runtime_error(std::string("OpenXR: ") + what + " failed with result " + std::to_string(result));
  }
}

/** Off-center perspective projection from an OpenXR field of view (OpenGL clip space). */
glm::mat4 projection_from_fov(const XrFovf& fov, float near_plane, float far_plane)
{
  const float tan_left = std::tan(fov.angleLeft);
  const float tan_right = std::tan(fov.angleRight);
  const float tan_down = std::tan(fov.angleDown);
  const float tan_up = std::tan(fov.angleUp);

  const float width = tan_right - tan_left;
  const float height = tan_up - tan_down;

  glm::mat4 m(0.0f);
  m[0][0] = 2.0f / width;
  m[2][0] = (tan_right + tan_left) / width;
  m[1][1] = 2.0f / height;
  m[2][1] = (tan_up + tan_down) / height;
  m[2][2] = -(far_plane + near_plane) / (far_plane - near_plane);
  m[3][2] = -(2.0f * far_plane * near_plane) / (far_plane - near_plane);
  m[2][3] = -1.0f;
  return m;
}

glm::mat4 matrix_from_pose(const XrPosef& pose)
{
  const glm::quat orientation(pose.orientation.w, pose.orientation.x, pose.orientation.y, pose.orientation.z);
  const glm::vec3 position(pose.position.x, pose.position.y, pose.position.z);
  return glm::translate(glm::mat4(1.0f), position) * glm::mat4_cast(orientation);
}

bool has_gl_extension(const char* name)
{
  const char* extensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
  return extensions && std::strstr(extensions, name) != nullptr;
}

} // namespace

struct VRSystem::Impl final
{
  struct View
  {
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int32_t width = 0;
    int32_t height = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> framebuffers;
    /** Stencil buffer shared by the view's framebuffers, used to clip
        the layers to the outline of the virtual screen. */
    GLuint stencil = 0;
    uint32_t acquired_index = 0;
    bool acquired = false;
  };

  XrInstance instance = XR_NULL_HANDLE;
  XrSystemId system_id = XR_NULL_SYSTEM_ID;
  XrSession session = XR_NULL_HANDLE;
  XrSpace local_space = XR_NULL_HANDLE;
  XrSpace view_space = XR_NULL_HANDLE;

  std::vector<XrViewConfigurationView> config_views;
  std::vector<View> views;
  std::vector<XrView> located_views;
  XrViewState view_state{XR_TYPE_VIEW_STATE};
  XrFrameState frame_state{XR_TYPE_FRAME_STATE};
  XrSessionState session_state = XR_SESSION_STATE_UNKNOWN;
  XrTime last_display_time = 0;

  int64_t swapchain_format = 0;
  bool srgb_write_control = false;
  bool session_dead = false;
};

VRSystem::VRSystem() :
  m_impl(new Impl),
  m_input(),
  m_runtime_name(),
  m_session_running(false),
  m_session_focused(false),
  m_frame_begun(false),
  m_should_render(false),
  m_current_view(0),
  m_layer_projection(),
  m_screen_anchor(1.0f)
{
  Impl& impl = *m_impl;

  // The OpenXR loader needs the JavaVM and activity on Android to find the runtime.
  JNIEnv* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
  JavaVM* vm = nullptr;
  if (!env || env->GetJavaVM(&vm) != JNI_OK || !vm)
    throw std::runtime_error("OpenXR: unable to get the JavaVM from SDL");
  // SDL hands out a local reference; the loader and runtime keep the
  // activity around, so give them a global one.
  jobject local_activity = static_cast<jobject>(SDL_GetAndroidActivity());
  if (!local_activity)
    throw std::runtime_error("OpenXR: unable to get the Android activity from SDL");
  jobject activity = env->NewGlobalRef(local_activity);
  env->DeleteLocalRef(local_activity);

  PFN_xrInitializeLoaderKHR initialize_loader = nullptr;
  xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                        reinterpret_cast<PFN_xrVoidFunction*>(&initialize_loader));
  if (initialize_loader)
  {
    XrLoaderInitInfoAndroidKHR loader_info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    loader_info.applicationVM = vm;
    loader_info.applicationContext = activity;
    check(initialize_loader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR*>(&loader_info)),
          "xrInitializeLoaderKHR");
  }

  // Instance.
  const char* extensions[] = {
    XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
  };

  XrInstanceCreateInfoAndroidKHR android_info{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
  android_info.applicationVM = vm;
  android_info.applicationActivity = activity;

  XrInstanceCreateInfo instance_info{XR_TYPE_INSTANCE_CREATE_INFO};
  instance_info.next = &android_info;
  instance_info.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]);
  instance_info.enabledExtensionNames = extensions;
  std::strncpy(instance_info.applicationInfo.applicationName, "SuperTux", XR_MAX_APPLICATION_NAME_SIZE - 1);
  instance_info.applicationInfo.applicationVersion = 1;
  std::strncpy(instance_info.applicationInfo.engineName, "SuperTux", XR_MAX_ENGINE_NAME_SIZE - 1);
  instance_info.applicationInfo.engineVersion = 1;
  instance_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  check(xrCreateInstance(&instance_info, &impl.instance), "xrCreateInstance");

  XrInstanceProperties instance_props{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(impl.instance, &instance_props)))
  {
    m_runtime_name = instance_props.runtimeName;
    log_info << "OpenXR runtime: " << m_runtime_name << " "
             << XR_VERSION_MAJOR(instance_props.runtimeVersion) << "."
             << XR_VERSION_MINOR(instance_props.runtimeVersion) << "."
             << XR_VERSION_PATCH(instance_props.runtimeVersion) << std::endl;
  }

  // System (the headset).
  XrSystemGetInfo system_info{XR_TYPE_SYSTEM_GET_INFO};
  system_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
  check(xrGetSystem(impl.instance, &system_info, &impl.system_id), "xrGetSystem");

  XrSystemProperties system_props{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(impl.instance, impl.system_id, &system_props)))
  {
    log_info << "OpenXR system: " << system_props.systemName << std::endl;
  }

  // The spec requires querying the graphics requirements before creating a session.
  PFN_xrGetOpenGLESGraphicsRequirementsKHR get_requirements = nullptr;
  check(xrGetInstanceProcAddr(impl.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                              reinterpret_cast<PFN_xrVoidFunction*>(&get_requirements)),
        "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR)");
  XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
  check(get_requirements(impl.instance, impl.system_id, &requirements), "xrGetOpenGLESGraphicsRequirementsKHR");
  log_info << "OpenXR requires OpenGL ES "
           << XR_VERSION_MAJOR(requirements.minApiVersionSupported) << "."
           << XR_VERSION_MINOR(requirements.minApiVersionSupported) << " to "
           << XR_VERSION_MAJOR(requirements.maxApiVersionSupported) << "."
           << XR_VERSION_MINOR(requirements.maxApiVersionSupported) << std::endl;

  // Session, bound to the EGL context SDL created for us.
  EGLDisplay display = eglGetCurrentDisplay();
  EGLContext context = eglGetCurrentContext();
  if (display == EGL_NO_DISPLAY || context == EGL_NO_CONTEXT)
    throw std::runtime_error("OpenXR: no EGL context is current");

  EGLint config_id = 0;
  if (!eglQueryContext(display, context, EGL_CONFIG_ID, &config_id))
    throw std::runtime_error("OpenXR: eglQueryContext(EGL_CONFIG_ID) failed");

  const EGLint config_attribs[] = { EGL_CONFIG_ID, config_id, EGL_NONE };
  EGLConfig config = nullptr;
  EGLint num_configs = 0;
  if (!eglChooseConfig(display, config_attribs, &config, 1, &num_configs) || num_configs < 1)
    throw std::runtime_error("OpenXR: unable to look up the EGL config of the current context");

  XrGraphicsBindingOpenGLESAndroidKHR graphics_binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
  graphics_binding.display = display;
  graphics_binding.config = config;
  graphics_binding.context = context;

  XrSessionCreateInfo session_info{XR_TYPE_SESSION_CREATE_INFO};
  session_info.next = &graphics_binding;
  session_info.systemId = impl.system_id;
  check(xrCreateSession(impl.instance, &session_info, &impl.session), "xrCreateSession");

  // Reference spaces.
  XrReferenceSpaceCreateInfo space_info{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  space_info.poseInReferenceSpace.orientation.w = 1.0f;
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  check(xrCreateReferenceSpace(impl.session, &space_info, &impl.local_space), "xrCreateReferenceSpace(LOCAL)");
  space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
  check(xrCreateReferenceSpace(impl.session, &space_info, &impl.view_space), "xrCreateReferenceSpace(VIEW)");

  // Views.
  uint32_t view_count = 0;
  check(xrEnumerateViewConfigurationViews(impl.instance, impl.system_id, VIEW_CONFIG_TYPE, 0, &view_count, nullptr),
        "xrEnumerateViewConfigurationViews");
  if (view_count == 0)
    throw std::runtime_error("OpenXR: stereo view configuration reports no views");
  impl.config_views.resize(view_count, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
  check(xrEnumerateViewConfigurationViews(impl.instance, impl.system_id, VIEW_CONFIG_TYPE, view_count, &view_count,
                                          impl.config_views.data()),
        "xrEnumerateViewConfigurationViews");
  impl.located_views.resize(view_count, {XR_TYPE_VIEW});

  // Swapchain format. The 2D artwork is already sRGB encoded, so we want the
  // compositor to treat the pixels as sRGB without the GL driver re-encoding
  // them on write; that needs GL_EXT_sRGB_write_control. Otherwise fall back
  // to a plain RGBA8 swapchain.
  uint32_t format_count = 0;
  check(xrEnumerateSwapchainFormats(impl.session, 0, &format_count, nullptr), "xrEnumerateSwapchainFormats");
  std::vector<int64_t> formats(format_count);
  check(xrEnumerateSwapchainFormats(impl.session, format_count, &format_count, formats.data()),
        "xrEnumerateSwapchainFormats");

  impl.srgb_write_control = has_gl_extension("GL_EXT_sRGB_write_control");
  auto supports_format = [&formats](int64_t format) {
    return std::find(formats.begin(), formats.end(), format) != formats.end();
  };
  if (impl.srgb_write_control && supports_format(GL_SRGB8_ALPHA8))
    impl.swapchain_format = GL_SRGB8_ALPHA8;
  else if (supports_format(GL_RGBA8))
    impl.swapchain_format = GL_RGBA8;
  else if (!formats.empty())
    impl.swapchain_format = formats[0];
  else
    throw std::runtime_error("OpenXR: runtime offers no swapchain formats");

  log_info << "OpenXR swapchain format 0x" << std::hex << impl.swapchain_format << std::dec
           << (impl.srgb_write_control ? " (sRGB write control available)" : "") << std::endl;

  // Swapchains and framebuffers, one per view.
  impl.views.resize(view_count);
  for (uint32_t i = 0; i < view_count; ++i)
  {
    Impl::View& view = impl.views[i];
    view.width = static_cast<int32_t>(impl.config_views[i].recommendedImageRectWidth);
    view.height = static_cast<int32_t>(impl.config_views[i].recommendedImageRectHeight);

    XrSwapchainCreateInfo swapchain_info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    swapchain_info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    swapchain_info.format = impl.swapchain_format;
    swapchain_info.sampleCount = 1;
    swapchain_info.width = static_cast<uint32_t>(view.width);
    swapchain_info.height = static_cast<uint32_t>(view.height);
    swapchain_info.faceCount = 1;
    swapchain_info.arraySize = 1;
    swapchain_info.mipCount = 1;
    check(xrCreateSwapchain(impl.session, &swapchain_info, &view.swapchain), "xrCreateSwapchain");

    uint32_t image_count = 0;
    check(xrEnumerateSwapchainImages(view.swapchain, 0, &image_count, nullptr), "xrEnumerateSwapchainImages");
    view.images.resize(image_count, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    check(xrEnumerateSwapchainImages(view.swapchain, image_count, &image_count,
                                     reinterpret_cast<XrSwapchainImageBaseHeader*>(view.images.data())),
          "xrEnumerateSwapchainImages");

    glGenRenderbuffers(1, &view.stencil);
    glBindRenderbuffer(GL_RENDERBUFFER, view.stencil);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, view.width, view.height);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    view.framebuffers.resize(image_count, 0);
    glGenFramebuffers(static_cast<GLsizei>(image_count), view.framebuffers.data());
    for (uint32_t j = 0; j < image_count; ++j)
    {
      glBindFramebuffer(GL_FRAMEBUFFER, view.framebuffers[j]);
      glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, view.images[j].image, 0);
      glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, view.stencil);
      const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
      if (status != GL_FRAMEBUFFER_COMPLETE)
      {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        throw std::runtime_error("OpenXR: eye framebuffer incomplete, status " + std::to_string(status));
      }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    log_info << "OpenXR view " << i << ": " << view.width << "x" << view.height
             << ", " << image_count << " swapchain images" << std::endl;
  }
  assert_gl();

  // Controllers.
  m_input.reset(new VRInput(impl.instance, impl.session));
  m_input->attach();

  apply_config();

  log_info << "VR initialized (" << view_count << " views)" << std::endl;
}

VRSystem::~VRSystem()
{
  Impl& impl = *m_impl;

  m_input.reset();

  for (Impl::View& view : impl.views)
  {
    if (!view.framebuffers.empty())
      glDeleteFramebuffers(static_cast<GLsizei>(view.framebuffers.size()), view.framebuffers.data());
    if (view.stencil != 0)
      glDeleteRenderbuffers(1, &view.stencil);
    if (view.swapchain != XR_NULL_HANDLE)
      xrDestroySwapchain(view.swapchain);
  }

  if (impl.view_space != XR_NULL_HANDLE)
    xrDestroySpace(impl.view_space);
  if (impl.local_space != XR_NULL_HANDLE)
    xrDestroySpace(impl.local_space);
  if (impl.session != XR_NULL_HANDLE)
    xrDestroySession(impl.session);
  if (impl.instance != XR_NULL_HANDLE)
    xrDestroyInstance(impl.instance);
}

void
VRSystem::apply_config()
{
  m_layer_projection.screen_distance = std::max(0.5f, g_config->vr_screen_distance);
  m_layer_projection.screen_width = std::max(0.5f, g_config->vr_screen_width);
  m_layer_projection.depth_strength = std::clamp(g_config->vr_depth_strength, 0.0f, 3.0f);
  m_layer_projection.logical_width = static_cast<float>(VR_LOGICAL_SIZE.width);
  m_layer_projection.logical_height = static_cast<float>(VR_LOGICAL_SIZE.height);
}

Size
VRSystem::get_logical_size() const
{
  return VR_LOGICAL_SIZE;
}

Size
VRSystem::get_view_size() const
{
  const Impl::View& view = m_impl->views[m_current_view];
  return Size(view.width, view.height);
}

void
VRSystem::poll_events()
{
  Impl& impl = *m_impl;

  XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
  while (xrPollEvent(impl.instance, &event) == XR_SUCCESS)
  {
    switch (event.type)
    {
      case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
      {
        const auto& changed = *reinterpret_cast<const XrEventDataSessionStateChanged*>(&event);
        impl.session_state = changed.state;
        log_debug << "OpenXR session state " << changed.state << std::endl;

        switch (changed.state)
        {
          case XR_SESSION_STATE_READY:
          {
            XrSessionBeginInfo begin_info{XR_TYPE_SESSION_BEGIN_INFO};
            begin_info.primaryViewConfigurationType = VIEW_CONFIG_TYPE;
            const XrResult result = xrBeginSession(impl.session, &begin_info);
            if (XR_SUCCEEDED(result))
            {
              m_session_running = true;
            }
            else
            {
              log_warning << "OpenXR: xrBeginSession failed with result " << result << std::endl;
            }
            break;
          }

          case XR_SESSION_STATE_STOPPING:
            m_session_running = false;
            m_session_focused = false;
            m_frame_begun = false;
            xrEndSession(impl.session);
            break;

          case XR_SESSION_STATE_EXITING:
          case XR_SESSION_STATE_LOSS_PENDING:
            m_session_running = false;
            m_session_focused = false;
            impl.session_dead = true;
            log_warning << "OpenXR session ended" << std::endl;
            break;

          case XR_SESSION_STATE_FOCUSED:
            m_session_focused = true;
            break;

          default:
            m_session_focused = false;
            break;
        }
        break;
      }

      case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
        impl.session_dead = true;
        m_session_running = false;
        log_warning << "OpenXR instance loss pending" << std::endl;
        break;

      case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
        // The runtime recentered the LOCAL space for us (system "reset view").
        m_screen_anchor = glm::mat4(1.0f);
        break;

      case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED:
        log_info << "OpenXR interaction profile changed" << std::endl;
        break;

      default:
        break;
    }

    event = {XR_TYPE_EVENT_DATA_BUFFER};
  }
}

int
VRSystem::begin_frame()
{
  Impl& impl = *m_impl;

  m_should_render = false;
  poll_events();

  if (impl.session_dead)
    return 0;

  if (!m_session_running)
  {
    // Nothing to present; don't spin the CPU while the runtime has us parked.
    SDL_Delay(10);
    return 0;
  }

  XrFrameWaitInfo wait_info{XR_TYPE_FRAME_WAIT_INFO};
  impl.frame_state = {XR_TYPE_FRAME_STATE};
  XrResult result = xrWaitFrame(impl.session, &wait_info, &impl.frame_state);
  if (XR_FAILED(result))
  {
    log_warning << "OpenXR: xrWaitFrame failed with result " << result << std::endl;
    return 0;
  }

  XrFrameBeginInfo begin_info{XR_TYPE_FRAME_BEGIN_INFO};
  result = xrBeginFrame(impl.session, &begin_info);
  if (XR_FAILED(result))
  {
    log_warning << "OpenXR: xrBeginFrame failed with result " << result << std::endl;
    return 0;
  }
  m_frame_begun = true;
  impl.last_display_time = impl.frame_state.predictedDisplayTime;

  if (!impl.frame_state.shouldRender)
    return 0;

  XrViewLocateInfo locate_info{XR_TYPE_VIEW_LOCATE_INFO};
  locate_info.viewConfigurationType = VIEW_CONFIG_TYPE;
  locate_info.displayTime = impl.frame_state.predictedDisplayTime;
  locate_info.space = impl.local_space;

  impl.view_state = {XR_TYPE_VIEW_STATE};
  uint32_t view_count = 0;
  result = xrLocateViews(impl.session, &locate_info, &impl.view_state,
                         static_cast<uint32_t>(impl.located_views.size()), &view_count,
                         impl.located_views.data());
  if (XR_FAILED(result) || view_count == 0)
  {
    log_warning << "OpenXR: xrLocateViews failed with result " << result << std::endl;
    return 0;
  }

  m_should_render = true;
  set_current_view(0);
  return static_cast<int>(view_count);
}

void
VRSystem::set_current_view(int view)
{
  m_current_view = std::clamp(view, 0, static_cast<int>(m_impl->views.size()) - 1);
  update_layer_projection();
}

void
VRSystem::update_layer_projection()
{
  const XrView& view = m_impl->located_views[m_current_view];
  const glm::mat4 projection = projection_from_fov(view.fov, NEAR_PLANE, FAR_PLANE);
  const glm::mat4 view_matrix = glm::inverse(matrix_from_pose(view.pose));

  m_layer_projection.view_projection = projection * view_matrix;
  m_layer_projection.anchor = m_screen_anchor;
}

void
VRSystem::bind_current_view()
{
  Impl::View& view = m_impl->views[m_current_view];
  assert(!view.acquired);

  XrSwapchainImageAcquireInfo acquire_info{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  check(xrAcquireSwapchainImage(view.swapchain, &acquire_info, &view.acquired_index), "xrAcquireSwapchainImage");

  XrSwapchainImageWaitInfo wait_info{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wait_info.timeout = XR_INFINITE_DURATION;
  check(xrWaitSwapchainImage(view.swapchain, &wait_info), "xrWaitSwapchainImage");
  view.acquired = true;

  glBindFramebuffer(GL_FRAMEBUFFER, view.framebuffers[view.acquired_index]);
  glViewport(0, 0, view.width, view.height);
  glDisable(GL_SCISSOR_TEST);

  if (m_impl->srgb_write_control)
    glDisable(GL_FRAMEBUFFER_SRGB_EXT);
}

void
VRSystem::release_current_view()
{
  Impl::View& view = m_impl->views[m_current_view];
  if (!view.acquired)
    return;

  glBindFramebuffer(GL_FRAMEBUFFER, 0);

  XrSwapchainImageReleaseInfo release_info{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
  xrReleaseSwapchainImage(view.swapchain, &release_info);
  view.acquired = false;
}

void
VRSystem::end_frame()
{
  Impl& impl = *m_impl;

  if (!m_frame_begun)
    return;
  m_frame_begun = false;

  // Make sure nothing is left acquired, the runtime refuses the frame otherwise.
  for (size_t i = 0; i < impl.views.size(); ++i)
  {
    if (impl.views[i].acquired)
    {
      m_current_view = static_cast<int>(i);
      release_current_view();
    }
  }

  std::vector<XrCompositionLayerProjectionView> projection_views(impl.views.size(),
                                                                 {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW});
  for (size_t i = 0; i < impl.views.size(); ++i)
  {
    projection_views[i].pose = impl.located_views[i].pose;
    projection_views[i].fov = impl.located_views[i].fov;
    projection_views[i].subImage.swapchain = impl.views[i].swapchain;
    projection_views[i].subImage.imageRect.offset = {0, 0};
    projection_views[i].subImage.imageRect.extent = {impl.views[i].width, impl.views[i].height};
    projection_views[i].subImage.imageArrayIndex = 0;
  }

  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
  layer.layerFlags = 0;
  layer.space = impl.local_space;
  layer.viewCount = static_cast<uint32_t>(projection_views.size());
  layer.views = projection_views.data();

  const XrCompositionLayerBaseHeader* layers[] = {
    reinterpret_cast<const XrCompositionLayerBaseHeader*>(&layer)
  };

  XrFrameEndInfo end_info{XR_TYPE_FRAME_END_INFO};
  end_info.displayTime = impl.frame_state.predictedDisplayTime;
  end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
  end_info.layerCount = m_should_render ? 1 : 0;
  end_info.layers = m_should_render ? layers : nullptr;

  const XrResult result = xrEndFrame(impl.session, &end_info);
  if (XR_FAILED(result))
  {
    log_warning << "OpenXR: xrEndFrame failed with result " << result << std::endl;
  }
  m_should_render = false;
}

void
VRSystem::poll_input(Controller& controller)
{
  Impl& impl = *m_impl;

  if (impl.session_dead || !m_session_running || !m_input)
    return;

  if (m_input->poll(impl.last_display_time, controller))
  {
    recenter();
  }
}

void
VRSystem::recenter()
{
  Impl& impl = *m_impl;

  if (!m_session_running || impl.last_display_time == 0)
    return;

  XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
  if (XR_FAILED(xrLocateSpace(impl.view_space, impl.local_space, impl.last_display_time, &location)))
    return;

  const bool valid = (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
                     (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT);
  if (!valid)
    return;

  // Keep only the yaw of the head so the screen stays upright and level.
  const glm::quat orientation(location.pose.orientation.w, location.pose.orientation.x,
                              location.pose.orientation.y, location.pose.orientation.z);
  const glm::vec3 forward = orientation * glm::vec3(0.0f, 0.0f, -1.0f);
  const float yaw = std::atan2(-forward.x, -forward.z);

  const glm::vec3 position(location.pose.position.x, location.pose.position.y, location.pose.position.z);
  m_screen_anchor = glm::translate(glm::mat4(1.0f), position) *
                    glm::rotate(glm::mat4(1.0f), yaw, glm::vec3(0.0f, 1.0f, 0.0f));

  log_info << "VR screen recentered" << std::endl;
}

#endif // ENABLE_OPENXR
