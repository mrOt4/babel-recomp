/*
 * opengl.c — the shared desktop OpenGL presenter.
 *
 * util.h has declared `OpenGLRenderer_Create(struct RendererFuncs *)` for a
 * long time, and mmx23_host_main.inc calls it, but the framework shipped no
 * implementation: every port carried its own copy. Seven of them did, at
 * ~225 lines each, differing only in the name of a per-game viewport helper.
 *
 * That is the shape a shared fix cannot reach, and the cost was already on
 * the board: one port's copy computed
 *
 *     int viewport_y = (viewport_height - viewport_height) >> 1;
 *
 * which is zero for every window size, so its picture never centred
 * vertically. Six other copies had the line right. Nothing could tell them
 * apart because nothing compared them.
 *
 * Presentation policy comes from the framework's own config (`display_aspect`,
 * `ignore_aspect_ratio`, `linear_filtering`, `shader`) via
 * SnesDisplayAspect_ComputeViewport, which already handles all three pixel
 * aspects, integer scaling and seam suppression — the per-game helpers this
 * replaces mostly hardcoded 7:6.
 *
 * Vendored originally from snesrev/smw's src/opengl.c (MIT, (c) 2023 snesrev,
 * (c) 2021 elzo_d); see THIRD_PARTY_ATTRIBUTION.md.
 */

#include "gl_core_3_1.h"
#include "sdl_compat.h"

#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "types.h"
#include "util.h"
#include "glsl_shader.h"
#if SNESRECOMP_TOUCH_PAD
#include "touch_pad.h"
#endif
#include "config.h"
#include "display_aspect.h"
#include "present_filter.h"

#define CODE(...) #__VA_ARGS__

static SDL_Window *g_window;
static uint8 *g_screen_buffer;
static size_t g_screen_buffer_size;
static int g_draw_width, g_draw_height;
static unsigned int g_program, g_flash_program, g_VAO;
/* PresentFilter programs (0 if they failed to build: plain program). */
static unsigned int g_sharp_program, g_smooth_program;

/* Sharp: solid SNES pixels at any scale. Sample at the texel centre except
 * within one output pixel of a texel edge, where bilinear filtering blends
 * the two texels across exactly that pixel (needs GL_LINEAR). */
static const char *kSharpFs =
    "precision highp float;\n"
    "out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D texture1;\n"
    "uniform vec2 uSrc;\n"
    "void main(void) {\n"
    "  vec2 px = TexCoord * uSrc;\n"
    "  vec2 seam = floor(px + 0.5);\n"
    "  vec2 d = max(fwidth(px), vec2(1e-5));\n"
    "  px = seam + clamp((px - seam) / d, -0.5, 0.5);\n"
    "  FragColor = texture(texture1, px / uSrc).SWZ;\n"
    "}\n";

/* Smooth: xBR level 2 (after Hyllian's xBR), one pass at any scale. Each
 * output pixel looks at the quadrant of its SNES pixel it falls in; the
 * neighbourhood is mirrored so that quadrant is always the bottom-right
 * one, and the bottom-right corner rule decides whether an edge (45, 30 or
 * 60 degrees) crosses it. Edges are antialiased over one output pixel.
 * Needs GL_NEAREST (texelFetch). */
static const char *kSmoothFs =
    "precision highp float;\n"
    "out vec4 FragColor;\n"
    "in vec2 TexCoord;\n"
    "uniform sampler2D texture1;\n"
    "uniform vec2 uSrc;\n"
    "ivec2 gC, gS;\n"
    "vec3 P(int x, int y) {\n"
    "  ivec2 p = clamp(gC + ivec2(x, y) * gS, ivec2(0), ivec2(uSrc) - 1);\n"
    "  return texelFetch(texture1, p, 0).SWZ.rgb;\n"
    "}\n"
    "float df(vec3 a, vec3 b) {\n"   /* luma difference, x48 as in xBR */
    "  return abs(dot(a - b, vec3(14.352, 28.176, 5.472)));\n"
    "}\n"
    "bool eq(vec3 a, vec3 b) { return df(a, b) < 15.0; }\n"
    "bool ne(vec3 a, vec3 b) { return any(greaterThan(abs(a - b), vec3(1.0 / 512.0))); }\n"
    "void main(void) {\n"
    "  vec2 px = TexCoord * uSrc;\n"
    "  gC = ivec2(floor(px));\n"
    "  vec2 fp = fract(px);\n"
    "  gS = ivec2(fp.x < 0.5 ? -1 : 1, fp.y < 0.5 ? -1 : 1);\n"
    "  vec2 f = vec2(gS.x > 0 ? fp.x : 1.0 - fp.x, gS.y > 0 ? fp.y : 1.0 - fp.y);\n"
    "  vec3 E = P(0, 0), B = P(0, -1), D = P(-1, 0), F = P(1, 0), H = P(0, 1);\n"
    "  vec3 C = P(1, -1), G = P(-1, 1), I = P(1, 1);\n"
    "  vec3 F4 = P(2, 0), I4 = P(2, 1), H5 = P(0, 2), I5 = P(1, 2);\n"
    "  float e = df(E, C) + df(E, G) + df(I, H5) + df(I, F4) + 4.0 * df(H, F);\n"
    "  float i = df(H, D) + df(H, I5) + df(F, I4) + df(F, B) + 4.0 * df(E, I);\n"
    "  bool rst = ne(E, F) && ne(E, H) &&\n"
    "             ((!eq(F, B) && !eq(H, D)) || (eq(E, I) && !eq(F, I4) && !eq(H, I5)) ||\n"
    "              eq(E, G) || eq(E, C));\n"
    "  bool edr = e < i && rst;\n"
    "  bool edrL = edr && 2.0 * df(F, G) <= df(H, C) && ne(E, G) && ne(D, G);\n"
    "  bool edrU = edr && df(F, G) >= 2.0 * df(H, C) && ne(E, C) && ne(B, C);\n"
    "  vec3 nc = df(E, F) <= df(E, H) ? F : H;\n"
    "  float aa = max(max(fwidth(px.x), fwidth(px.y)), 1e-4);\n"
    "  float w = 0.0;\n"
    "  if (edr)  w = smoothstep(1.5 - aa, 1.5 + aa, f.x + f.y);\n"
    "  if (edrL) w = max(w, smoothstep(1.0 - aa, 1.0 + aa, f.y + 0.5 * f.x));\n"
    "  if (edrU) w = max(w, smoothstep(1.0 - aa, 1.0 + aa, f.x + 0.5 * f.y));\n"
    "  FragColor = vec4(mix(E, nc, w), 1.0);\n"
    "}\n";

/* Builds vs + one of the filter fragment shaders; 0 on failure. */
static unsigned int BuildFilterProgram(unsigned int vs, const char *head, const char *body) {
  unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
  const GLchar *code[2] = {head, body};
  glShaderSource(fs, 2, code, NULL);
  glCompileShader(fs);
  int ok = 0;
  char log[1024];
  glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
  if (!ok) {
    glGetShaderInfoLog(fs, sizeof(log), NULL, log);
    fprintf(stderr, "[gl] scaling filter shader: %s\n", log);
    glDeleteShader(fs);
    return 0;
  }
  unsigned int prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(fs);
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (!ok) {
    glGetProgramInfoLog(prog, sizeof(log), NULL, log);
    fprintf(stderr, "[gl] scaling filter program: %s\n", log);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}
static GlTextureWithSize g_texture;
static GlslShader *g_glsl_shader;
static bool g_want_screenshot;
static double g_screenshot_flash_start_time = -1.0;
static const double kScreenshotFlashSeconds = 0.15;

extern void MkDir(const char *s);

/* SNESRECOMP_GL_SCREENSHOT=<path.ppm> [SNESRECOMP_GL_SCREENSHOT_FRAME=<n>]:
 * the presented picture as the GPU drew it, for headless
 * checks with SDL_VIDEODRIVER=offscreen. */
extern int snes_frame_counter;
/* SNESRECOMP_GL_SCREENSHOT_TRIGGER=<file>: whenever <file> exists, the next
 * present is saved to the path written inside it and the file is removed. */
static void MaybeGpuScreenshot(int w, int h) {
  static int done;
  char trig_path[1024];
  const char *path = NULL;
  const char *trig = getenv("SNESRECOMP_GL_SCREENSHOT_TRIGGER");
  if (trig && *trig) {
    FILE *t = fopen(trig, "rb");
    if (!t) return;
    size_t n = fread(trig_path, 1, sizeof(trig_path) - 1, t);
    fclose(t);
    remove(trig);
    while (n && (trig_path[n - 1] == '\n' || trig_path[n - 1] == '\r' || trig_path[n - 1] == ' ')) n--;
    trig_path[n] = 0;
    path = trig_path;
  } else {
    if (done) return;
    path = getenv("SNESRECOMP_GL_SCREENSHOT");
    if (!path || !*path) { done = 1; return; }
    const char *fr = getenv("SNESRECOMP_GL_SCREENSHOT_FRAME");
    if (fr && snes_frame_counter < atoi(fr)) return;
    done = 1;
  }
  uint8 *px = (uint8 *)malloc((size_t)w * h * 4);
  if (!px) return;
  glPixelStorei(GL_PACK_ALIGNMENT, 4);
  glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);   /* valid in GL and GLES */
  for (size_t i = 0, n = (size_t)w * h; i < n; i++) {
    px[i * 3] = px[i * 4]; px[i * 3 + 1] = px[i * 4 + 1]; px[i * 3 + 2] = px[i * 4 + 2];
  }
  FILE *f = fopen(path, "wb");
  if (f) {
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = h - 1; y >= 0; y--)
      fwrite(px + (size_t)y * w * 3, 1, (size_t)w * 3, f);
    fclose(f);
    fprintf(stderr, "[gl] screenshot %dx%d -> %s (frame %d)\n", w, h, path, snes_frame_counter);
  }
  free(px);
}

/* -1 = decide from config at init. A host that paces presentation itself
 * (an FPS cap, a simulation/presentation split) sets 0 so the swap does not
 * also block; one that wants the driver to pace it sets 1. */
static int g_vsync_override = -1;

void snesrecomp_opengl_set_vsync(int enable) {
  g_vsync_override = enable ? 1 : 0;
}

static void GL_APIENTRY MessageCallback(GLenum source,
                GLenum type,
                GLuint id,
                GLenum severity,
                GLsizei length,
                const GLchar *message,
                const void *userParam) {
  (void)source; (void)id; (void)length; (void)userParam;
  if (type == GL_DEBUG_TYPE_OTHER)
    return;

  fprintf(stderr, "GL CALLBACK: %s type = 0x%x, severity = 0x%x, message = %s\n",
          (type == GL_DEBUG_TYPE_ERROR ? "** GL ERROR **" : ""),
          type, severity, message);
  if (type == GL_DEBUG_TYPE_ERROR)
    Die("OpenGL error!\n");
}

/* GLES 3.0 mode: no BGRA upload or readback, no debug output, shader
 * presets off; the built-in shaders get an ES header and swizzle the ARGB
 * frame themselves. */
static bool g_gles;
#ifndef GL_TEXTURE_SWIZZLE_R
#define GL_TEXTURE_SWIZZLE_R 0x8E42
#define GL_TEXTURE_SWIZZLE_G 0x8E43
#define GL_TEXTURE_SWIZZLE_B 0x8E44
#define GL_TEXTURE_SWIZZLE_A 0x8E45
#endif
/* The frame texture as every shader expects it: RGB in order and opaque.
 * GLES uploads the ARGB words as RGBA bytes, so red and blue come in
 * swapped; and the frame's alpha byte is 0 on every platform. Set on each
 * upload because a preset with frame history swaps textures around. */
static void SetFrameSwizzle(void) {
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, g_gles ? GL_BLUE : GL_RED);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, g_gles ? GL_RED : GL_BLUE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
}

static GLenum UploadFormat(void) { return g_gles ? GL_RGBA : GL_BGRA; }
static GLenum UploadType(void) { return g_gles ? GL_UNSIGNED_BYTE : GL_UNSIGNED_INT_8_8_8_8_REV; }

static bool OpenGLRenderer_Init(SDL_Window *window) {
  g_window = window;
  SDL_GLContext context = SDL_GL_CreateContext(window);
  if (!context) {
    fprintf(stderr, "[gl] cannot create an OpenGL%s context: %s\n",
            g_gles ? " ES 3.0" : " 3.3", SDL_GetError());
    return false;
  }

  SDL_GL_SetSwapInterval(g_vsync_override >= 0
                             ? g_vsync_override
                             : (g_config.disable_frame_delay ? 0 : 1));
  ogl_LoadFunctions();

  fprintf(stderr, "[gl] context: %s (%s)\n", (const char *)glGetString(GL_VERSION),
          (const char *)glGetString(GL_RENDERER));
  if (!g_gles && !ogl_IsVersionGEQ(3, 3))
    Die("You need OpenGL 3.3");

  if (kDebugFlag && !g_gles) {
    glEnable(GL_DEBUG_OUTPUT);
    glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
    glDebugMessageCallback(MessageCallback, 0);
  }

  glGenTextures(1, &g_texture.gl_texture);

  static const float kVertices[] = {
    // positions          // texture coords
    -1.0f,  1.0f, 0.0f,   0.0f, 0.0f, // top left
    -1.0f, -1.0f, 0.0f,   0.0f, 1.0f, // bottom left
     1.0f,  1.0f, 0.0f,   1.0f, 0.0f, // top right
     1.0f, -1.0f, 0.0f,   1.0f, 1.0f, // bottom right
  };

  // create a vertex buffer object
  unsigned int vbo;
  glGenBuffers(1, &vbo);

  // vertex array object
  glGenVertexArrays(1, &g_VAO);
  // 1. bind Vertex Array Object
  glBindVertexArray(g_VAO);
  // 2. copy our vertices array in a buffer for OpenGL to use
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);
  // position attribute
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
  glEnableVertexAttribArray(0);
  // texture coord attribute
  glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
  glEnableVertexAttribArray(1);

  // vertex shader
  const GLchar *vs_code_body = CODE(
  layout(location = 0) in vec3 aPos;
  layout(location = 1) in vec2 aTexCoord;
  out vec2 TexCoord;
  void main(void) {
    gl_Position = vec4(aPos, 1.0);
    TexCoord = vec2(aTexCoord.x, aTexCoord.y);
  }
);

  const GLchar *head = g_gles ? "#version 300 es\nprecision mediump float;\n#define SWZ rgba\n"
                              : "#version 330 core\n#define SWZ rgba\n";
  const GLchar *vs_code[2] = {head, vs_code_body};
  unsigned int vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 2, vs_code, NULL);
  glCompileShader(vs);

  int success;
  char infolog[512];
  glGetShaderiv(vs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(vs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // fragment shader
  const GLchar *fs_code_body = CODE(
  out vec4 FragColor;
  in vec2 TexCoord;
  // texture samplers
  uniform sampler2D texture1;
  void main(void) {
    FragColor = texture(texture1, TexCoord).SWZ;
  }
);

  unsigned int fs = glCreateShader(GL_FRAGMENT_SHADER);
  const GLchar *fs_code[2] = {head, fs_code_body};
  glShaderSource(fs, 2, fs_code, NULL);
  glCompileShader(fs);

  glGetShaderiv(fs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(fs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  // create program
  int program = g_program = glCreateProgram();
  glAttachShader(program, vs);
  glAttachShader(program, fs);
  glLinkProgram(program);
  glGetProgramiv(program, GL_LINK_STATUS, &success);

  if (!success) {
    glGetProgramInfoLog(program, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  const GLchar *flash_fs_code_body = CODE(
  out vec4 FragColor;
  uniform vec4 color;
  void main(void) {
    FragColor = color;
  }
);
  unsigned int flash_fs = glCreateShader(GL_FRAGMENT_SHADER);
  const GLchar *flash_fs_code[2] = {head, flash_fs_code_body};
  glShaderSource(flash_fs, 2, flash_fs_code, NULL);
  glCompileShader(flash_fs);
  glGetShaderiv(flash_fs, GL_COMPILE_STATUS, &success);
  if (!success) {
    glGetShaderInfoLog(flash_fs, 512, NULL, infolog);
    printf("%s\n", infolog);
  }
  g_flash_program = glCreateProgram();
  glAttachShader(g_flash_program, vs);
  glAttachShader(g_flash_program, flash_fs);
  glLinkProgram(g_flash_program);
  glGetProgramiv(g_flash_program, GL_LINK_STATUS, &success);
  if (!success) {
    glGetProgramInfoLog(g_flash_program, 512, NULL, infolog);
    printf("%s\n", infolog);
  }

  g_sharp_program = BuildFilterProgram(vs, head, kSharpFs);
  g_smooth_program = BuildFilterProgram(vs, head, kSmoothFs);

  if (g_config.shader) {
    GlslShader_SetGles(g_gles);
    g_glsl_shader = GlslShader_CreateFromFile(g_config.shader);
    if (!g_glsl_shader)
      fprintf(stderr, "[gl] shader preset '%s' failed to load; plain presentation\n", g_config.shader);
  }

  return true;
}

static void OpenGLRenderer_Destroy(void) {
}

static void OpenGLRenderer_GetOutputSize(int *width, int *height) {
  snesrecomp_sdl_get_drawable_size(g_window, width, height);
}

void OpenGLRenderer_RequestScreenshot(void) {
  g_want_screenshot = true;
}

static double MonotonicSeconds(void) {
  return (double)SDL_GetPerformanceCounter() / SDL_GetPerformanceFrequency();
}

static void SaveScreenshotBmp(int width, int height) {
  size_t pixel_size = (size_t)width * (size_t)height * 4;
  uint8 *pixels = (uint8 *)malloc(pixel_size);
  if (!pixels)
    return;

  if (g_gles) {
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    for (size_t i = 0; i < pixel_size; i += 4) {
      uint8 t = pixels[i]; pixels[i] = pixels[i + 2]; pixels[i + 2] = t;
    }
  } else {
    glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, pixels);
  }

  MkDir("screenshots");

  time_t t = time(NULL);
  struct tm tm_snapshot;
  struct tm *local = localtime(&t);
  if (local)
    tm_snapshot = *local;
  else
    memset(&tm_snapshot, 0, sizeof(tm_snapshot));

  char path[256];
  snprintf(path, sizeof(path),
           "screenshots/screenshot_%04d%02d%02d_%02d%02d%02d.bmp",
           tm_snapshot.tm_year + 1900, tm_snapshot.tm_mon + 1,
           tm_snapshot.tm_mday, tm_snapshot.tm_hour, tm_snapshot.tm_min,
           tm_snapshot.tm_sec);

  FILE *f = fopen(path, "wb");
  if (f) {
    uint32 pixel_data_size = (uint32)pixel_size;
    uint32 file_size = 14 + 40 + pixel_data_size;
    uint32 pixel_offset = 14 + 40;
    uint32 dib_size = 40;
    int32 w = width;
    int32 h = height;
    uint16 planes = 1;
    uint16 bits = 32;
    uint32 zero = 0;

    fwrite("BM", 1, 2, f);
    fwrite(&file_size, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&pixel_offset, 4, 1, f);
    fwrite(&dib_size, 4, 1, f);
    fwrite(&w, 4, 1, f);
    fwrite(&h, 4, 1, f);
    fwrite(&planes, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&pixel_data_size, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(&zero, 4, 1, f);
    fwrite(pixels, 1, pixel_size, f);
    fclose(f);
    printf("Screenshot saved: %s\n", path);
    g_screenshot_flash_start_time = MonotonicSeconds();
  } else {
    fprintf(stderr, "Screenshot: couldn't open '%s' for writing\n", path);
  }

  free(pixels);
}

static bool DrawScreenshotFlash(double elapsed_seconds) {
  if (elapsed_seconds >= kScreenshotFlashSeconds)
    return false;

  float alpha = 0.5f * (float)(1.0 - elapsed_seconds / kScreenshotFlashSeconds);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(g_flash_program);
  glUniform4f(glGetUniformLocation(g_flash_program, "color"), 1.0f, 1.0f, 1.0f, alpha);
  glBindVertexArray(g_VAO);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisable(GL_BLEND);
  return true;
}

#if SNESRECOMP_TOUCH_PAD
/* The on-screen pad (touch_pad.c) over the whole drawable, with its alpha.
 * Uploaded only when its picture changes. */
static void DrawTouchPad(int drawable_w, int drawable_h) {
  static GLuint tex;
  static unsigned uploaded = ~0u;
  int w, h;
  unsigned serial;
  const uint32_t *img = TouchPad_Image(drawable_w, drawable_h, &w, &h, &serial);
  if (!img) return;
  if (!tex) glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  if (serial != uploaded) {
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, UploadFormat(), UploadType(), img);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, g_gles ? GL_BLUE : GL_RED);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_G, GL_GREEN);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, g_gles ? GL_RED : GL_BLUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ALPHA);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    uploaded = serial;
  }
  glViewport(0, 0, drawable_w, drawable_h);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(g_program);
  glBindVertexArray(g_VAO);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisable(GL_BLEND);
}
#endif

static void OpenGLRenderer_BeginDraw(int width, int height, uint8 **pixels, int *pitch) {
  size_t size = (size_t)width * (size_t)height;

  if (size > g_screen_buffer_size) {
    g_screen_buffer_size = size;
    free(g_screen_buffer);
    g_screen_buffer = (uint8 *)malloc(size * 4);
  }

  g_draw_width = width;
  g_draw_height = height;
  *pixels = g_screen_buffer;
  *pitch = width * 4;
}

static void OpenGLRenderer_EndDraw(void) {
  int drawable_width, drawable_height;

  snesrecomp_sdl_get_drawable_size(g_window, &drawable_width, &drawable_height);

  /* The framework's own aspect maths, rather than a per-game copy: it knows
   * all three pixel aspects the config offers, and centres on both axes. */
  SnesDisplayViewport viewport;
  SnesDisplayAspect_ComputeViewport(
      g_draw_width, g_draw_height, drawable_width, drawable_height,
      SnesDisplayAspect_ForPresent(g_config.display_aspect),
      (g_config.ignore_aspect_ratio && !g_present_square_pixels) || g_present_square_pixels == 2,
      false, &viewport);

  glBindTexture(GL_TEXTURE_2D, g_texture.gl_texture);
  SetFrameSwizzle();
  if (g_draw_width == g_texture.width && g_draw_height == g_texture.height) {
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g_draw_width, g_draw_height, UploadFormat(), UploadType(), g_screen_buffer);
  } else {
    g_texture.width = g_draw_width;
    g_texture.height = g_draw_height;
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g_draw_width, g_draw_height, 0, UploadFormat(), UploadType(), g_screen_buffer);
  }

  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT);

  if (g_glsl_shader == NULL) {
    glViewport(viewport.x, viewport.y, viewport.width, viewport.height);
    /* HD scaling (present_filter.h). Smooth only redraws the 1x art: a 4x
     * frame (HD pack, Mode 7 at 4x) is already high resolution. */
    int mode = PresentFilter_Get();
    if (mode == kPresentFilter_Smooth && (g_draw_width > 640 || !g_smooth_program))
      mode = kPresentFilter_Sharp;
    if (mode == kPresentFilter_Sharp && !g_sharp_program)
      mode = kPresentFilter_Config;
    unsigned int prog = mode == kPresentFilter_Sharp ? g_sharp_program :
                        mode == kPresentFilter_Smooth ? g_smooth_program : g_program;
    glUseProgram(prog);
    if (prog != g_program) {
      glUniform1i(glGetUniformLocation(prog, "texture1"), 0);
      glUniform2f(glGetUniformLocation(prog, "uSrc"), (float)g_draw_width, (float)g_draw_height);
    }
    int filter = mode == kPresentFilter_Sharp ? GL_LINEAR :
                 mode == kPresentFilter_Smooth ? GL_NEAREST :
                 g_config.linear_filtering ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glBindVertexArray(g_VAO);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  } else {
    GlslShader_Render(g_glsl_shader, &g_texture, viewport.x, viewport.y,
                      viewport.width, viewport.height);
  }

#if SNESRECOMP_TOUCH_PAD
  DrawTouchPad(drawable_width, drawable_height);
#endif
  MaybeGpuScreenshot(drawable_width, drawable_height);
  if (g_want_screenshot) {
    g_want_screenshot = false;
    SaveScreenshotBmp(drawable_width, drawable_height);
  }
  if (g_screenshot_flash_start_time >= 0.0) {
    double now = MonotonicSeconds();
    glViewport(0, 0, drawable_width, drawable_height);
    if (!DrawScreenshotFlash(now - g_screenshot_flash_start_time))
      g_screenshot_flash_start_time = -1.0;
  }

  SDL_GL_SwapWindow(g_window);
}

static const struct RendererFuncs kOpenGLRendererFuncs = {
  &OpenGLRenderer_Init,
  &OpenGLRenderer_Destroy,
  &OpenGLRenderer_GetOutputSize,
  &OpenGLRenderer_BeginDraw,
  &OpenGLRenderer_EndDraw,
};

static bool g_core_attempt;   /* asked for desktop GL in place of the configured ES */

static void SetContextAttributes(void) {
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,
                      g_gles ? SDL_GL_CONTEXT_PROFILE_ES : SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, g_gles ? 0 : 3);
}

void OpenGLRenderer_Create(struct RendererFuncs *funcs) {
  g_gles = g_config.opengl_es;
#if defined(__SWITCH__)
  /* RetroArch's GLSL presets are written for desktop GL; under GLSL ES many
   * fail (no '##' in macros, non-constant global initializers). The Switch's
   * Mesa has desktop GL too, as RetroArch itself uses there: ask for it when
   * a preset is chosen, and fall back to ES if it is not available
   * (OpenGLRenderer_FallbackToGles). */
  if (g_gles && g_config.shader) {
    g_gles = false;
    g_core_attempt = true;
  }
#endif
  SetContextAttributes();
  *funcs = kOpenGLRendererFuncs;
}

bool OpenGLRenderer_FallbackToGles(void) {
  if (!g_core_attempt)
    return false;
  g_core_attempt = false;
  g_gles = true;
  SetContextAttributes();
  fprintf(stderr, "[gl] desktop OpenGL unavailable; retrying with OpenGL ES 3.0\n");
  return true;
}
