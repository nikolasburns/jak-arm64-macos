#include "opengl_utils.h"

#include <array>
#include <cstdio>

#include "common/log/log.h"
#include "common/util/Assert.h"

#include "game/graphics/opengl_renderer/BucketRenderer.h"

#include <SDL3/SDL_video.h>

void set_polygon_mode(GLenum mode) {
  if (!gfx_backend_is_angle()) {
    glPolygonMode(GL_FRONT_AND_BACK, mode);
    return;
  }

  // GL_ANGLE_polygon_mode. Resolved once, on first use, so this costs a single
  // branch afterwards. A context is current by the time any renderer draws.
  using PolygonModeANGLE = void(APIENTRY*)(GLenum, GLenum);
  static PolygonModeANGLE s_polygon_mode_angle = nullptr;
  static bool s_resolved = false;
  if (!s_resolved) {
    s_resolved = true;
    s_polygon_mode_angle =
        reinterpret_cast<PolygonModeANGLE>(SDL_GL_GetProcAddress("glPolygonModeANGLE"));
    if (s_polygon_mode_angle) {
      lg::info("gfx backend ANGLE: glPolygonModeANGLE available (debug wireframe supported)");
    } else {
      lg::warn(
          "gfx backend ANGLE: glPolygonModeANGLE unavailable; debug wireframe draws will render "
          "filled");
    }
  }
  if (s_polygon_mode_angle) {
    s_polygon_mode_angle(GL_FRONT_AND_BACK, mode);
  }
}

FramebufferTexturePair::FramebufferTexturePair(int w, int h, u64 texture_format, int num_levels)
    : m_w(w), m_h(h) {
  m_framebuffers.resize(num_levels);
  glGenFramebuffers(num_levels, m_framebuffers.data());
  glGenTextures(1, &m_texture);

  GLint old_framebuffer;
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_framebuffer);

  for (int i = 0; i < num_levels; i++) {
    glBindTexture(GL_TEXTURE_2D, m_texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, num_levels);
    glTexImage2D(GL_TEXTURE_2D, i, GL_RGBA8, w >> i, h >> i, 0, GL_RGBA, texture_format, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  }

  for (int i = 0; i < num_levels; i++) {
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, m_framebuffers[i]);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    // I don't know if we really need to do this. whatever uses this texture should figure it out.

    framebuffer_attach_color_texture(GL_COLOR_ATTACHMENT0 + i, m_texture, i);
    GLenum draw_buffers[1] = {GLenum(GL_COLOR_ATTACHMENT0 + i)};
    glDrawBuffers(1, draw_buffers);
    auto status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
      lg::error("Failed to setup framebuffer texture pair: {} {} ", w, h);
      switch (status) {
        case GL_FRAMEBUFFER_UNDEFINED:
          lg::error("GL_FRAMEBUFFER_UNDEFINED\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_DRAW_BUFFER\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_READ_BUFFER\n");
          break;
        case GL_FRAMEBUFFER_UNSUPPORTED:
          lg::error("GL_FRAMEBUFFER_UNSUPPORTED\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE\n");
          break;
        case GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS:
          lg::error("GL_FRAMEBUFFER_INCOMPLETE_LAYER_TARGETS\n");
          break;
      }

      ASSERT(false);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, old_framebuffer);
  }

  glBindFramebuffer(GL_FRAMEBUFFER, old_framebuffer);
}

FramebufferTexturePair::~FramebufferTexturePair() {
  if (m_moved_from) {
    return;
  }
  glDeleteFramebuffers(m_framebuffers.size(), m_framebuffers.data());
  glDeleteTextures(1, &m_texture);
}

FramebufferTexturePairContext::FramebufferTexturePairContext(FramebufferTexturePair& fb, int level)
    : m_fb(&fb) {
  glGetIntegerv(GL_VIEWPORT, m_old_viewport);
  glGetIntegerv(GL_FRAMEBUFFER_BINDING, &m_old_framebuffer);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fb->m_framebuffers[level]);
  glViewport(0, 0, m_fb->m_w, m_fb->m_h);
  if (level != 0) {
    // Re-point attachment 0 at this mip. Only OceanTexture's per-mip loop takes this
    // path: FramebufferTexturePair's constructor attached level i to
    // GL_COLOR_ATTACHMENT0 + i, so for i > 0 attachment 0 is not yet this level and
    // the re-attach is what actually makes the framebuffer render to the mip.
    //
    // For level 0 it is skipped, because there it re-attaches exactly what the pair's
    // constructor already attached and checked complete. Re-attaching a color texture
    // to the framebuffer that was just bound as the draw target is a no-op on desktop
    // GL but not on ANGLE-Metal: the next command that syncs draw-framebuffer state
    // (glClearBufferfv in EyeRenderer::run_gpu) goes through
    // onDrawFrameBufferChangedState -> endEncoding, flushing a render pass whose
    // encoder no longer matches its attachments, and that flush faults inside Metal
    // (AGXG16XFamilyRenderContext drawIndexedPrimitives, null render context). It is
    // below ANGLE's validation layer, so no GL error is ever raised.
    framebuffer_attach_color_texture(GL_COLOR_ATTACHMENT0, m_fb->m_texture, level);
  }
}

void FramebufferTexturePairContext::switch_to(FramebufferTexturePair& fb) {
  if (&fb != m_fb) {
    m_fb = &fb;
    glBindFramebuffer(GL_FRAMEBUFFER, m_fb->m_framebuffers[0]);
    glViewport(0, 0, m_fb->m_w, m_fb->m_h);
    // No color-texture re-attach here, deliberately. FramebufferTexturePair's
    // constructor already attached m_texture level 0 to GL_COLOR_ATTACHMENT0 of
    // m_framebuffers[0] and checked the result complete, so repeating it sets the
    // attachment it already has -- a no-op on desktop GL, which is why it stood.
    //
    // On ANGLE-Metal it is not a no-op: it mutates the attachments of the
    // framebuffer that was just made current, so the next command that touches
    // draw-framebuffer state (glClearBufferfv, for the eye renderer) sends ANGLE
    // through onDrawFrameBufferChangedState -> endEncoding to flush the render pass
    // that is still in flight from the *previous* eye, whose encoder now disagrees
    // with the attachment it was built against. That flush faults inside Metal
    // itself, at AGXG16XFamilyRenderContext drawIndexedPrimitives with a null
    // render context -- past our validation layer, so no GL error is ever set.
    //
    // The constructor's attach is left alone: it is what establishes the binding in
    // the first place, and it attaches level i to GL_COLOR_ATTACHMENT0 + i, which
    // OceanTexture's per-mip contexts (NUM_MIPS levels) depend on.
  }
}

FramebufferTexturePairContext::~FramebufferTexturePairContext() {
  glViewport(m_old_viewport[0], m_old_viewport[1], m_old_viewport[2], m_old_viewport[3]);
  glBindFramebuffer(GL_FRAMEBUFFER, m_old_framebuffer);
}

FullScreenDraw::FullScreenDraw() {
  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vertex_buffer);
  glBindVertexArray(m_vao);

  struct Vertex {
    float x, y;
  };

  std::array<Vertex, 4> vertices = {
      Vertex{-1, -1},
      Vertex{-1, 1},
      Vertex{1, -1},
      Vertex{1, 1},
  };

  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(Vertex) * 4, vertices.data(), GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribPointer(0,               // location 0 in the shader
                        2,               // 2 floats per vert
                        GL_FLOAT,        // floats
                        GL_TRUE,         // normalized, ignored,
                        sizeof(Vertex),  //
                        nullptr          //
  );

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

FullScreenDraw::~FullScreenDraw() {
  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_vertex_buffer);
}

void FullScreenDraw::draw(const math::Vector4f& color,
                          SharedRenderState* render_state,
                          ScopedProfilerNode& prof) {
  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  auto& shader = render_state->shaders[ShaderId::SOLID_COLOR];
  shader.activate();
  glUniform4f(glGetUniformLocation(shader.id(), "fragment_color"), color[0], color[1], color[2],
              color[3]);

  prof.add_tri(2);
  prof.add_draw_call();
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

FullScreenTexDraw::FullScreenTexDraw() {
  glGenVertexArrays(1, &m_vao);
  glGenBuffers(1, &m_vertex_buffer);
  glBindVertexArray(m_vao);

  std::array<int32_t, 4> vertices = {0, 1, 2, 3};

  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  glBufferData(GL_ARRAY_BUFFER, sizeof(int32_t) * 4, vertices.data(), GL_STATIC_DRAW);

  glEnableVertexAttribArray(0);
  glVertexAttribIPointer(0, 1, GL_INT, sizeof(int32_t), nullptr);

  glBindBuffer(GL_ARRAY_BUFFER, 0);
  glBindVertexArray(0);
}

FullScreenTexDraw::~FullScreenTexDraw() {
  glDeleteVertexArrays(1, &m_vao);
  glDeleteBuffers(1, &m_vertex_buffer);
}

void FullScreenTexDraw::draw(const math::Vector4f& color,
                             const math::Vector2f& tex0,
                             const math::Vector2f& tex1,
                             SharedRenderState* render_state,
                             ScopedProfilerNode& prof) {
  glBindVertexArray(m_vao);
  glBindBuffer(GL_ARRAY_BUFFER, m_vertex_buffer);
  auto& shader = render_state->shaders[ShaderId::SIMPLE_TEXTURE];
  shader.activate();
  glUniform4f(glGetUniformLocation(shader.id(), "color"), color[0], color[1], color[2], color[3]);
  glUniform2f(glGetUniformLocation(shader.id(), "tex_coord_0"), tex0.x(), tex0.y());
  glUniform2f(glGetUniformLocation(shader.id(), "tex_coord_1"), tex1.x(), tex1.y());

  prof.add_tri(2);
  prof.add_draw_call();
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

FramebufferCopier::FramebufferCopier() {
  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);

  glGenTextures(1, &m_fbo_texture);
  glBindTexture(GL_TEXTURE_2D, m_fbo_texture);

  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, m_fbo_width, m_fbo_height, 0, GL_RGB, GL_UNSIGNED_BYTE,
               NULL);

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_fbo_texture, 0);

  ASSERT(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);

  glBindTexture(GL_TEXTURE_2D, 0);
}

FramebufferCopier::~FramebufferCopier() {
  glDeleteTextures(1, &m_fbo_texture);
  glDeleteFramebuffers(1, &m_fbo);
}

void FramebufferCopier::copy_now(int render_fb_w, int render_fb_h, GLuint render_fb) {
  if (m_fbo_width != render_fb_w || m_fbo_height != render_fb_h) {
    m_fbo_width = render_fb_w;
    m_fbo_height = render_fb_h;

    glBindTexture(GL_TEXTURE_2D, m_fbo_texture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB8, m_fbo_width, m_fbo_height, 0, GL_RGB, GL_UNSIGNED_BYTE,
                 NULL);

    glBindTexture(GL_TEXTURE_2D, 0);
  }

  glBindFramebuffer(GL_READ_FRAMEBUFFER, render_fb);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_fbo);

  glBlitFramebuffer(0,                    // srcX0
                    0,                    // srcY0
                    render_fb_w,          // srcX1
                    render_fb_h,          // srcY1
                    0,                    // dstX0
                    0,                    // dstY0
                    m_fbo_width,          // dstX1
                    m_fbo_height,         // dstY1
                    GL_COLOR_BUFFER_BIT,  // mask
                    GL_NEAREST            // filter
  );

  glBindFramebuffer(GL_FRAMEBUFFER, render_fb);
}

void FramebufferCopier::copy_back_now(int render_fb_w, int render_fb_h, GLuint render_fb) {
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, render_fb);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);

  glBlitFramebuffer(0,                    // srcX0
                    0,                    // srcY0
                    m_fbo_width,          // srcX1
                    m_fbo_height,         // srcY1
                    0,                    // dstX0
                    0,                    // dstY0
                    render_fb_w,          // dstX1
                    render_fb_h,          // dstY1
                    GL_COLOR_BUFFER_BIT,  // mask
                    GL_NEAREST            // filter
  );

  glBindFramebuffer(GL_FRAMEBUFFER, render_fb);
}
