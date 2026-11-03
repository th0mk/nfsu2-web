/* shader_to_glsl.c -- translates WGSL modules (the game's dump: NFSU2_NX_SHADER_DUMP) with runtime/switch/wgsl2glsl.c and
 * compiles each result with the desktop OpenGL 4.3 core of this machine (EGL, Mesa llvmpipe).
 *   shader_to_glsl [-v] file.wgsl...        -v prints the GLSL of every module
 * Exit status: the number of modules that failed. */
#include "../wgsl2glsl.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef unsigned GLuint; typedef unsigned GLenum; typedef int GLint; typedef int GLsizei; typedef char GLchar;
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_VERTEX_SHADER 0x8B31
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
static GLuint (*glCreateShader)(GLenum);
static void (*glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *);
static void (*glCompileShader)(GLuint);
static void (*glGetShaderiv)(GLuint, GLenum, GLint *);
static void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static GLuint (*glCreateProgram)(void);
static void (*glAttachShader)(GLuint, GLuint);
static void (*glLinkProgram)(GLuint);
static void (*glGetProgramiv)(GLuint, GLenum, GLint *);
static void (*glGetProgramInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *);
static const unsigned char *(*glGetString)(GLenum);
#define LOAD(f) do { *(void **)&f = (void *)eglGetProcAddress(#f); if (!f){ fprintf(stderr, "no " #f "\n"); return 99; } } while (0)

int main(int argc, char **argv){
    int verbose = 0, first = 1;
    if (argc > 1 && !strcmp(argv[1], "-v")){ verbose = 1; first = 2; }
    EGLDisplay d = EGL_NO_DISPLAY;
    PFNEGLGETPLATFORMDISPLAYEXTPROC gpd = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (gpd) d = gpd(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    if (d == EGL_NO_DISPLAY) d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!eglInitialize(d, NULL, NULL)){ fprintf(stderr, "eglInitialize failed\n"); return 99; }
    eglBindAPI(EGL_OPENGL_API);
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    if (!eglChooseConfig(d, ca, &cfg, 1, &n) || !n){ fprintf(stderr, "no config\n"); return 99; }
    EGLint pa[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    EGLSurface s = eglCreatePbufferSurface(d, cfg, pa);
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 4, EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE };
    EGLContext c = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (c == EGL_NO_CONTEXT){ fprintf(stderr, "no GL 4.3 core context (0x%x)\n", eglGetError()); return 99; }
    eglMakeCurrent(d, s, s, c);
    LOAD(glCreateShader); LOAD(glShaderSource); LOAD(glCompileShader); LOAD(glGetShaderiv); LOAD(glGetShaderInfoLog);
    LOAD(glCreateProgram); LOAD(glAttachShader); LOAD(glLinkProgram); LOAD(glGetProgramiv); LOAD(glGetProgramInfoLog); LOAD(glGetString);
    fprintf(stderr, "GL %s / %s\n", glGetString(0x1F02), glGetString(0x1F01));
    static char glsl[1 << 18], err[256], text[1 << 17];
    int bad = 0, ok = 0;
    for (int a = first; a < argc; a++){
        FILE *f = fopen(argv[a], "rb"); if (!f){ printf("%s: cannot open\n", argv[a]); bad++; continue; }
        size_t len = fread(text, 1, sizeof text - 1, f); fclose(f); text[len] = 0;
        int frag = 0;
        int rc = wgsl_to_glsl(text, glsl, sizeof glsl, &frag, err, sizeof err);
        if (rc){ printf("%s: TRANSLATE FAILED (%d): %s\n", argv[a], rc, err); bad++; continue; }
        if (verbose) printf("=== %s\n%s\n", argv[a], glsl);
        GLuint sh = glCreateShader(frag ? GL_FRAGMENT_SHADER : GL_VERTEX_SHADER);
        const char *src = glsl; glShaderSource(sh, 1, &src, NULL); glCompileShader(sh);
        GLint st = 0; glGetShaderiv(sh, GL_COMPILE_STATUS, &st);
        if (!st){ char log[2048]; glGetShaderInfoLog(sh, sizeof log, NULL, log); printf("%s: COMPILE FAILED: %s\n", argv[a], log); bad++; continue; }
        GLuint pr = glCreateProgram(); glAttachShader(pr, sh); glLinkProgram(pr);   /* a stage alone links with separable programs only: the compile is the check */
        ok++;
    }
    printf("%d translated and compiled, %d failed\n", ok, bad);
    return bad;
}
