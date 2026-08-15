/*!
 * Pins the shader-source dialect contract established for the ANGLE backend.
 *
 * The on-disk shaders are a single tree written in the common subset of desktop
 * GLSL 4.10 core and OpenGL ES SL 3.00. Shader.cpp injects the dialect-specific
 * `#version` prologue at load time, so one set of files serves both backends.
 *
 * Every rule below has a matching failure mode that compiles fine on the AppleGL
 * path and breaks only on ANGLE, i.e. exactly the kind of regression that would
 * otherwise be found by booting the game on a backend nobody tests locally.
 */

#include <algorithm>
#include <regex>
#include <string>
#include <vector>

#include "common/util/FileUtil.h"

#include "game/graphics/opengl_renderer/Shader.h"
#include "gtest/gtest.h"

namespace {

struct ShaderFile {
  std::string name;
  std::string text;
};

std::vector<ShaderFile> all_shaders() {
  std::vector<ShaderFile> out;
  const auto dir = file_util::get_file_path({Shader::shader_folder});
  for (const auto& p : fs::directory_iterator(dir)) {
    if (!p.is_regular_file()) {
      continue;
    }
    const auto ext = p.path().extension().string();
    if (ext != ".vert" && ext != ".frag") {
      continue;
    }
    out.push_back({p.path().filename().string(), file_util::read_text_file(p.path())});
  }
  std::sort(out.begin(), out.end(),
            [](const ShaderFile& a, const ShaderFile& b) { return a.name < b.name; });
  return out;
}

// Strip // and /* */ comments so a rule about code is not tripped by prose.
std::string strip_comments(const std::string& src) {
  std::string out;
  out.reserve(src.size());
  for (size_t i = 0; i < src.size(); ++i) {
    if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/') {
      while (i < src.size() && src[i] != '\n') {
        ++i;
      }
      out.push_back('\n');
    } else if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '*') {
      i += 2;
      while (i + 1 < src.size() && !(src[i] == '*' && src[i + 1] == '/')) {
        ++i;
      }
      ++i;
    } else {
      out.push_back(src[i]);
    }
  }
  return out;
}

}  // namespace

// There must actually be shaders to check: a glob that silently matches nothing
// would make every other test in this file vacuously pass.
TEST(ShaderDialect, ShaderSetIsNonEmpty) {
  const auto shaders = all_shaders();
  EXPECT_GE(shaders.size(), 90u) << "expected the full shader set; found " << shaders.size();
}

// The prologue is injected by Shader.cpp. A `#version` on disk means the loader
// emits two of them, which is an error in both dialects.
TEST(ShaderDialect, NoVersionDirectiveOnDisk) {
  for (const auto& s : all_shaders()) {
    EXPECT_EQ(s.text.find("#version"), std::string::npos)
        << s.name << " carries a #version directive; Shader.cpp injects it at load time";
  }
}

// ESSL 3.00 has no 1D sampler. The palette textures are uploaded as 1 x N 2D
// textures and read with texelFetch(t, ivec2(i, 0), 0).
TEST(ShaderDialect, NoSampler1D) {
  const std::regex re(R"(\bsampler1D\b)");
  for (const auto& s : all_shaders()) {
    EXPECT_FALSE(std::regex_search(strip_comments(s.text), re))
        << s.name << " uses sampler1D, which does not exist in GLSL ES 3.00";
  }
}

// `noperspective` is a desktop-only interpolation qualifier.
TEST(ShaderDialect, NoNoperspectiveQualifier) {
  const std::regex re(R"(\bnoperspective\b)");
  for (const auto& s : all_shaders()) {
    EXPECT_FALSE(std::regex_search(strip_comments(s.text), re))
        << s.name << " uses `noperspective`, which does not exist in GLSL ES 3.00";
  }
}

// Hex float literals (0x1p0) are GLSL 4.00+ only; ESSL 3.00 rejects them with
// "invalid number". Hex *integer* literals are fine and are not matched here.
TEST(ShaderDialect, NoHexFloatLiterals) {
  const std::regex re(R"(0[xX][0-9a-fA-F]*(\.[0-9a-fA-F]*)?[pP][+-]?[0-9]+)");
  for (const auto& s : all_shaders()) {
    EXPECT_FALSE(std::regex_search(strip_comments(s.text), re))
        << s.name << " uses a hex float literal, which GLSL ES 3.00 rejects";
  }
}

// The prologue itself: the two backends must get different, well-formed text,
// and the ES fragment stage must declare a default float precision (ESSL 3.00
// has none, unlike desktop GL).
TEST(ShaderDialect, PrologueMatchesBackend) {
  // AppleGL: desktop core profile, no precision statements.
  const auto gl_vert = shader_prologue(ShaderBackend::AppleGL, false);
  const auto gl_frag = shader_prologue(ShaderBackend::AppleGL, true);
  EXPECT_EQ(gl_vert.rfind("#version 410 core", 0), 0u);
  EXPECT_EQ(gl_frag.rfind("#version 410 core", 0), 0u);
  EXPECT_EQ(gl_frag.find("precision"), std::string::npos);

  // ANGLE: ES 3.00, with an explicit float precision in the fragment stage.
  const auto es_vert = shader_prologue(ShaderBackend::Angle, false);
  const auto es_frag = shader_prologue(ShaderBackend::Angle, true);
  EXPECT_EQ(es_vert.rfind("#version 300 es", 0), 0u);
  EXPECT_EQ(es_frag.rfind("#version 300 es", 0), 0u);
  EXPECT_NE(es_frag.find("precision highp float;"), std::string::npos);

  // In every case the directive must be the very first token of the shader.
  for (const auto& p : {gl_vert, gl_frag, es_vert, es_frag}) {
    EXPECT_EQ(p.rfind("#version", 0), 0u) << "prologue must begin with #version";
    EXPECT_EQ(p.back(), '\n') << "prologue must end in a newline";
  }
}

// The rules above check the shader FILES. This one checks the CALLERS, which is
// where the invariant actually lives.
//
// Session 1 verified every shader in ShaderLibrary and still shipped a break:
// the splash screen compiles its own shader outside the library, read the .vert
// straight off disk, and never got the injected prologue -- so it failed with
// "#version required and missing" once per frame for a whole load. Enumerating
// the registry is not enumerating the callers.
//
// So: find every glShaderSource call in the graphics tree and require that the
// text it hands to GL came through shader_prologue(). A new compile site that
// skips the prologue fails here instead of at someone's loading screen.
TEST(ShaderDialect, EveryCompileSiteInjectsThePrologue) {
  std::vector<fs::path> sources;
  for (const auto& dir : {"game/graphics"}) {
    for (const auto& p : fs::recursive_directory_iterator(file_util::get_file_path({dir}))) {
      if (!p.is_regular_file()) {
        continue;
      }
      const auto ext = p.path().extension().string();
      if (ext == ".cpp" || ext == ".mm") {
        sources.push_back(p.path());
      }
    }
  }
  ASSERT_FALSE(sources.empty()) << "found no graphics sources to scan";

  int compile_sites = 0;
  for (const auto& path : sources) {
    const auto text = strip_comments(file_util::read_text_file(path));
    if (text.find("glShaderSource") == std::string::npos) {
      continue;
    }
    ++compile_sites;
    // The file feeds source text to GL, so it must also build the prologue.
    EXPECT_NE(text.find("shader_prologue("), std::string::npos)
        << path.filename().string()
        << " calls glShaderSource but never calls shader_prologue(); on-disk"
           " shaders carry no #version, so this site will fail to compile."
           " Route it through shader_prologue() rather than adding a local copy.";
  }

  // Guard the scan itself: if a refactor moves the compile sites, this test must
  // not silently pass by finding nothing to check.
  EXPECT_GE(compile_sites, 2) << "expected at least the ShaderLibrary and splash compile sites";
}
