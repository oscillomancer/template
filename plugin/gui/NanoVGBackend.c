// The single translation unit that instantiates NanoVG's OpenGL backend.
//
// GL2 rather than GL3: everything this panel draws is ordinary filled and
// stroked paths, so the VAO-based GL3 path buys nothing measurable, while GL2
// links directly against the system GL on both Linux (Mesa exports the 2.0
// entry points) and macOS (the OpenGL framework does the same).
//
// Windows is the exception, and the reason for the bulk of this file:
// opengl32.dll has exported nothing past GL 1.1 since 1996, and the Windows SDK
// ships no glext.h, so both the entry points and the enums have to be declared
// here and resolved through wglGetProcAddress. It is mechanical rather than
// subtle, and it keeps the mess confined to one place.

#if defined(_WIN32)

#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#  include <stddef.h>
#  include <GL/gl.h>

typedef char      GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;

#  ifndef GL_ARRAY_BUFFER
#    define GL_ARRAY_BUFFER      0x8892
#    define GL_STREAM_DRAW       0x88E0
#    define GL_FRAGMENT_SHADER   0x8B30
#    define GL_VERTEX_SHADER     0x8B31
#    define GL_COMPILE_STATUS    0x8B81
#    define GL_LINK_STATUS       0x8B82
#    define GL_TEXTURE0          0x84C0
#    define GL_UNPACK_SKIP_PIXELS 0x0CF4
#    define GL_UNPACK_SKIP_ROWS  0x0CF3
#  endif

// name, return type, parameter list -- one row per entry point NanoVG's GL2
// backend calls above GL 1.1.
#  define STARTER_GL_PROCS                                                     \
    X(glActiveTexture, void, (GLenum))                                         \
    X(glAttachShader, void, (GLuint, GLuint))                                  \
    X(glBindAttribLocation, void, (GLuint, GLuint, const GLchar *))            \
    X(glBindBuffer, void, (GLenum, GLuint))                                    \
    X(glBlendFuncSeparate, void, (GLenum, GLenum, GLenum, GLenum))             \
    X(glBufferData, void, (GLenum, GLsizeiptr, const void *, GLenum))          \
    X(glCompileShader, void, (GLuint))                                         \
    X(glCreateProgram, GLuint, (void))                                         \
    X(glCreateShader, GLuint, (GLenum))                                        \
    X(glDeleteBuffers, void, (GLsizei, const GLuint *))                        \
    X(glDeleteProgram, void, (GLuint))                                         \
    X(glDeleteShader, void, (GLuint))                                          \
    X(glDisableVertexAttribArray, void, (GLuint))                              \
    X(glEnableVertexAttribArray, void, (GLuint))                               \
    X(glGenBuffers, void, (GLsizei, GLuint *))                                 \
    X(glGenerateMipmap, void, (GLenum))                                        \
    X(glGetProgramInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))       \
    X(glGetProgramiv, void, (GLuint, GLenum, GLint *))                         \
    X(glGetShaderInfoLog, void, (GLuint, GLsizei, GLsizei *, GLchar *))        \
    X(glGetShaderiv, void, (GLuint, GLenum, GLint *))                          \
    X(glGetUniformLocation, GLint, (GLuint, const GLchar *))                   \
    X(glLinkProgram, void, (GLuint))                                           \
    X(glShaderSource, void, (GLuint, GLsizei, const GLchar *const *,           \
                             const GLint *))                                   \
    X(glStencilOpSeparate, void, (GLenum, GLenum, GLenum, GLenum))             \
    X(glUniform1i, void, (GLint, GLint))                                       \
    X(glUniform2fv, void, (GLint, GLsizei, const GLfloat *))                   \
    X(glUniform4fv, void, (GLint, GLsizei, const GLfloat *))                   \
    X(glUseProgram, void, (GLuint))                                            \
    X(glVertexAttribPointer, void, (GLuint, GLint, GLenum, GLboolean, GLsizei, \
                                    const void *))

#  define X(name, ret, params) typedef ret(APIENTRY *starter_pfn_##name) params;
STARTER_GL_PROCS
#  undef X

#  define X(name, ret, params) static starter_pfn_##name starter_##name;
STARTER_GL_PROCS
#  undef X

int starterLoadGL(void)
{
#  define X(name, ret, params)                                                 \
    starter_##name = (starter_pfn_##name)wglGetProcAddress(#name);             \
    if (!starter_##name) return 0;
    STARTER_GL_PROCS
#  undef X
    return 1;
}

// Redirect the names onto the resolved pointers for everything below.
#  define glActiveTexture            starter_glActiveTexture
#  define glAttachShader             starter_glAttachShader
#  define glBindAttribLocation       starter_glBindAttribLocation
#  define glBindBuffer               starter_glBindBuffer
#  define glBlendFuncSeparate        starter_glBlendFuncSeparate
#  define glBufferData               starter_glBufferData
#  define glCompileShader            starter_glCompileShader
#  define glCreateProgram            starter_glCreateProgram
#  define glCreateShader             starter_glCreateShader
#  define glDeleteBuffers            starter_glDeleteBuffers
#  define glDeleteProgram            starter_glDeleteProgram
#  define glDeleteShader             starter_glDeleteShader
#  define glDisableVertexAttribArray starter_glDisableVertexAttribArray
#  define glEnableVertexAttribArray  starter_glEnableVertexAttribArray
#  define glGenBuffers               starter_glGenBuffers
#  define glGenerateMipmap           starter_glGenerateMipmap
#  define glGetProgramInfoLog        starter_glGetProgramInfoLog
#  define glGetProgramiv             starter_glGetProgramiv
#  define glGetShaderInfoLog         starter_glGetShaderInfoLog
#  define glGetShaderiv              starter_glGetShaderiv
#  define glGetUniformLocation       starter_glGetUniformLocation
#  define glLinkProgram              starter_glLinkProgram
#  define glShaderSource             starter_glShaderSource
#  define glStencilOpSeparate        starter_glStencilOpSeparate
#  define glUniform1i                starter_glUniform1i
#  define glUniform2fv               starter_glUniform2fv
#  define glUniform4fv               starter_glUniform4fv
#  define glUseProgram               starter_glUseProgram
#  define glVertexAttribPointer      starter_glVertexAttribPointer

#elif defined(__APPLE__)

#  ifndef GL_SILENCE_DEPRECATION  // also set on the pugl target
#    define GL_SILENCE_DEPRECATION
#  endif
#  include <OpenGL/gl.h>

int starterLoadGL(void) { return 1; }

#else

#  include <GL/gl.h>
#  include <GL/glext.h>

int starterLoadGL(void) { return 1; }

#endif

#define NANOVG_GL2_IMPLEMENTATION
#include "nanovg.h"
#include "nanovg_gl.h"

// Context creation lives behind the same seam as everything else here, so that
// Window.cpp names no renderer at all. The Metal backend's version of this
// takes the CAMetalLayer through nativeContext; GL has no equivalent and
// ignores it. Keeping the signatures identical is what lets the two swap.
//
// The create flags are the renderer's own, so they are chosen here rather than
// passed in. NVG_STENCIL_STROKES needs the stencil buffer requested in
// Window::create; without it the multi-pass glow strokes double-blend at the
// joins.

NVGcontext* starterNvgCreate(void* nativeContext)
{
    (void)nativeContext;
    return nvgCreateGL2(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
}

void starterNvgDelete(NVGcontext* ctx)
{
    if (ctx) {
        nvgDeleteGL2(ctx);
    }
}

// The only two GL calls the editor needs that NanoVG does not make for us.
// Keeping them here means no C++ translation unit has to include a GL header,
// which on Windows would mean repeating the mess above.

void starterViewport(int w, int h) { glViewport(0, 0, w, h); }

// The context is unused here; Metal needs it, because clearing there is a
// render-pass property recorded against the context rather than a command.
void starterClear(NVGcontext* ctx, float r, float g, float b)
{
    (void)ctx;
    glClearColor(r, g, b, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
}

// Used only by the offline preview tool, which renders the real editor and
// writes the framebuffer out so the panel can be checked without a host.
// The size is asked of GL rather than derived from the view: whether the
// framebuffer is scaled depends on the display the window happened to open on.
void starterViewportSize(int* w, int* h)
{
    GLint vp[4] = {0, 0, 0, 0};
    glGetIntegerv(GL_VIEWPORT, vp);
    *w = vp[2];
    *h = vp[3];
}

void starterReadPixels(int w, int h, unsigned char* rgb)
{
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, rgb);
}
