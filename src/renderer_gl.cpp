// Copyright 2014 Google Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "precompiled.h"  // NOLINT

#include "fplbase/internal/type_conversions_gl.h"
#include "fplbase/preprocessor.h"
#include "fplbase/render_target.h"
#include "fplbase/renderer.h"
#include "fplbase/texture.h"
#include "fplbase/utilities.h"

using mathfu::mat4;
using mathfu::vec2;
using mathfu::vec2i;
using mathfu::vec3;
using mathfu::vec4;

namespace fplbase {

TextureHandle InvalidTextureHandle() { return TextureHandleFromGl(0); }
TextureTarget InvalidTextureTarget() { return TextureTargetFromGl(0); }
ShaderHandle InvalidShaderHandle() { return ShaderHandleFromGl(0); }
UniformHandle InvalidUniformHandle() { return UniformHandleFromGl(-1); }
BufferHandle InvalidBufferHandle() { return BufferHandleFromGl(0); }
DeviceMemoryHandle InvalidDeviceMemoryHandle() { return DeviceMemoryHandle(); }

bool ValidTextureHandle(TextureHandle handle) {
  return GlTextureHandle(handle) != 0;
}
bool ValidTextureTarget(TextureTarget target) {
  return GlTextureTarget(target) != 0;
}
bool ValidShaderHandle(ShaderHandle handle) {
  return GlShaderHandle(handle) != 0;
}
bool ValidUniformHandle(UniformHandle handle) {
  return GlUniformHandle(handle) >= 0;
}
bool ValidBufferHandle(BufferHandle handle) {
  return GlBufferHandle(handle) != 0;
}
bool ValidDeviceMemoryHandle(DeviceMemoryHandle /*handle*/) { return false; }

RendererBaseImpl *RendererBase::CreateRendererBaseImpl() { return nullptr; }
void RendererBase::DestroyRendererBaseImpl(RendererBaseImpl *impl) {
  (void)impl;
}

RendererImpl *Renderer::CreateRendererImpl() { return nullptr; }
void Renderer::DestroyRendererImpl(RendererImpl *impl) { (void)impl; }

void RendererBase::AdvanceFrame(bool minimized, double time) {
  time_ = time;

  environment_.AdvanceFrame(minimized);
}

bool RendererBase::InitializeRenderingState() {
  auto exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));

  auto HasGLExt = [&exts](const char *ext) -> bool {
    // TODO(b/28761934): Consider supporting GL3.0 version.
    if (exts == nullptr) {
      return false;
    }
    auto pos = strstr(exts, ext);
    return pos && pos[strlen(ext)] <= ' ';  // Make sure it matched all.
  };

  // Check for ASTC: Available in devices supporting AEP.
  if (!HasGLExt("GL_KHR_texture_compression_astc_ldr")) {
    supports_texture_format_ &= ~(1 << kFormatASTC);
  }
  // Check for Non Power of 2 (NPOT) extension.
  if (HasGLExt("GL_ARB_texture_non_power_of_two") ||
      HasGLExt("GL_OES_texture_npot")) {
    supports_texture_npot_ = true;
  }

// Check for ETC2:
#ifdef PLATFORM_MOBILE
  if (environment_.feature_level() < kFeatureLevel30) {
#else
  if (!HasGLExt("GL_ARB_ES3_compatibility")) {
#endif
    supports_texture_format_ &= ~((1 << kFormatPKM) | (1 << kFormatKTX));
  }

#ifndef PLATFORM_MOBILE
  if (!HasGLExt("GL_ARB_vertex_buffer_object") ||
      !HasGLExt("GL_ARB_multitexture") || !HasGLExt("GL_ARB_vertex_program") ||
      !HasGLExt("GL_ARB_fragment_program")) {
    last_error_ = "missing GL extensions";
    return false;
  }
#endif

  GL_CALL(glGetIntegerv(GL_MAX_VERTEX_UNIFORM_COMPONENTS,
                        &max_vertex_uniform_components_));
#if defined(GL_MAX_VERTEX_UNIFORM_VECTORS)
  if (max_vertex_uniform_components_ == 0) {
    // If missing the number of uniform components, use the number of vectors.
    GL_CALL(glGetIntegerv(GL_MAX_VERTEX_UNIFORM_VECTORS,
                          &max_vertex_uniform_components_));
    max_vertex_uniform_components_ *= 4;
  }
#endif  // defined(GL_MAX_VERTEX_UNIFORM_VECTORS)

  return true;
}

void Renderer::ClearFrameBuffer(const vec4 &color) {
  GL_CALL(glClearColor(color.x, color.y, color.z, color.w));
  GL_CALL(glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT));
}

void Renderer::ClearDepthBuffer() { GL_CALL(glClear(GL_DEPTH_BUFFER_BIT)); }

ShaderHandle RendererBase::CompileShader(bool is_vertex_shader,
                                         ShaderHandle program,
                                         const char *csource) {
  assert(max_vertex_uniform_components_);

  const std::string max_components =
      "MAX_VERTEX_UNIFORM_COMPONENTS " +
      flatbuffers::NumToString(max_vertex_uniform_components_);
  const char *defines[] = {max_components.c_str(), nullptr};

  const char *source = csource;
  if (!is_vertex_shader && override_pixel_shader_.length())
    source = override_pixel_shader_.c_str();
  std::string platform_source;
  PlatformSanitizeShaderSource(source, defines, &platform_source);
  const char *platform_source_ptr = platform_source.c_str();

  const GLenum stage = is_vertex_shader ? GL_VERTEX_SHADER : GL_FRAGMENT_SHADER;
  const GLuint shader_obj = glCreateShader(stage);
  GL_CALL(glShaderSource(shader_obj, 1, &platform_source_ptr, nullptr));
  GL_CALL(glCompileShader(shader_obj));
  GLint success;
  GL_CALL(glGetShaderiv(shader_obj, GL_COMPILE_STATUS, &success));
  if (success) {
    GL_CALL(glAttachShader(GlShaderHandle(program), shader_obj));
    return ShaderHandleFromGl(shader_obj);
  } else {
    GLint length = 0;
    GL_CALL(glGetShaderiv(shader_obj, GL_INFO_LOG_LENGTH, &length));
    std::string shader_error(length + 1, '\0');
    GL_CALL(glGetShaderInfoLog(shader_obj, length, &length, &shader_error[0]));
    last_error_ = platform_source + "\n----------\n" + shader_error;
    GL_CALL(glDeleteShader(shader_obj));
    return InvalidShaderHandle();
  }
}

Shader *RendererBase::CompileAndLinkShaderHelper(const char *vs_source,
                                                 const char *ps_source,
                                                 Shader *shader) {
  auto program_gl = glCreateProgram();
  ShaderHandle program = ShaderHandleFromGl(program_gl);
  auto vs = CompileShader(true, program, vs_source);
  if (ValidShaderHandle(vs)) {
    auto ps = CompileShader(false, program, ps_source);
    if (ValidShaderHandle(ps)) {
      GL_CALL(glBindAttribLocation(program_gl, Mesh::kAttributePosition,
                                   "aPosition"));
      GL_CALL(
          glBindAttribLocation(program_gl, Mesh::kAttributeNormal, "aNormal"));
      GL_CALL(glBindAttribLocation(program_gl, Mesh::kAttributeTangent,
                                   "aTangent"));
      GL_CALL(glBindAttribLocation(program_gl, Mesh::kAttributeTexCoord,
                                   "aTexCoord"));
      GL_CALL(glBindAttribLocation(program_gl, Mesh::kAttributeTexCoordAlt,
                                   "aTexCoordAlt"));
      GL_CALL(
          glBindAttribLocation(program_gl, Mesh::kAttributeColor, "aColor"));
      GL_CALL(glBindAttribLocation(program_gl, Mesh::kAttributeBoneIndices,
                                   "aBoneIndices"));
      GL_CALL(glBindAttribLocation(program_gl, Mesh::kAttributeBoneWeights,
                                   "aBoneWeights"));
      GL_CALL(glLinkProgram(program_gl));
      GLint status;
      GL_CALL(glGetProgramiv(program_gl, GL_LINK_STATUS, &status));
      if (status == GL_TRUE) {
        if (shader == nullptr) {
          // Load a new shader.
          shader = new Shader(program, vs, ps);
        } else {
          // Reset the old shader with the recompiled shader.
          shader->Reset(program, vs, ps);
        }
        GL_CALL(glUseProgram(program_gl));
        shader->InitializeUniforms();
        return shader;
      }
      GLint length = 0;
      GL_CALL(glGetProgramiv(program_gl, GL_INFO_LOG_LENGTH, &length));
      last_error_.assign(length, '\0');
      GL_CALL(
          glGetProgramInfoLog(program_gl, length, &length, &last_error_[0]));
      GL_CALL(glDeleteShader(GlShaderHandle(ps)));
    }
    GL_CALL(glDeleteShader(GlShaderHandle(vs)));
  }
  GL_CALL(glDeleteProgram(program_gl));
  return nullptr;
}

void Renderer::SetDepthFunction(DepthFunction func) {
  if (func == depth_function_) {
    return;
  }

  DepthState depth_state = render_state_.depth_state;

  switch (func) {
    case kDepthFunctionDisabled:
      depth_state.enabled = false;
      break;

    case kDepthFunctionNever:
      depth_state.enabled = true;
      depth_state.function = kRenderNever;
      break;

    case kDepthFunctionAlways:
      depth_state.enabled = true;
      depth_state.function = kRenderAlways;
      break;

    case kDepthFunctionLess:
      depth_state.enabled = true;
      depth_state.function = kRenderLess;
      break;

    case kDepthFunctionLessEqual:
      depth_state.enabled = true;
      depth_state.function = kRenderLessEqual;
      break;

    case kDepthFunctionGreater:
      depth_state.enabled = true;
      depth_state.function = kRenderGreater;
      break;

    case kDepthFunctionGreaterEqual:
      depth_state.enabled = true;
      depth_state.function = kRenderGreaterEqual;
      break;

    case kDepthFunctionEqual:
      depth_state.enabled = true;
      depth_state.function = kRenderEqual;
      break;

    case kDepthFunctionNotEqual:
      depth_state.enabled = true;
      depth_state.function = kRenderNotEqual;
      break;

    case kDepthFunctionUnknown:
      // Do nothing.
      break;

    default:
      assert(false);  // Invalid depth function.
      break;
  }

  if (depth_state.enabled != render_state_.depth_state.enabled) {
    if (depth_state.enabled) {
      glEnable(GL_DEPTH_TEST);
    } else {
      glDisable(GL_DEPTH_TEST);
    }
  }

  if (depth_state.function != render_state_.depth_state.function) {
    const GLenum depth_func = RenderFunctionToGlFunction(depth_state.function);
    GL_CALL(glDepthFunc(depth_func));
  }

  depth_function_ = func;
  render_state_.depth_state = depth_state;
}

void Renderer::SetBlendMode(BlendMode blend_mode, float amount) {
  (void)amount;

  if (blend_mode == blend_mode_ &&
      !(blend_mode == kBlendModeTest &&
        amount != render_state_.alpha_test_state.ref)) {
    return;
  }

  AlphaTestState alpha_test_state = render_state_.alpha_test_state;
  BlendState blend_state = render_state_.blend_state;

  switch (blend_mode) {
    case kBlendModeOff:
      alpha_test_state.enabled = false;
      blend_state.enabled = false;
      break;

    case kBlendModeTest:
      alpha_test_state.enabled = true;
      alpha_test_state.function = kRenderGreater;
      alpha_test_state.ref = amount;
      blend_state.enabled = false;
      break;

    case kBlendModeAlpha:
      alpha_test_state.enabled = false;
      blend_state.enabled = true;
      blend_state.src_alpha = BlendState::kSrcAlpha;
      blend_state.src_color = BlendState::kSrcAlpha;
      blend_state.dst_alpha = BlendState::kOneMinusSrcAlpha;
      blend_state.dst_color = BlendState::kOneMinusSrcAlpha;
      break;

    case kBlendModeAdd:
      alpha_test_state.enabled = false;
      blend_state.enabled = true;
      blend_state.src_alpha = BlendState::kOne;
      blend_state.src_color = BlendState::kOne;
      blend_state.dst_alpha = BlendState::kOne;
      blend_state.dst_color = BlendState::kOne;
      break;

    case kBlendModeAddAlpha:
      alpha_test_state.enabled = false;
      blend_state.enabled = true;
      blend_state.src_alpha = BlendState::kSrcAlpha;
      blend_state.src_color = BlendState::kSrcAlpha;
      blend_state.dst_alpha = BlendState::kOne;
      blend_state.dst_color = BlendState::kOne;
      break;

    case kBlendModeMultiply:
      alpha_test_state.enabled = false;
      blend_state.enabled = true;
      blend_state.src_alpha = BlendState::kDstColor;
      blend_state.src_color = BlendState::kDstColor;
      blend_state.dst_alpha = BlendState::kZero;
      blend_state.dst_color = BlendState::kZero;
      break;

    case kBlendModePreMultipliedAlpha:
      alpha_test_state.enabled = false;
      blend_state.enabled = true;
      blend_state.src_alpha = BlendState::kOne;
      blend_state.src_color = BlendState::kOne;
      blend_state.dst_alpha = BlendState::kOneMinusSrcAlpha;
      blend_state.dst_color = BlendState::kOneMinusSrcAlpha;
      break;

    case kBlendModeUnknown:
      // Do nothing.
      break;

    default:
      assert(false);  // Not yet implemented.
      break;
  }

  if (blend_state.enabled != render_state_.blend_state.enabled) {
    if (blend_state.enabled) {
      GL_CALL(glEnable(GL_BLEND));
    } else {
      GL_CALL(glDisable(GL_BLEND));
    }
  }

  if (blend_state.src_alpha != render_state_.blend_state.src_alpha ||
      blend_state.src_color != render_state_.blend_state.src_color ||
      blend_state.dst_alpha != render_state_.blend_state.dst_alpha ||
      blend_state.dst_color != render_state_.blend_state.dst_color) {
    const GLenum src_factor = BlendStateFactorToGl(blend_state.src_alpha);
    const GLenum dst_factor = BlendStateFactorToGl(blend_state.dst_alpha);

    GL_CALL(glBlendFunc(src_factor, dst_factor));
  }

#ifndef PLATFORM_MOBILE  // Alpha test not supported in ES 2.
  if (alpha_test_state.enabled != render_state_.alpha_test_state.enabled) {
    if (alpha_test_state.enabled) {
      GL_CALL(glEnable(GL_ALPHA_TEST));
    } else {
      GL_CALL(glDisable(GL_ALPHA_TEST));
    }
  }

  if (alpha_test_state.ref != render_state_.alpha_test_state.ref ||
      alpha_test_state.function != render_state_.alpha_test_state.function) {
    const GLenum gl_func =
        RenderFunctionToGlFunction(alpha_test_state.function);
    GL_CALL(glAlphaFunc(gl_func, alpha_test_state.ref));
  }
#endif

  blend_mode_ = blend_mode;
  blend_amount_ = amount;
  render_state_.alpha_test_state = alpha_test_state;
  render_state_.blend_state = blend_state;
}

static void SetStencilOp(GLenum face, const StencilOperation &set_op,
                         const StencilOperation &current_op) {
  if (set_op == current_op) {
    return;
  }

  const GLenum sfail = StencilOpToGlOp(set_op.stencil_fail);
  const GLenum dpfail = StencilOpToGlOp(set_op.depth_fail);
  const GLenum dppass = StencilOpToGlOp(set_op.pass);
  GL_CALL(glStencilOpSeparate(face, sfail, dpfail, dppass));
}

static void SetStencilFunction(GLenum face, const StencilFunction &set_func,
                               const StencilFunction &current_func) {
  if (set_func == current_func) {
    return;
  }

  const GLenum gl_func = RenderFunctionToGlFunction(set_func.function);
  GL_CALL(glStencilFuncSeparate(face, gl_func, set_func.ref, set_func.mask));
}

void Renderer::SetStencilMode(StencilMode mode, int ref, uint32_t mask) {
  if (mode == stencil_mode_ && ref == stencil_ref_ && mask == stencil_mask_) {
    return;
  }

  StencilState stencil_state = render_state_.stencil_state;
  switch (mode) {
    case kStencilDisabled:
      stencil_state.enabled = false;
      break;

    case kStencilCompareEqual:
      stencil_state.enabled = true;

      stencil_state.front_function.function = kRenderEqual;
      stencil_state.front_function.ref = ref;
      stencil_state.front_function.mask = mask;
      stencil_state.back_function = stencil_state.front_function;

      stencil_state.front_op.stencil_fail = StencilOperation::kKeep;
      stencil_state.front_op.depth_fail = StencilOperation::kKeep;
      stencil_state.front_op.pass = StencilOperation::kKeep;
      stencil_state.back_op = stencil_state.front_op;
      break;

    case kStencilWrite:
      stencil_state.enabled = true;

      stencil_state.front_function.function = kRenderAlways;
      stencil_state.front_function.ref = ref;
      stencil_state.front_function.mask = mask;
      stencil_state.back_function = stencil_state.front_function;

      stencil_state.front_op.stencil_fail = StencilOperation::kKeep;
      stencil_state.front_op.depth_fail = StencilOperation::kKeep;
      stencil_state.front_op.pass = StencilOperation::kReplace;
      stencil_state.back_op = stencil_state.front_op;
      break;

    case kStencilUnknown:
      // Do nothing.
      break;

    default:
      assert(false);
  }

  if (stencil_state.enabled != render_state_.stencil_state.enabled) {
    if (stencil_state.enabled) {
      GL_CALL(glEnable(GL_STENCIL_TEST));
    } else {
      GL_CALL(glDisable(GL_STENCIL_TEST));
    }
  }

  SetStencilFunction(GL_BACK, stencil_state.back_function,
                     render_state_.stencil_state.back_function);
  SetStencilFunction(GL_FRONT, stencil_state.front_function,
                     render_state_.stencil_state.front_function);

  SetStencilOp(GL_FRONT, stencil_state.front_op,
               render_state_.stencil_state.front_op);
  SetStencilOp(GL_BACK, stencil_state.back_op,
               render_state_.stencil_state.back_op);

  render_state_.stencil_state = stencil_state;
  stencil_mode_ = mode;
  stencil_ref_ = ref;
  stencil_mask_ = mask;
}

void Renderer::SetCulling(CullingMode mode) {
  if (mode == cull_mode_) {
    return;
  }

  CullState cull_state = render_state_.cull_state;
  switch (mode) {
    case kCullingModeNone:
      cull_state.enabled = false;
      break;
    case kCullingModeBack:
      cull_state.enabled = true;
      cull_state.face = CullState::kBack;
      break;
    case kCullingModeFront:
      cull_state.enabled = true;
      cull_state.face = CullState::kFront;
      break;
    case kCullingModeFrontAndBack:
      cull_state.enabled = true;
      cull_state.face = CullState::kFrontAndBack;
      break;
    case kCullingModeUnknown:
      // Do nothing.
      break;
    default:
      // Unknown culling mode.
      assert(false);
  }

  if (cull_state.enabled != render_state_.cull_state.enabled) {
    if (cull_state.enabled) {
      GL_CALL(glEnable(GL_CULL_FACE));
    } else {
      GL_CALL(glDisable(GL_CULL_FACE));
    }
  }

  if (cull_state.face != render_state_.cull_state.face) {
    const GLenum cull_face = CullFaceToGl(cull_state.face);
    GL_CALL(glCullFace(cull_face));
  }

  cull_mode_ = mode;
  render_state_.cull_state = cull_state;
}

void Renderer::SetViewport(const Viewport &viewport) {
  if (viewport == render_state_.viewport) {
    return;
  }

  GL_CALL(glViewport(viewport.pos.x, viewport.pos.y, viewport.size.x,
                     viewport.size.y));
  render_state_.viewport = viewport;
}

void Renderer::ScissorOn(const vec2i &pos, const vec2i &size) {
  if (!render_state_.scissor_state.enabled) {
    GL_CALL(glEnable(GL_SCISSOR_TEST));
    render_state_.scissor_state.enabled = true;
  }

  auto viewport_size = base_->GetViewportSize();
  GL_CALL(glViewport(0, 0, viewport_size.x, viewport_size.y));

  auto scaling_ratio = vec2(viewport_size) / vec2(base_->window_size());
  auto scaled_pos = vec2(pos) * scaling_ratio;
  auto scaled_size = vec2(size) * scaling_ratio;
  GL_CALL(glScissor(static_cast<GLint>(scaled_pos.x),
                    static_cast<GLint>(scaled_pos.y),
                    static_cast<GLsizei>(scaled_size.x),
                    static_cast<GLsizei>(scaled_size.y)));
}

void Renderer::ScissorOff() {
  if (!render_state_.scissor_state.enabled) {
    return;
  }

  GL_CALL(glDisable(GL_SCISSOR_TEST));
  render_state_.scissor_state.enabled = false;
}

}  // namespace fplbase

#ifndef GL_INVALID_FRAMEBUFFER_OPERATION
#define GL_INVALID_FRAMEBUFFER_OPERATION GL_INVALID_FRAMEBUFFER_OPERATION_EXT
#endif

void LogGLError(const char *file, int line, const char *call) {
  auto err = glGetError();
  if (err == GL_NO_ERROR) return;
  const char *err_str = "<unknown error enum>";
  switch (err) {
    case GL_INVALID_ENUM:
      err_str = "GL_INVALID_ENUM";
      break;
    case GL_INVALID_VALUE:
      err_str = "GL_INVALID_VALUE";
      break;
    case GL_INVALID_OPERATION:
      err_str = "GL_INVALID_OPERATION";
      break;
    case GL_INVALID_FRAMEBUFFER_OPERATION:
      err_str = "GL_INVALID_FRAMEBUFFER_OPERATION";
      break;
    case GL_OUT_OF_MEMORY:
      err_str = "GL_OUT_OF_MEMORY";
      break;
  }
  LogError(fplbase::kError, "%s(%d): OpenGL Error: %s from %s", file, line,
           err_str, call);
  assert(0);
}

#if !defined(GL_GLEXT_PROTOTYPES)
#if !defined(PLATFORM_MOBILE) && !defined(__APPLE__)
#define GLEXT(type, name, required) type name = nullptr;
GLBASEEXTS GLEXTS
#undef GLEXT
#endif
#endif  // !defined(GL_GLEXT_PROTOTYPES)

#ifdef PLATFORM_MOBILE
#define GLEXT(type, name, required) type name = nullptr;
    GLESEXTS
#endif  // PLATFORM_MOBILE
