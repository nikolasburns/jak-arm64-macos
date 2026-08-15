#pragma once

#include "common/math/Vector.h"

#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/pipelines/opengl.h"

struct SharedRenderState;
class ScopedProfilerNode;

/*!
 * Enable primitive restart with UINT32_MAX as the restart index.
 *
 * Every draw in this renderer indexes with GL_UNSIGNED_INT and restarts on
 * UINT32_MAX, which is exactly GLES 3.0's *fixed* restart index -- so on GLES the
 * whole thing is GL_PRIMITIVE_RESTART_FIXED_INDEX and there is no index to set.
 *
 * Desktop GL is NOT the same, and this is why the calls cannot simply be deleted:
 * GL_PRIMITIVE_RESTART_FIXED_INDEX is core only in GL 4.3+, and the macOS system
 * GL context is 4.1 (see opengl.cpp -- 4.3 is requested off-Apple, 4.1 on it).
 * There, restart is GL_PRIMITIVE_RESTART with a user-supplied index that defaults
 * to 0, so dropping glPrimitiveRestartIndex would restart on index 0 and corrupt
 * every indexed draw rather than fail loudly.
 */
inline void enable_primitive_restart_u32() {
  if (gfx_backend_is_angle()) {
    glEnable(GL_PRIMITIVE_RESTART_FIXED_INDEX);
  } else {
    glEnable(GL_PRIMITIVE_RESTART);
    glPrimitiveRestartIndex(UINT32_MAX);
  }
}

/*!
 * Set the polygon rasterization mode (GL_FILL / GL_LINE) for GL_FRONT_AND_BACK.
 *
 * glPolygonMode is desktop-GL only: GLES 3.0 has no such entry point, and every
 * caller here is a debug wireframe toggle that brackets a draw with LINE then
 * restores FILL. ANGLE exposes the same functionality through
 * GL_ANGLE_polygon_mode as glPolygonModeANGLE, resolved dynamically because our
 * glad loader is generated for desktop GL and knows nothing about it.
 *
 * If the extension is absent the call is dropped rather than faked: the only
 * consequence is that debug wireframe draws render filled. Nothing on the normal
 * render path calls this.
 */
void set_polygon_mode(GLenum mode);

/*!
 * Attach one mip level of a 2D texture as a framebuffer color attachment.
 *
 * Every attachment in this renderer is a single mip level of a plain
 * GL_TEXTURE_2D, which is exactly what glFramebufferTexture2D does -- and it is
 * core in both desktop GL and GLES 3.0.
 *
 * glFramebufferTexture (no "2D") is NOT the same call and is not in GLES 3.0.
 * It is the *layered* attachment entry point, added in desktop GL 3.2 to attach
 * a whole array/cubemap/3D texture at once for geometry shaders to index with
 * gl_Layer. On a 2D texture it degenerates to the same result, which is why the
 * desktop path never noticed the difference.
 *
 * The failure mode on GLES is quiet and worth knowing, because it is not the
 * null-pointer class: ANGLE *exports* glFramebufferTexture (as the
 * GL_EXT_geometry_shader entry point), so the loader binds it and the call
 * dispatches normally. ANGLE then rejects it in a GLES 3.0 context, sets an
 * error nobody reads, and attaches nothing -- so the only symptom is
 * GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT at the completeness check, one
 * call later, naming neither the function nor the reason.
 */
inline void framebuffer_attach_color_texture(GLenum attachment, GLuint texture, int level) {
  glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, texture, level);
}

/*!
 * This is a wrapper around a framebuffer and texture to make it easier to render to a texture.
 */
class FramebufferTexturePair {
 public:
  FramebufferTexturePair(int w, int h, u64 texture_format, int num_levels = 1);
  ~FramebufferTexturePair();

  GLuint texture() const { return m_texture; }

  void update_texture_size(int w, int h) {
    m_w = w;
    m_h = h;
  }

  void update_texture_unsafe(GLuint texture) { m_texture = texture; }

  FramebufferTexturePair(const FramebufferTexturePair&) = delete;
  FramebufferTexturePair& operator=(const FramebufferTexturePair&) = delete;
  FramebufferTexturePair(FramebufferTexturePair&& other) {
    if (this == &other) {
      return;
    }
    ASSERT(!m_moved_from && !other.m_moved_from);
    other.m_moved_from = true;
    m_w = other.m_w;
    m_h = other.m_h;
    m_texture = other.m_texture;
    m_framebuffers = std::move(other.m_framebuffers);
  }
  int width() const { return m_w; }
  int height() const { return m_h; }

 private:
  friend class FramebufferTexturePairContext;
  std::vector<GLuint> m_framebuffers;
  GLuint m_texture;
  int m_w, m_h;
  bool m_moved_from = false;
};

class FramebufferTexturePairContext {
 public:
  FramebufferTexturePairContext(FramebufferTexturePair& fb, int level = 0);
  ~FramebufferTexturePairContext();

  void switch_to(FramebufferTexturePair& fb);

  FramebufferTexturePairContext(const FramebufferTexturePairContext&) = delete;
  FramebufferTexturePairContext& operator=(const FramebufferTexturePairContext&) = delete;

 private:
  FramebufferTexturePair* m_fb;
  GLint m_old_viewport[4];
  GLint m_old_framebuffer;
};

// draw over the full screen.
// you must set alpha/ztest/etc.
class FullScreenDraw {
 public:
  FullScreenDraw();
  ~FullScreenDraw();
  FullScreenDraw(const FullScreenDraw&) = delete;
  FullScreenDraw& operator=(const FullScreenDraw&) = delete;
  void draw(const math::Vector4f& color, SharedRenderState* render_state, ScopedProfilerNode& prof);

 private:
  GLuint m_vao;
  GLuint m_vertex_buffer;
};

class FullScreenTexDraw {
 public:
  FullScreenTexDraw();
  ~FullScreenTexDraw();
  FullScreenTexDraw(const FullScreenTexDraw&) = delete;
  FullScreenTexDraw& operator=(const FullScreenTexDraw&) = delete;
  void draw(const math::Vector4f& color,
            const math::Vector2f& tex0,
            const math::Vector2f& tex1,
            SharedRenderState* render_state,
            ScopedProfilerNode& prof);

 private:
  GLuint m_vao;
  GLuint m_vertex_buffer;
};

class FramebufferCopier {
 public:
  FramebufferCopier();
  ~FramebufferCopier();
  FramebufferCopier(const FramebufferCopier&) = delete;
  FramebufferCopier& operator=(const FramebufferCopier&) = delete;
  void copy_now(int render_fb_w, int render_fb_h, GLuint render_fb);
  void copy_back_now(int render_fb_w, int render_fb_h, GLuint render_fb);
  u64 texture() const { return m_fbo_texture; }
  int width() const { return m_fbo_width; }
  int height() const { return m_fbo_height; }

 private:
  GLuint m_fbo = 0, m_fbo_texture = 0;
  int m_fbo_width = 640, m_fbo_height = 480;
};