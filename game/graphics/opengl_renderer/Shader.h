#pragma once

#include <string>

#include "common/common_types.h"
#include "common/versions/versions.h"

// Which GL implementation the shader source is being prepared for.
//
// The on-disk shaders are written in the common subset of desktop GLSL 4.10 core
// and OpenGL ES SL 3.00: they carry NO `#version` directive and no precision
// qualifiers. The dialect-specific prologue is injected at load time by
// Shader::Shader, so a single source tree serves both backends.
enum class ShaderBackend {
  AppleGL,  // desktop GL 4.10 core (the macOS system GL / oracle path)
  Angle,    // OpenGL ES 3.0 via ANGLE-Metal
};

// The backend the shader loader targets. This is a stub constant until the real
// runtime toggle lands; AppleGL keeps the existing path byte-for-byte.
constexpr ShaderBackend kShaderBackend = ShaderBackend::AppleGL;

// The dialect prologue Shader::Shader prepends as line 1 of every shader.
// Exposed so the dialect contract can be tested without a GL context.
std::string shader_prologue_for_test(ShaderBackend backend, bool is_fragment);

class Shader {
 public:
  static constexpr char shader_folder[] = "game/graphics/opengl_renderer/shaders/";
  Shader(const std::string& shader_name, GameVersion version);
  Shader() = default;
  void activate() const;
  bool okay() const { return m_is_okay; }
  u64 id() const { return m_program; }

 private:
  std::string m_name;
  u64 m_frag_shader = 0;
  u64 m_vert_shader = 0;
  u64 m_program = 0;
  bool m_is_okay = false;
};

// note: update the constructor in Shader.cpp
enum class ShaderId {
  SOLID_COLOR = 0,
  DIRECT_BASIC = 1,
  DIRECT_BASIC_TEXTURED = 2,
  DEBUG_RED = 3,
  SKY = 4,
  SKY_BLEND = 5,
  TFRAG3 = 6,
  TFRAG3_NO_TEX = 7,
  SPRITE = 8,
  SPRITE3 = 9,
  DIRECT2 = 10,
  EYE = 11,
  GENERIC = 12,
  OCEAN_TEXTURE = 13,
  OCEAN_TEXTURE_MIPMAP = 14,
  OCEAN_COMMON = 15,
  SHADOW = 16,
  SHRUB = 17,
  COLLISION = 18,
  MERC2 = 19,
  SPRITE_DISTORT = 20,
  SPRITE_DISTORT_INSTANCED = 21,
  POST_PROCESSING = 22,
  DEPTH_CUE = 23,
  EMERC = 24,
  GLOW_PROBE = 25,
  GLOW_PROBE_READ = 26,
  GLOW_PROBE_READ_DEBUG = 27,
  GLOW_PROBE_DOWNSAMPLE = 28,
  GLOW_DRAW = 29,
  ETIE_BASE = 30,
  ETIE = 31,
  SHADOW2 = 32,
  DIRECT_BASIC_TEXTURED_MULTI_UNIT = 33,
  TEX_ANIM = 34,
  GLOW_DEPTH_COPY = 35,
  GLOW_PROBE_ON_GRID = 36,
  HFRAG = 37,
  HFRAG_MONTAGE = 38,
  PLAIN_TEXTURE = 39,
  TIE_WIND = 40,
  SIMPLE_TEXTURE = 41,
  SLOW_TIME = 42,
  OCEAN_ENVMAP = 43,
  OCEAN_ENVMAP_HAZE = 44,
  MAX_SHADERS
};

class ShaderLibrary {
 public:
  ShaderLibrary(GameVersion version);
  Shader& operator[](ShaderId id) { return m_shaders[(int)id]; }
  Shader& at(ShaderId id) { return m_shaders[(int)id]; }

 private:
  Shader m_shaders[(int)ShaderId::MAX_SHADERS];
};
