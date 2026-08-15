/*!
 * @file opengl.cpp
 * Lower-level OpenGL interface. No actual rendering is performed here!
 */

#include "opengl.h"

#include <condition_variable>
#include <memory>
#include <mutex>
#include <sstream>

#include "common/dma/dma_copy.h"
#include "common/global_profiler/GlobalProfiler.h"
#include "common/goal_constants.h"
#include "common/log/log.h"
#include "common/util/FileUtil.h"
#include "common/util/FrameLimiter.h"
#include "common/util/Timer.h"
#include "common/util/compress.h"

#include "game/graphics/display.h"
#include "game/graphics/gfx.h"
#include "game/graphics/opengl_renderer/OpenGLRenderer.h"
#include "game/graphics/opengl_renderer/Shader.h"
#include "game/graphics/opengl_renderer/debug_gui.h"
#include "game/graphics/screenshot.h"
#include "game/graphics/texture/TexturePool.h"
#include "game/runtime.h"
#include "game/sce/libscf.h"
#include "game/system/hid/input_manager.h"
#include "game/system/hid/sdl_util.h"

#include "fmt/format.h"
#include "third-party/SDL/include/SDL3/SDL.h"
#include "third-party/SDL/include/SDL3/SDL_hints.h"
#include "third-party/SDL/include/SDL3/SDL_version.h"
#include "third-party/imgui/imgui.h"
#include "third-party/imgui/imgui_freetype.h"
#include "third-party/imgui/imgui_impl_opengl3.h"
#include "third-party/imgui/imgui_impl_sdl3.h"
#include "third-party/imgui/imgui_style.h"
#define STBI_WINDOWS_UTF8
#include "common/util/dialogs.h"
#include "common/util/string_util.h"

#include "third-party/stb_image/stb_image.h"

constexpr bool run_dma_copy = false;

constexpr PerGameVersion<int> fr3_level_count(jak1::LEVEL_TOTAL,
                                              jak2::LEVEL_TOTAL,
                                              jak3::LEVEL_TOTAL,
                                              jakx::LEVEL_TOTAL);

struct GraphicsData {
  // vsync
  std::mutex sync_mutex;
  std::condition_variable sync_cv;

  // dma chain transfer
  std::mutex dma_mutex;
  std::condition_variable dma_cv;
  u64 frame_idx = 0;
  u64 frame_idx_of_input_data = 0;
  bool has_data_to_render = false;
  FixedChunkDmaCopier dma_copier;

  // texture pool
  std::shared_ptr<TexturePool> texture_pool;

  std::shared_ptr<Loader> loader;

  // temporary opengl renderer
  OpenGLRenderer ogl_renderer;

  OpenGlDebugGui debug_gui;

  FrameLimiter frame_limiter;
  Timer engine_timer;
  double last_engine_time = 1. / 60.;
  float pmode_alp = 1.f;

  std::string imgui_log_filename, imgui_filename;
  GameVersion version;

  GraphicsData(GameVersion version)
      : dma_copier(EE_MAIN_MEM_SIZE),
        texture_pool(std::make_shared<TexturePool>(version)),
        loader(std::make_shared<Loader>(
            file_util::get_game_output_dir(version) / "fr3",
            fr3_level_count[version])),
        ogl_renderer(texture_pool, loader, version),
        debug_gui(),
        version(version) {}
};

std::unique_ptr<GraphicsData> g_gfx_data;

// Point SDL at the ANGLE libEGL/libGLESv2 to dlopen for the GLES path.
//
// NOTHING IS LINKED AGAINST ANGLE. SDL loads both libraries by name at runtime
// (third-party/SDL/src/video/SDL_egl.c: DEFAULT_EGL / DEFAULT_OGL_ES2, each
// overridable by hint), so an AppleGL-only build neither references nor requires
// any ANGLE artifact -- absent them, only the ANGLE path fails, and it fails at
// context creation with SDL's own error rather than at load time.
//
// ANGLE build pinned to commit aa192212af54a9de42a63db84a292b4cbfcaf114
// ("IR: Validate matrix packing decorations", 2026-08-14). The build recipe --
// args.gn plus the two environment blockers (DEVELOPER_DIR, MetalToolchain) --
// is recorded in PROGRESS.md, session 28. libEGL.dylib has install name
// "./libEGL.dylib", so an ABSOLUTE path is required here; a bare name would only
// resolve relative to the working directory.
static void set_angle_library_hints() {
  // GK_ANGLE_DIR overrides the directory holding libEGL.dylib + libGLESv2.dylib.
  const char* dir = std::getenv("GK_ANGLE_DIR");
  if (!dir) {
    lg::warn(
        "gfx backend ANGLE: GK_ANGLE_DIR is not set; falling back to SDL's default library "
        "search. Set it to the directory containing libEGL.dylib and libGLESv2.dylib.");
    return;
  }
  const std::string egl = std::string(dir) + "/libEGL.dylib";
  const std::string gles = std::string(dir) + "/libGLESv2.dylib";
  if (!fs::exists(egl) || !fs::exists(gles)) {
    lg::error("gfx backend ANGLE: GK_ANGLE_DIR=\"{}\" lacks libEGL.dylib and/or libGLESv2.dylib",
              dir);
    return;
  }
  SDL_SetHint(SDL_HINT_EGL_LIBRARY, egl.c_str());
  SDL_SetHint(SDL_HINT_OPENGL_LIBRARY, gles.c_str());
  lg::info("gfx backend ANGLE: EGL={} GLESv2={}", egl, gles);

  // Select ANGLE's Metal backend.
  //
  // THIS IS LOAD-BEARING, and its absence FAILS QUIETLY. SDL creates the EGL
  // display without an EGL_PLATFORM_ANGLE_TYPE_ANGLE attribute, so ANGLE picks
  // its own default -- which on this build is the *GL* backend, i.e. GLES
  // emulated on the very Apple GL driver ANGLE exists to replace. That still
  // produces a working GLES 3.0 context and a plausible log line
  // ("ANGLE (Apple, Apple M4 Pro, OpenGL 4.1 Metal - 90.5)"), so it reads as
  // success while not being the backend we asked for. With this set the
  // renderer string instead reads "ANGLE Metal Renderer: Apple M4 Pro".
  //
  // Respect an explicit override so the GL backend stays reachable for A/B
  // attribution (ANGLE_DEFAULT_PLATFORM=gl), which is worth having when
  // deciding whether a defect belongs to ANGLE's frontend or to Metal.
  if (!std::getenv("ANGLE_DEFAULT_PLATFORM")) {
    setenv("ANGLE_DEFAULT_PLATFORM", "metal", 0);
    lg::info("gfx backend ANGLE: ANGLE_DEFAULT_PLATFORM=metal");
  } else {
    lg::info("gfx backend ANGLE: ANGLE_DEFAULT_PLATFORM={} (from environment)",
             std::getenv("ANGLE_DEFAULT_PLATFORM"));
  }
}

static bool gl_inited = false;
static int gl_init(GfxGlobalSettings& settings) {
  prof().instant_event("ROOT");
  Timer gl_init_timer;

  // Select the graphics backend before anything reads it. Must happen before the
  // GL attributes are set (they differ per backend) and before any shader is
  // compiled, since the dialect prologue is chosen from this.
  gfx_backend_init();

  // Initialize SDL
  {
    auto p = scoped_prof("startup::sdl::init_sdl");
    // remove SDL garbage from hooking signal handler.
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
      sdl_util::log_error("Could not initialize SDL, exiting");
      dialogs::create_error_message_dialog("Critical Error Encountered",
                                           "Could not initialize SDL, exiting");
      return 1;
    }
  }

  {
    auto p = scoped_prof("startup::sdl::get_version_info");

    auto compiled_sdl_version = SDL_VERSION;
    auto linked_sdl_version = SDL_GetVersion();
    lg::info("SDL Initialized, compiled with version - {} | linked with version - {}",
             compiled_sdl_version, linked_sdl_version);
  }

  {
    auto p = scoped_prof("startup::sdl::set_gl_attributes");

    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    if (settings.debug) {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_DEBUG_FLAG);
    } else {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
    }

    if (gfx_backend_is_angle()) {
      // ANGLE: an OpenGL ES 3.0 context, reached through SDL's own EGL path.
      //
      // We do not create the EGL display/surface ourselves. SDL's Cocoa GLES
      // backend (src/video/cocoa/SDL_cocoaopengles.m) already does all of it,
      // and it selects that path off the profile mask alone: with
      // SDL_GL_CONTEXT_PROFILE_ES it uses EGL, and with anything else it
      // switches itself back to the CGL entry points. So this attribute IS the
      // backend switch -- the AppleGL path below is reached unchanged.
      //
      // The window surface comes from the NSView's CALayer (SDL_cocoaopengles.m
      // :139), which ANGLE-Metal accepts directly (SurfaceMtl.mm:451-473).
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
      set_angle_library_hints();
    } else {
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
#ifndef __APPLE__
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
#else
      SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
#endif
    }

    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
  }
  lg::info("gl init took {:.3f}s\n", gl_init_timer.getSeconds());
  return 0;
}

static void gl_exit() {
  g_gfx_data.reset();
  gl_inited = false;
}

// imgui ships its OWN GL loader (imgui_impl_opengl3_loader.h, a stripped gl3w),
// entirely separate from glad. On __APPLE__ its open_libgl() is unconditional:
// it dlopen()s /System/Library/Frameworks/OpenGL.framework and dlsym()s every
// entry point straight out of AppleGL, with no EGL/GLES awareness at all. On the
// ANGLE path that binds imgui to the wrong GL implementation -- AppleGL's
// glGetIntegerv then runs with an EGL context current and no CGL context to
// dereference, and faults on a null context pointer inside libGL.dylib before it
// can return.
//
// This is the same defect class as gladLoadGL() (session 3.5): a second loader
// resolving against the system framework while the real context belongs to
// somebody else. It is declared here rather than by including the loader header,
// which is an implementation header -- it #defines __gl_h_ and redeclares the GL
// typedefs, so pulling it in beside glad would collide. imgl3wInit2 is the
// loader's documented public entry point for exactly this override.
extern "C" {
typedef void (*GL3WglProc)(void);
typedef GL3WglProc (*GL3WGetProcAddressProc)(const char* proc);
int imgl3wInit2(GL3WGetProcAddressProc proc);
}

static void init_imgui(SDL_Window* window,
                       SDL_GLContext gl_context,
                       const std::string& glsl_version) {
  // check that version of the library is okay
  IMGUI_CHECKVERSION();

  // this does initialization for stuff like the font data
  ImGui::CreateContext();

  // Init ImGui settings
  g_gfx_data->imgui_filename = file_util::get_file_path({"imgui.ini"});
  g_gfx_data->imgui_log_filename = file_util::get_file_path({"imgui_log.txt"});
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
  io.IniFilename = g_gfx_data->imgui_filename.c_str();
  io.LogFilename = g_gfx_data->imgui_log_filename.c_str();

  if (Gfx::g_debug_settings.alternate_style) {
    ImGui::applyAlternateStyle();
  }

  ImGui::applyFontStyle();

  // set up to get inputs for this window
  ImGui_ImplSDL3_InitForOpenGL(window, gl_context);

  // NOTE: imgui's setup calls functions that may fail intentionally, and attempts to disable error
  // reporting so these errors are invisible. But it does not work, and some weird X11 default
  // cursor error is set here that we clear.
  SDL_ClearError();

  // Load imgui's GL entry points through SDL, which resolves them against
  // whichever GL the current context actually belongs to.
  //
  // This runs on BOTH backends, and is safe on the desktop path by the same
  // argument as the glad fix: there the context IS AppleGL's, so SDL hands back
  // pointers into the very library imgui's own loader would have dlopen()ed.
  // Keeping one path avoids a desktop branch that nothing would exercise.
  //
  // Order is load-bearing. ImGui_ImplOpenGL3_Init calls the backend's own
  // ImGui_ImplOpenGL3_InitLoader, which is lazy -- it only runs imgl3wInit() if
  // glGetIntegerv is still null. Populating the table first means the AppleGL
  // dlopen never happens. Doing this AFTER init would be too late.
  if (imgl3wInit2((GL3WGetProcAddressProc)SDL_GL_GetProcAddress) != 0) {
    lg::error("imgui GL loader init failed");
    dialogs::create_error_message_dialog("Critical Error Encountered",
                                         "Unable to initialize the ImGui OpenGL loader.");
    return;
  }

  // set up the renderer
  if (!ImGui_ImplOpenGL3_Init(glsl_version.c_str())) {
    lg::error("ImGui_ImplOpenGL3_Init failed (glsl version \"{}\")", glsl_version);
  }
}

static std::shared_ptr<GfxDisplay> gl_make_display(int width,
                                                   int height,
                                                   const char* title,
                                                   GfxGlobalSettings& /*settings*/,
                                                   GameVersion game_version,
                                                   bool is_main) {
  // Setup the window
  prof().instant_event("ROOT");
  prof().begin_event("startup::sdl::create_window");
  SDL_Window* window =
      SDL_CreateWindow(title, width, height,
                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY);
  prof().end_event();
  if (!window) {
    sdl_util::log_error("gl_make_display failed - Could not create display window");
    dialogs::create_error_message_dialog(
        "Critical Error Encountered",
        "Unable to create OpenGL window.\nOpenGOAL requires OpenGL 4.3.\nEnsure your GPU "
        "supports this and your drivers are up to date.");
    return NULL;
  }

  // Make an OpenGL Context
  prof().begin_event("startup::sdl::create_context");
  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  prof().end_event();
  if (!gl_context) {
    sdl_util::log_error("gl_make_display failed - Could not create OpenGL Context");
    dialogs::create_error_message_dialog(
        "Critical Error Encountered",
        "Unable to create OpenGL context.\nOpenGOAL requires OpenGL 4.3.\nEnsure your GPU "
        "supports this and your drivers are up to date.");
    return NULL;
  }

  {
    auto p = scoped_prof("startup::sdl::assign_context");
    if (!SDL_GL_MakeCurrent(window, gl_context)) {
      sdl_util::log_error("gl_make_display failed - Could not associated context with window");
      dialogs::create_error_message_dialog("Critical Error Encountered",
                                           "Unable to create OpenGL window with context.\nOpenGOAL "
                                           "requires OpenGL 4.3.\nEnsure your GPU "
                                           "supports this and your drivers are up to date.");
      return NULL;
    }
  }

  if (!gl_inited) {
    {
      auto p = scoped_prof("startup::sdl::glad_init");
      // Load the entry points through SDL, which resolves them against whichever
      // GL the context actually belongs to: AppleGL on the desktop path, ANGLE's
      // libGLESv2 on the ANGLE path.
      //
      // The GLES version-number mapping that makes this loader cover GL 3.1-3.3
      // lives in find_coreGL (third-party/glad/src/glad.c).
      bool glad_ok = gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress) != 0;
      if (!gfx_backend_is_angle()) {
        // Desktop path unchanged. gladLoadGL() must NOT run on the ANGLE path:
        // its no-argument form dlopen()s OpenGL.framework itself (glad.c:89) and
        // re-binds every pointer by dlsym, discarding what the loader above
        // installed. Its third statement is then glGetString(GL_VERSION) -- now
        // AppleGL's, called with an EGL context current and no CGL context at
        // all, which segfaults inside libGL.dylib before it can return.
        glad_ok = gladLoadGL();
      }
      if (!glad_ok) {
        lg::error("GL init fail");
        dialogs::create_error_message_dialog("Critical Error Encountered",
                                             "Unable to initialize OpenGL API.\nOpenGOAL requires "
                                             "OpenGL 4.3.\nEnsure your GPU "
                                             "supports this and your drivers are up to date.");
        return NULL;
      }
    }
    if (gfx_backend_is_angle()) {
      // Flat-qualified varyings take their value from the PROVOKING vertex, and
      // the two APIs disagree on which that is: desktop GL defaults to the LAST
      // vertex of the primitive, GLES 3.0 is fixed to the FIRST. The shaders are
      // written for desktop GL, so on ANGLE the flat-shaded renderers would pick
      // up a different vertex's colour without this.
      //
      // GL_ANGLE_provoking_vertex exists for exactly this; resolve it
      // dynamically, since our glad loader is desktop-GL generated.
      using ProvokingVertexANGLE = void(APIENTRY*)(GLenum);
      auto provoking_vertex_angle =
          reinterpret_cast<ProvokingVertexANGLE>(SDL_GL_GetProcAddress("glProvokingVertexANGLE"));
      if (provoking_vertex_angle) {
        constexpr GLenum kFirstVertexConventionANGLE = 0x8E4D;  // GL_FIRST_VERTEX_CONVENTION_ANGLE
        provoking_vertex_angle(kFirstVertexConventionANGLE);
        lg::info("gfx backend ANGLE: provoking vertex set to FIRST_VERTEX_CONVENTION");
      } else {
        // Not fatal, but flat-shaded colours will be wrong. Named visual checks
        // for this: direct2, sprite_3d, sprite_distort, depth_cue.
        lg::warn(
            "gfx backend ANGLE: glProvokingVertexANGLE unavailable; flat-shaded colours may be "
            "taken from the wrong vertex");
      }
    }

    {
      auto p = scoped_prof("startup::sdl::gfx_data_init");
      g_gfx_data = std::make_unique<GraphicsData>(game_version);
    }
    gl_inited = true;
    const char* gl_version = (const char*)glGetString(GL_VERSION);
    const char* gl_renderer = (const char*)glGetString(GL_RENDERER);
    lg::info("OpenGL initialized - v{}.{} | Version: {} | Renderer: {}", GLVersion.major,
             GLVersion.minor, gl_version ? gl_version : "(null)",
             gl_renderer ? gl_renderer : "(null)");
  }

  {
    auto p = scoped_prof("startup::sdl::window_extras");
    float dpi = 1.0f;
    float display_scale = SDL_GetWindowDisplayScale(window);
    if (display_scale > 0.0) {
      dpi = display_scale * 96.0f;
    }

    // Setup Window Icon
    const auto image_path = file_util::get_jak_project_dir() / "game" / "assets" /
                            version_to_game_name(game_version) /
                            (dpi == 1.0f ? "app64.png" : "app256.png");
    if (fs::exists(image_path)) {
      int icon_width;
      int icon_height;

      auto icon_data = stbi_load(image_path.string().c_str(), &icon_width, &icon_height, nullptr,
                                 STBI_rgb_alpha);
      if (icon_data) {
        SDL_Surface* icon_surf = SDL_CreateSurfaceFrom(
            icon_width, icon_height, SDL_PIXELFORMAT_RGBA32, (void*)icon_data, 4 * icon_width);
        if (!icon_surf) {
          sdl_util::log_error("unable to generate surface from app icon data");
        } else {
          SDL_SetWindowIcon(window, icon_surf);
          SDL_DestroySurface(icon_surf);
        }
        stbi_image_free(icon_data);
      } else {
        lg::error("Could not load icon for OpenGL window, couldn't load image data");
      }
    } else {
      lg::error("Could not load icon for OpenGL window, {} does not exist", image_path.string());
    }
  }

  prof().begin_event("startup::sdl::create_GLDisplay");
  auto display = std::make_shared<GLDisplay>(window, gl_context, is_main);
  display->set_imgui_visible(Gfx::g_debug_settings.show_imgui);
  prof().end_event();

  {
    auto p = scoped_prof("startup::sdl::init_imgui");
    // setup imgui
    //
    // imgui compiles its own shaders from this prologue, so it needs the same
    // dialect the rest of the tree gets from shader_prologue() -- a desktop
    // "#version 410" against a GLES 3.0 context would compile-fail at imgui's
    // first frame rather than at init. imgui selects a dedicated 300 es shader
    // pair on an exact "#version 300" match (imgui_impl_opengl3.cpp), and it
    // detects GlProfileIsES3 itself from the GL_VERSION string ANGLE reports.
    if (gfx_backend_is_angle()) {
      init_imgui(window, gl_context, "#version 300 es");
    } else {
#ifdef __APPLE__
      init_imgui(window, gl_context, "#version 410");
#else
      init_imgui(window, gl_context, "#version 430");
#endif
    }
  }

  return std::static_pointer_cast<GfxDisplay>(display);
}

GLDisplay::GLDisplay(SDL_Window* window, SDL_GLContext gl_context, bool is_main)
    : m_window(window),
      m_gl_context(gl_context),
      m_display_manager(std::make_shared<DisplayManager>(window)),
      m_input_manager(std::make_shared<InputManager>(window)) {
  m_main = is_main;
  m_display_manager->set_input_manager(m_input_manager);
  // Register commands
  m_input_manager->register_command(
      CommandBinding::Source::KEYBOARD,
      CommandBinding(Gfx::g_debug_settings.hide_imgui_key, [&](const SDL_Event& event) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0) {
          if (!Gfx::g_debug_settings.ignore_hide_imgui) {
            set_imgui_visible(!is_imgui_visible());
          }
        }
      }));
  ;
  m_input_manager->register_command(
      CommandBinding::Source::KEYBOARD,
      CommandBinding(SDLK_F2, [&]() { m_take_screenshot_next_frame = true; }));

  const auto& bind = Gfx::g_debug_settings.toggle_fullscreen_key;

  m_input_manager->register_command(
      CommandBinding::Source::KEYBOARD,
      CommandBinding(bind.key, bind.modifiers, [&](const SDL_Event& event) {
        if (event.type == SDL_EVENT_KEY_DOWN && event.key.repeat == 0 &&
            bind.modifiers.has_necessary_modifiers(SDL_GetModState())) {
          m_display_manager->toggle_display_mode();
        }
      }));
}

void GLDisplay::init_splash() {
  if (m_splash_program)
    return;
  if (!Gfx::g_splash.ready.load())
    return;

  auto shader_folder = "game/graphics/opengl_renderer/shaders";
  auto vert_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, "splash.vert"}));
  auto frag_src =
      file_util::read_text_file(file_util::get_file_path({shader_folder, "splash.frag"}));

  // The on-disk shaders carry no `#version`; it is injected here, exactly as
  // ShaderLibrary does for the shaders it owns. Use the shared builder rather
  // than a local copy so the two sites cannot drift apart.
  vert_src = shader_prologue(gfx_shader_backend(), false) + vert_src;
  frag_src = shader_prologue(gfx_shader_backend(), true) + frag_src;

  constexpr int len = 1024;
  GLint compile_ok;
  char err[len];

  auto compile_shader = [](GLenum type, const char* src, GLint& compile_ok, char* err) -> GLuint {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    glGetShaderiv(s, GL_COMPILE_STATUS, &compile_ok);
    if (!compile_ok) {
      glGetShaderInfoLog(s, len, nullptr, err);
      // init_splash runs once per frame until it succeeds, so an unconditional
      // log here spams a line per frame for the whole load. Report once.
      static bool reported = false;
      if (!reported) {
        reported = true;
        lg::error("splash shader compile failed: {}", err);
      }
      glDeleteShader(s);
      return 0;
    }
    return s;
  };

  GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src.c_str(), compile_ok, err);
  GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src.c_str(), compile_ok, err);
  if (!vs || !fs) {
    glDeleteShader(vs);
    glDeleteShader(fs);
    return;
  }

  m_splash_program = glCreateProgram();
  glAttachShader(m_splash_program, vs);
  glAttachShader(m_splash_program, fs);
  glLinkProgram(m_splash_program);
  glDeleteShader(vs);
  glDeleteShader(fs);

  glGetProgramiv(m_splash_program, GL_LINK_STATUS, &compile_ok);
  if (!compile_ok) {
    glGetProgramInfoLog(m_splash_program, len, nullptr, err);
    // as above: this path retries every frame, so report the failure once.
    static bool link_reported = false;
    if (!link_reported) {
      link_reported = true;
      lg::error("Failed to link splash shader:\n{}", err);
    }
    glDeleteProgram(m_splash_program);
    m_splash_program = 0;
    return;
  }

  if (!Gfx::g_splash.data.empty()) {
    glGenTextures(1, &m_splash_texture);
    glBindTexture(GL_TEXTURE_2D, m_splash_texture);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, Gfx::g_splash.width, Gfx::g_splash.height, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, Gfx::g_splash.data.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    Gfx::g_splash.data.clear();
    Gfx::g_splash.data.shrink_to_fit();
  }

  glGenVertexArrays(1, &m_splash_vao);
}

void GLDisplay::draw_splash(int fb_w, int fb_h) {
  if (!m_splash_program || !m_splash_texture)
    return;
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
  glClearColor(0.f, 0.f, 0.f, 1.f);
  glClear(GL_COLOR_BUFFER_BIT);
  glViewport(0, 0, fb_w, fb_h);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glUseProgram(m_splash_program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_splash_texture);
  glUniform1i(glGetUniformLocation(m_splash_program, "splash_tex"), 0);
  glUniform2f(glGetUniformLocation(m_splash_program, "u_res"), fb_w, fb_h);
  glUniform2f(glGetUniformLocation(m_splash_program, "u_tex"), Gfx::g_splash.width,
              Gfx::g_splash.height);
  glBindVertexArray(m_splash_vao);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glBindVertexArray(0);
}

GLDisplay::~GLDisplay() {
  if (m_splash_texture)
    glDeleteTextures(1, &m_splash_texture);
  if (m_splash_program)
    glDeleteProgram(m_splash_program);
  if (m_splash_vao)
    glDeleteVertexArrays(1, &m_splash_vao);
  // Cleanup ImGUI
  ImGuiIO& io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  // Cleanup SDL
  SDL_GL_DestroyContext(m_gl_context);
  SDL_DestroyWindow(m_window);
  // cleanup SDL related sub-systems before we quit SDL
  if (m_display_manager) {
    m_display_manager.reset();
  }
  if (m_input_manager) {
    m_input_manager.reset();
  }
  // now quit SDL
  SDL_Quit();
  if (m_main) {
    gl_exit();
  }
}

void render_game_frame(int game_width,
                       int game_height,
                       int window_fb_width,
                       int window_fb_height,
                       int draw_region_width,
                       int draw_region_height,
                       int msaa_samples,
                       int brightness_contrast_color,
                       int brightness_contrast_alpha,
                       bool take_screenshot) {
  // wait for a copied chain.
  bool got_chain = false;
  {
    auto p = scoped_prof("wait-for-dma");
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    // there's a timeout here, so imgui can still be responsive even if we don't render anything
    got_chain = g_gfx_data->dma_cv.wait_for(lock, std::chrono::milliseconds(40),
                                            [=] { return g_gfx_data->has_data_to_render; });
  }
  // render that chain.
  if (got_chain) {
    g_gfx_data->frame_idx_of_input_data = g_gfx_data->frame_idx;
    RenderOptions options;
    options.game_res_w = game_width;
    options.game_res_h = game_height;
    options.window_framebuffer_width = window_fb_width;
    options.window_framebuffer_height = window_fb_height;
    options.draw_region_width = draw_region_width;
    options.draw_region_height = draw_region_height;
    options.msaa_samples = msaa_samples;
    options.brightness_contrast_color = brightness_contrast_color;
    options.brightness_contrast_alpha = brightness_contrast_alpha;
    options.draw_render_debug_window = g_gfx_data->debug_gui.should_draw_render_debug();
    options.draw_profiler_window = g_gfx_data->debug_gui.should_draw_profiler();
    options.draw_loader_window = g_gfx_data->debug_gui.should_draw_loader_menu();
    options.draw_subtitle_editor_window = g_gfx_data->debug_gui.should_draw_subtitle_editor();
    options.draw_filters_window = g_gfx_data->debug_gui.should_draw_filters_menu();
    options.save_screenshot = false;
    options.quick_screenshot = false;
    options.internal_res_screenshot = false;
    options.gpu_sync = g_gfx_data->debug_gui.should_gl_finish();

    if (take_screenshot) {
      options.save_screenshot = true;
      options.quick_screenshot = true;
      options.screenshot_path = file_util::make_screenshot_filepath(g_game_version);
    }
    // note : it's important we call get_screenshot_flag first because it modifies state
    if (g_gfx_data->debug_gui.get_screenshot_flag() || g_want_screenshot) {
      g_want_screenshot = false;
      options.save_screenshot = true;
      options.internal_res_screenshot = true;
      options.game_res_w = g_screen_shot_settings->width;
      options.game_res_h = g_screen_shot_settings->height;
      options.window_framebuffer_width = options.game_res_w;
      options.window_framebuffer_height = options.game_res_h;
      options.draw_region_width = options.game_res_w;
      options.draw_region_height = options.game_res_h;
      options.msaa_samples = g_screen_shot_settings->msaa;
      options.screenshot_path =
          file_util::make_screenshot_filepath(g_game_version, get_screen_shot_name());
    }

    options.draw_small_profiler_window =
        g_gfx_data->debug_gui.master_enable && g_gfx_data->debug_gui.small_profiler;
    options.pmode_alp_register = g_gfx_data->pmode_alp;

    GLint msaa_max;
    glGetIntegerv(GL_MAX_SAMPLES, &msaa_max);
    if (options.msaa_samples > msaa_max) {
      options.msaa_samples = msaa_max;
    }

    // MSAA is disabled on ANGLE: the resolve blit in do_pcrtc_effects faults inside
    // ANGLE's Metal backend (AGXMetalG16X drawIndexedPrimitives, offset-from-null),
    // and nothing renders at all when it does. Forcing 1 sample takes the
    // resolve_buffer branch out of the frame entirely and gives a correct, if
    // unantialiased, picture -- which is strictly better than no picture.
    //
    // Clamped here rather than at the settings source because this is the one point
    // every path funnels through, including the internal-res screenshot override
    // above, which sets msaa_samples independently of the global setting.
    //
    // The single-sampled path is well-exercised on GLES: it is a plain texture
    // attachment with no resolve step, and it is what jak2 shipped on during the
    // call-site audit. Revisit if the resolve is fixed; see the MSAA RESOLVE item
    // in the work log for the captured blit parameters.
    if (gfx_backend_is_angle()) {
      options.msaa_samples = 1;
    }

    if constexpr (run_dma_copy) {
      auto& chain = g_gfx_data->dma_copier.get_last_result();
      g_gfx_data->ogl_renderer.render(DmaFollower(chain.data.data(), chain.start_offset), options);
    } else {
      auto p = scoped_prof("ogl-render");
      g_gfx_data->ogl_renderer.render(DmaFollower(g_gfx_data->dma_copier.get_last_input_data(),
                                                  g_gfx_data->dma_copier.get_last_input_offset()),
                                      options);
    }
  }

  // before vsync, mark the chain as rendered.
  {
    // should be fine to remove this mutex if the game actually waits for vsync to call
    // send_chain again. but let's be safe for now.
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    g_gfx_data->engine_timer.start();
    g_gfx_data->has_data_to_render = false;
    g_gfx_data->sync_cv.notify_all();
  }
}

void GLDisplay::process_sdl_events() {
  SDL_Event evt;
  while (SDL_PollEvent(&evt) != 0) {
    if (evt.type == SDL_EVENT_QUIT) {
      m_should_quit = true;
    }
    {
      auto p = scoped_prof("sdl-display-manager");
      m_display_manager->process_sdl_event(evt);
    }
    if (!m_should_quit) {
      {
        auto p = scoped_prof("imgui-sdl-process");
        ImGui_ImplSDL3_ProcessEvent(&evt);
      }
    }
    {
      auto p = scoped_prof("sdl-input-monitor-process-event");
      m_input_manager->process_sdl_event(evt);
    }
  }
}

/*!
 * Main function called to render graphics frames. This is called in a loop.
 */
void GLDisplay::render() {
  // Before we process the current frames SDL events we for keyboard/mouse button inputs.
  //
  // This technically means that keyboard/mouse button inputs will be a frame behind but the
  // event-based code is limiting (there aren't enough events to achieve a totally stateless
  // approach). Binding handling is still taken care of by the event code though.
  {
    auto p = scoped_prof("sdl-input-monitor-poll-for-kb-mouse");
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantCaptureKeyboard) {
      m_input_manager->clear_keyboard_actions();
    } else {
      m_input_manager->poll_keyboard_data();
    }
    if (io.WantCaptureMouse) {
      m_input_manager->clear_mouse_actions();
    } else {
      m_input_manager->poll_mouse_data();
    }
    m_input_manager->finish_polling();
  }
  // Now process SDL Events
  process_sdl_events();
  // Also process any display related events received from the EE (the game)
  // this is done here so they run from the perspective of the graphics thread
  {
    auto p = scoped_prof("display-manager-ee-events");
    m_display_manager->process_ee_events();
  }
  {
    auto p = scoped_prof("input-manager-ee-events");
    m_input_manager->process_ee_events();
  }

  // imgui start of frame
  {
    auto p = scoped_prof("imgui-new-frame");
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
  }

  // framebuffer size
  int fbuf_w, fbuf_h;
  SDL_GetWindowSizeInPixels(m_window, &fbuf_w, &fbuf_h);

  // render game!
  g_gfx_data->debug_gui.master_enable = is_imgui_visible();
  if (g_gfx_data->debug_gui.should_advance_frame()) {
    auto p = scoped_prof("game-render");
    int game_res_w = Gfx::g_global_settings.game_res_w;
    int game_res_h = Gfx::g_global_settings.game_res_h;
    if (game_res_w <= 0 || game_res_h <= 0) {
      // if the window size reports 0, the game will ask for a 0 sized window, and nothing likes
      // that.
      game_res_w = 640;
      game_res_h = 480;
    }
    // set the size of the visible/playable portion of the game in the window
    get_display_manager()->set_game_size(Gfx::g_global_settings.lbox_w,
                                         Gfx::g_global_settings.lbox_h);

    // draw splash screen on startup if requested
    if (SplashScreen && DiskBoot && !m_splash_program) {
      init_splash();
    }
    if (SplashScreen && Gfx::g_splash.ready.load() &&
        SplashTimer.getSeconds() < SPLASH_SCREEN_TIME) {
      draw_splash(fbuf_w, fbuf_h);
    }

    render_game_frame(
        game_res_w, game_res_h, fbuf_w, fbuf_h, Gfx::g_global_settings.lbox_w,
        Gfx::g_global_settings.lbox_h, Gfx::g_global_settings.msaa_samples,
        Gfx::g_global_settings.brightness_contrast_color,
        Gfx::g_global_settings.brightness_contrast_alpha,
        m_take_screenshot_next_frame && g_gfx_data->debug_gui.screenshot_hotkey_enabled);
    // If we took a screenshot, stop taking them now!
    if (m_take_screenshot_next_frame) {
      m_take_screenshot_next_frame = false;
    }
  }

  // render debug
  if (is_imgui_visible()) {
    auto p = scoped_prof("debug-gui");
    g_gfx_data->debug_gui.draw(g_gfx_data->dma_copier.get_last_result().stats);
  }
  {
    auto p = scoped_prof("imgui-render");
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  }

  // actual vsync
  g_gfx_data->debug_gui.finish_frame();
  if (Gfx::g_global_settings.framelimiter) {
    auto p = scoped_prof("frame-limiter");
    g_gfx_data->frame_limiter.run(
        Gfx::g_global_settings.target_fps, Gfx::g_global_settings.experimental_accurate_lag,
        Gfx::g_global_settings.sleep_in_frame_limiter, g_gfx_data->last_engine_time);
  }

  {
    auto p = scoped_prof("swap-buffers");
    SDL_GL_SwapWindow(m_window);
  }

  // actually wait for vsync
  if (g_gfx_data->debug_gui.should_gl_finish()) {
    glFinish();
  }

  // switch vsync modes, if requested
  if (Gfx::g_global_settings.vsync != Gfx::g_global_settings.old_vsync) {
    Gfx::g_global_settings.old_vsync = Gfx::g_global_settings.vsync;
    // NOTE - -1 can be used for adaptive vsync, maybe useful for Jak 2+?
    // https://wiki.libsdl.org/SDL3/SDL_GL_SetSwapInterval
    SDL_GL_SetSwapInterval(Gfx::g_global_settings.vsync);
  }

  // Start timing for the next frame.
  g_gfx_data->debug_gui.start_frame();
  prof().instant_event("ROOT");

  // toggle even odd and wake up engine waiting on vsync.
  // TODO: we could play with moving this earlier, right after the final bucket renderer.
  //       it breaks the VIF-interrupt profiling though.
  {
    prof().instant_event("engine-notify");
    std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
    g_gfx_data->frame_idx++;
    g_gfx_data->sync_cv.notify_all();
  }

  // reboot whole game, if requested
  if (g_gfx_data->debug_gui.want_reboot_in_debug) {
    g_gfx_data->debug_gui.want_reboot_in_debug = false;
    MasterExit = RuntimeExitStatus::RESTART_IN_DEBUG;
  }

  {
    auto p = scoped_prof("check-close-window");
    // exit if display window was closed
    if (m_should_quit) {
      std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
      MasterExit = RuntimeExitStatus::EXIT;
      g_gfx_data->sync_cv.notify_all();
    }
  }
}

/*!
 * Wait for the next vsync. Returns 0 or 1 depending on if frame is even or odd.
 * Called from the game thread, on a GOAL stack.
 */
u32 gl_vsync() {
  if (!g_gfx_data) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
  auto init_frame = g_gfx_data->frame_idx_of_input_data;
  g_gfx_data->sync_cv.wait(lock, [=] {
    return (MasterExit != RuntimeExitStatus::RUNNING) || g_gfx_data->frame_idx > init_frame;
  });
  return g_gfx_data->frame_idx & 1;
}

u32 gl_sync_path() {
  if (!g_gfx_data) {
    return 0;
  }
  std::unique_lock<std::mutex> lock(g_gfx_data->sync_mutex);
  g_gfx_data->last_engine_time = g_gfx_data->engine_timer.getSeconds();
  if (!g_gfx_data->has_data_to_render) {
    return 0;
  }
  g_gfx_data->sync_cv.wait(lock, [=] { return !g_gfx_data->has_data_to_render; });
  return 0;
}

/*!
 * Send DMA to the renderer.
 * Called from the game thread, on a GOAL stack.
 */
void gl_send_chain(const void* data, u32 offset) {
  if (g_gfx_data) {
    std::unique_lock<std::mutex> lock(g_gfx_data->dma_mutex);
    if (g_gfx_data->has_data_to_render) {
      lg::error(
          "Gfx::send_chain called when the graphics renderer has pending data. Was this called "
          "multiple times per frame?");
      return;
    }

    // we copy the dma data and give a copy of it to the render.
    // the copy has a few advantages:
    // - if the game code has a bug and corrupts the DMA buffer, the renderer won't see it.
    // - the copied DMA is much smaller than the entire game memory, so it can be dumped to a
    // file
    //    separate of the entire RAM.
    // - it verifies the DMA data is valid early on.
    // but it may also be pretty expensive. Both the renderer and the game wait on this to
    // complete.

    // The renderers should just operate on DMA chains, so eliminating this step in the future
    // may be easy.

    g_gfx_data->dma_copier.set_input_data(data, offset, run_dma_copy);

    g_gfx_data->has_data_to_render = true;
    g_gfx_data->dma_cv.notify_all();
  }
}

/*!
 * Upload texture outside of main DMA chain.
 * We trust the game to not remove textures that are currently being used, but if the game is messed
 * up, there is a possible race to updating this texture.
 */
void gl_texture_upload_now(const u8* tpage, int mode, u32 s7_ptr) {
  // block
  if (g_gfx_data) {
    // just pass it to the texture pool.
    // the texture pool will take care of locking.
    // we don't want to lock here for the entire duration of the conversion.
    g_gfx_data->texture_pool->handle_upload_now(tpage, mode, g_ee_main_mem, s7_ptr, false);
  }
}

/*!
 * Handle a local->local texture copy. The texture pool can just update texture pointers.
 * This is called from the main thread and the texture pool itself will handle locking.
 */
void gl_texture_relocate(u32 destination, u32 source, u32 format) {
  if (g_gfx_data) {
    g_gfx_data->texture_pool->relocate(destination, source, format);
  }
}

void gl_set_levels(const std::vector<std::string>& levels) {
  g_gfx_data->loader->set_want_levels(levels);
}

void gl_set_active_levels(const std::vector<std::string>& levels) {
  g_gfx_data->loader->set_active_levels(levels);
}

void gl_force_reload_all() {
  g_gfx_data->loader->request_reload_all();
}

void gl_force_reload_level(const std::string& name) {
  g_gfx_data->loader->request_reload_level(name);
}

void gl_force_reload_common() {
  g_gfx_data->loader->request_reload_common();
}

void gl_set_pmode_alp(float val) {
  g_gfx_data->pmode_alp = val;
}

const GfxRendererModule gRendererOpenGL = {
    gl_init,                 // init
    gl_make_display,         // make_display
    gl_exit,                 // exit
    gl_vsync,                // vsync
    gl_sync_path,            // sync_path
    gl_send_chain,           // send_chain
    gl_texture_upload_now,   // texture_upload_now
    gl_texture_relocate,     // texture_relocate
    gl_set_levels,           // set_levels
    gl_set_active_levels,    // set_active_levels
    gl_force_reload_all,     // force_reload_all
    gl_force_reload_level,   // force_reload_level
    gl_force_reload_common,  // force_reload_common
    gl_set_pmode_alp,        // set_pmode_alp
    GfxPipeline::OpenGL,     // pipeline
    "OpenGL 4.3"             // name
};
