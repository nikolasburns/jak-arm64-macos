/*!
 * Pins the GL entry-point contract for the ANGLE backend.
 *
 * The renderer is one tree driving two very different GL implementations: the
 * macOS AppleGL driver at desktop GL 4.1, and ANGLE's GLES 3.0 on Metal. A call
 * that exists only in desktop GL compiles, links, and runs perfectly on the
 * first, and does nothing at all on the second.
 *
 * That failure mode is quiet in a specific and expensive way. It is NOT the
 * null-pointer class -- ANGLE exports many desktop-GL names as extension entry
 * points, so the loader binds them and the call dispatches normally. ANGLE then
 * rejects it in a GLES 3.0 context, sets an error nobody reads, and does
 * nothing. The symptom surfaces later, somewhere else, naming neither the
 * function nor the reason.
 *
 * Each rule below cost a boot to find. The tests are cheap; the boots were not.
 */

#include <string>
#include <vector>

#include "common/util/FileUtil.h"

#include "gtest/gtest.h"

namespace {

// Strip // and /* */ so a banned name quoted in a comment -- including the
// explanatory comments these rules ask authors to write -- does not fail the
// scan. Without this, documenting the trap would trip the test for it.
std::string strip_comments(const std::string& in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size();) {
    if (in.compare(i, 2, "//") == 0) {
      while (i < in.size() && in[i] != '\n') {
        ++i;
      }
    } else if (in.compare(i, 2, "/*") == 0) {
      i += 2;
      while (i + 1 < in.size() && !(in[i] == '*' && in[i + 1] == '/')) {
        ++i;
      }
      i = std::min(i + 2, in.size());
    } else {
      out.push_back(in[i++]);
    }
  }
  return out;
}

struct Source {
  std::string name;
  std::string text;
};

std::vector<Source> graphics_sources() {
  std::vector<Source> out;
  for (const auto& p :
       fs::recursive_directory_iterator(file_util::get_file_path({"game/graphics"}))) {
    if (!p.is_regular_file()) {
      continue;
    }
    const auto ext = p.path().extension().string();
    if (ext != ".cpp" && ext != ".h" && ext != ".mm") {
      continue;
    }
    out.push_back({p.path().filename().string(), strip_comments(file_util::read_text_file(p))});
  }
  return out;
}

// Does `text` call `fn`, as a whole identifier followed by an open paren?
//
// The whole-identifier part is load-bearing: every banned name below is a strict
// prefix of its own legal replacement, so a plain substring search for
// "glFramebufferTexture" also matches "glFramebufferTexture2D" -- the very call
// the fix asks people to use, which would make the test unsatisfiable. Matching
// the paren is what distinguishes the two, since the longer name has its own
// characters before its paren.
bool calls(const std::string& text, const std::string& fn) {
  for (size_t i = text.find(fn); i != std::string::npos; i = text.find(fn, i + 1)) {
    // Skip a match that is the tail of some longer identifier (e.g. finding
    // "glFramebufferTexture2D" inside "myglFramebufferTexture2D").
    if (i > 0) {
      const char before = text[i - 1];
      if (isalnum((unsigned char)before) || before == '_') {
        continue;
      }
    }
    // Allow whitespace between the name and its open paren; require the paren,
    // so a longer identifier sharing this prefix does not match.
    size_t j = i + fn.size();
    while (j < text.size() && isspace((unsigned char)text[j])) {
      ++j;
    }
    if (j < text.size() && text[j] == '(') {
      return true;
    }
  }
  return false;
}

}  // namespace

// glFramebufferTexture (no "2D") is the *layered* attachment entry point, added
// in desktop GL 3.2 to bind a whole array/cubemap/3D texture at once for
// geometry shaders to index with gl_Layer. GLES 3.0 has no such call; it offers
// glFramebufferTexture2D and glFramebufferTextureLayer instead.
//
// On a plain 2D texture the layered form degenerates to the same result, which
// is exactly why the desktop path never noticed and every attachment site in
// this renderer used it. On ANGLE, all five attached nothing, and the first
// framebuffer built during boot failed its completeness check with
// GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT -- a message that names neither
// the call nor the dialect. Every attachment here is one mip level of one
// GL_TEXTURE_2D, so glFramebufferTexture2D is the correct call on both backends.
TEST(GlesEntryPoints, NoLayeredFramebufferAttachment) {
  const auto sources = graphics_sources();
  ASSERT_FALSE(sources.empty()) << "found no graphics sources to scan";

  for (const auto& src : sources) {
    EXPECT_FALSE(calls(src.text, "glFramebufferTexture"))
        << src.name
        << " calls glFramebufferTexture(), which is desktop GL 3.2+ only and"
           " silently attaches nothing under GLES 3.0 -- the framebuffer then"
           " fails completeness with MISSING_ATTACHMENT, far from this call."
           " Use framebuffer_attach_color_texture() in opengl_utils.h.";
  }
}

// glDrawBuffer/glReadBuffer (singular) are desktop-only; the plural
// glDrawBuffers is core GLES 3.0, and glReadBuffer(GLenum) does exist in GLES
// 3.0 -- so only the singular draw form is banned here.
TEST(GlesEntryPoints, NoSingularDrawBuffer) {
  const auto sources = graphics_sources();
  ASSERT_FALSE(sources.empty()) << "found no graphics sources to scan";

  for (const auto& src : sources) {
    EXPECT_FALSE(calls(src.text, "glDrawBuffer"))
        << src.name
        << " calls glDrawBuffer(), which does not exist in GLES 3.0."
           " Use glDrawBuffers(1, ...), which is core in both dialects.";
  }
}

// glMultiDrawElements (and glMultiDrawArrays) are desktop GL 1.4+ and do NOT
// exist in GLES 3.0. ANGLE exports only the suffixed forms
// (glMultiDrawElementsEXT / ...ANGLE / ...BaseVertexEXT) and never the bare
// name, so glad resolves the unsuffixed symbol against AppleGL -- which then
// faults inside libGL.dylib with an EGL context current, exactly like the
// gladLoadGL and imgui-loader defects before it.
//
// The suffixed forms are NOT the fix, and that was measured rather than
// reasoned. On the ANGLE-Metal context this build actually creates,
// glMultiDrawElementsANGLE *resolves* but the context advertises neither
// GL_ANGLE_multi_draw nor GL_EXT_multi_draw_arrays -- zero of its 117
// extensions contain "multi_draw" -- and calling it returns
// GL_INVALID_OPERATION having drawn nothing. So the renderer takes its
// pre-existing single-draw path on ANGLE instead (SharedRenderState::
// no_multidraw defaults from the backend), and the one remaining desktop call
// lives behind multi_draw_elements() in opengl_utils.h.
TEST(GlesEntryPoints, NoUnsuffixedMultiDraw) {
  const auto sources = graphics_sources();
  ASSERT_FALSE(sources.empty()) << "found no graphics sources to scan";

  for (const auto& src : sources) {
    EXPECT_FALSE(calls(src.text, "glMultiDrawElements"))
        << src.name
        << " calls glMultiDrawElements(), which does not exist in GLES 3.0."
           " ANGLE exports only glMultiDrawElementsEXT/ANGLE. Route it through"
           " an extension entry point, and verify the suffixed form is usable"
           " in a GLES 3.0 context rather than merely exported.";
    EXPECT_FALSE(calls(src.text, "glMultiDrawArrays"))
        << src.name << " calls glMultiDrawArrays(), which does not exist in GLES 3.0.";
  }
}

// Guards the scanner itself. If a refactor moves the graphics tree or changes
// the extensions, the rules above must not pass by finding nothing to check --
// a green test over an empty corpus is the failure mode these exist to prevent.
TEST(GlesEntryPoints, ScannerSeesTheAttachmentSites) {
  const auto sources = graphics_sources();
  ASSERT_FALSE(sources.empty()) << "found no graphics sources to scan";

  int attachment_sites = 0;
  for (const auto& src : sources) {
    if (calls(src.text, "glFramebufferTexture2D")) {
      ++attachment_sites;
    }
  }
  EXPECT_GE(attachment_sites, 2) << "expected the shared helper plus at least one direct caller;"
                                    " the scan is probably looking at the wrong tree";

  // Same guard for the multidraw rule. The batched draws are real and must stay
  // routed through the helper -- if these vanish, NoUnsuffixedMultiDraw above is
  // passing over nothing rather than over correct code.
  int multidraw_sites = 0;
  for (const auto& src : sources) {
    if (calls(src.text, "multi_draw_elements")) {
      ++multidraw_sites;
    }
  }
  EXPECT_GE(multidraw_sites, 2) << "expected the shared helper plus the background renderers"
                                   " (Shrub/Tie3/TFragment) to call multi_draw_elements();"
                                   " the scan is probably looking at the wrong tree";
}


// glVertexAttribIPointer's type argument is a CONTRACT with the shader's
// declared attribute type on Metal, not a conversion request.
//
// Desktop GL silently widens: feeding GL_UNSIGNED_SHORT data to an attribute
// declared `in int` works, and did for years. Metal requires the declared type
// and the buffer format to agree, so ANGLE cannot build the pipeline at all:
//
//   Vertex attribute _utime_of_day_index(3) of type int cannot be read using
//   MTLAttributeFormatUShort
//
// The failure is quiet in the worst way. CreateRenderPipelineState fails and
// logs a GL error nobody reads; with no pipeline object, setRenderPipelineState
// is never called, and the draw is encoded anyway. A debug ANGLE catches that
// with its own ASSERT; the release dylib we ship has ASSERTs compiled out, so
// the pipeline-less draw reaches Metal and dereferences null inside the Apple
// driver -- a corpse naming neither the shader, the attribute, nor the dialect.
// It cost four sessions and one withdrawn upstream bug report.
//
// The rule is agreement, not "always unsigned": an attribute fed unsigned data
// must be declared uint/uvecN, and one fed GL_INT must stay int/ivecN. Cast at
// the use site -- ESSL 3.00 has no implicit int/uint conversion, so the cast is
// explicit and the shader compiler checks it.
//
// Signed declarations are therefore allowlisted rather than banned outright.
// Each entry below was checked against its glVertexAttribIPointer call and is
// fed GL_INT; adding to this list means having done that check.
TEST(GlesEntryPoints, IntegerVertexAttributesAreDeclaredWithMatchingSignedness) {
  // shader file -> the call site proving its integer attributes are signed.
  const std::vector<std::pair<std::string, std::string>> kSignedByDesign = {
      {"slow_time.vert", "SlowTimeEffect.cpp:47 glVertexAttribIPointer(0, 1, GL_INT, ...)"},
      {"simple_texture.vert", "opengl_utils.cpp:231 glVertexAttribIPointer(0, 1, GL_INT, ...)"},
      {"tex_anim.vert", "TextureAnimator.cpp:529 glVertexAttribIPointer(0, 1, GL_INT, ...)"},
  };

  const auto shader_dir = file_util::get_file_path({"game/graphics/opengl_renderer/shaders"});
  ASSERT_TRUE(fs::exists(shader_dir)) << "shader tree not found at " << shader_dir;

  int signed_seen = 0;
  int unsigned_decls = 0;
  for (const auto& p : fs::directory_iterator(shader_dir)) {
    if (!p.is_regular_file() || p.path().extension().string() != ".vert") {
      continue;
    }
    const auto name = p.path().filename().string();
    const auto text = strip_comments(file_util::read_text_file(p));

    bool allowlisted = false;
    for (const auto& entry : kSignedByDesign) {
      if (entry.first == name) {
        allowlisted = true;
      }
    }

    for (const char* t : {" in uint ", " in uvec2 ", " in uvec3 ", " in uvec4 "}) {
      for (size_t i = text.find(t); i != std::string::npos; i = text.find(t, i + 1)) {
        ++unsigned_decls;
      }
    }

    for (size_t i = text.find("layout"); i != std::string::npos; i = text.find("layout", i + 1)) {
      const size_t eol = text.find('\n', i);
      const std::string line = text.substr(i, eol == std::string::npos ? eol : eol - i);
      if (line.find(" in ") == std::string::npos) {
        continue;
      }
      for (const char* signed_type : {" in int ", " in ivec2 ", " in ivec3 ", " in ivec4 "}) {
        if (line.find(signed_type) == std::string::npos) {
          continue;
        }
        ++signed_seen;
        EXPECT_TRUE(allowlisted)
            << name << ": integer attribute declared signed, but is fed unsigned data.\n"
            << "  " << line << "\n"
            << "  Metal rejects the pipeline when the declared type and the"
               " glVertexAttribIPointer format disagree. Declare it uint/uvecN and cast at"
               " the use site -- or, if this attribute really is fed GL_INT, add it to"
               " kSignedByDesign with the call site that proves it.";
      }
    }
  }

  // Scanner self-guards. A green scan over an empty corpus is the failure mode
  // this class of test exists to prevent, so both halves must find real work:
  // the unsigned attributes the fix introduced, and the signed ones it kept.
  EXPECT_GE(unsigned_decls, 10)
      << "expected the background/sprite/merc shaders to declare unsigned integer attributes;"
         " the scan is probably looking at the wrong tree";
  EXPECT_EQ(signed_seen, (int)kSignedByDesign.size())
      << "expected to see exactly the allowlisted signed attributes; if a shader was added or"
         " removed, update kSignedByDesign after checking its call site";
}
