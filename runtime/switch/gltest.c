/* gltest.c -- what the Switch's OpenGL driver (Mesa nouveau, through EGL) gives the renderer.  A separate small .nro:
 * runtime/switch/build-gltest.sh.  It writes sdmc:/switch/nfsu2/gltest.txt (the version, the limits, the extensions the
 * game's renderer needs, whether the vertex-pulling shader of docs/web/gpu-abi.md compiles and draws, and the time of a
 * frame) and shows a colour that changes, so a screen that stays black means no context.  Press + to exit. */
#include <switch.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <glad/glad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

static FILE *g_out;
#define LOG(...) do { if (g_out){ fprintf(g_out, __VA_ARGS__); fflush(g_out); } } while (0)

static double now_ms(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e3 + t.tv_nsec / 1e6; }

static GLuint compile(GLenum type, const char *src, const char *name){
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL); glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    char log[1024] = ""; glGetShaderInfoLog(s, sizeof log, NULL, log);
    LOG("shader %-24s %s %s\n", name, ok ? "compiled" : "FAILED", log);
    return ok ? s : 0;
}

/* the shape of the game's vertex shaders (docs/web/gpu-abi.md): the vertices pulled from a storage buffer by byte
 * address, the constants in a 64 KB uniform block, the row of the draw's block a uniform instead of firstInstance */
static const char *VS =
    "#version 430 core\n"
    "layout(std430, binding = 0) readonly buffer VBuf { uint VB[]; };\n"
    "layout(std140, binding = 1) uniform KWin { vec4 c[4096]; } K;\n"
    "uniform int kb;\n"
    "out vec4 col0;\n"
    "uint ld32(uint a){ uint w = a >> 2; uint s = (a & 3u) << 3; if (s == 0u) return VB[w]; return (VB[w] >> s) | (VB[w + 1u] << (32u - s)); }\n"
    "void main(){\n"
    "  uvec4 h = floatBitsToUint(K.c[kb]);\n"
    "  uint a = h.z + uint(gl_VertexID) * h.x;\n"
    "  vec4 p = vec4(uintBitsToFloat(ld32(a)), uintBitsToFloat(ld32(a + 4u)), 0.0, 1.0);\n"
    "  gl_Position = vec4(p.xy, 0.0, 1.0) * K.c[kb + 1];\n"
    "  col0 = K.c[kb + 2];\n"
    "}\n";
static const char *FS =
    "#version 430 core\n"
    "in vec4 col0; out vec4 o;\n"
    "layout(binding = 0) uniform sampler2D tex0;\n"
    "void main(){ o = col0 * texture(tex0, vec2(0.5)); }\n";

int main(int argc, char **argv){
    mkdir("sdmc:/switch/nfsu2", 0777);
    g_out = fopen("sdmc:/switch/nfsu2/gltest.txt", "w");
    LOG("gltest: build " __DATE__ " " __TIME__ "\n");
    PadState pad; padConfigureInput(1, HidNpadStyleSet_NpadStandard); padInitializeDefault(&pad);

    EGLDisplay d = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!d || !eglInitialize(d, NULL, NULL)){ LOG("eglInitialize failed 0x%x\n", eglGetError()); return 1; }
    LOG("EGL vendor %s, version %s\n", eglQueryString(d, EGL_VENDOR), eglQueryString(d, EGL_VERSION));
    if (!eglBindAPI(EGL_OPENGL_API)){ LOG("eglBindAPI(OpenGL) failed\n"); return 1; }
    static const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT, EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8,
                                 EGL_ALPHA_SIZE, 8, EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint n = 0;
    eglChooseConfig(d, ca, &cfg, 1, &n);
    if (!n){ LOG("no EGL config\n"); return 1; }
    EGLSurface sf = eglCreateWindowSurface(d, cfg, nwindowGetDefault(), NULL);
    if (!sf){ LOG("no window surface 0x%x\n", eglGetError()); return 1; }
    static const EGLint xa[] = { EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
                                 EGL_CONTEXT_MAJOR_VERSION_KHR, 4, EGL_CONTEXT_MINOR_VERSION_KHR, 3, EGL_NONE };
    EGLContext cx = eglCreateContext(d, cfg, EGL_NO_CONTEXT, xa);
    if (!cx){ LOG("no GL 4.3 core context 0x%x\n", eglGetError()); return 1; }
    eglMakeCurrent(d, sf, sf, cx);
    if (!gladLoadGL()){ LOG("gladLoadGL failed\n"); return 1; }
    LOG("GL_VERSION  %s\nGL_RENDERER %s\nGLSL        %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER), glGetString(GL_SHADING_LANGUAGE_VERSION));
    GLint v;
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &v); LOG("max uniform block size   %d\n", v);
    glGetIntegerv(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &v); LOG("max SSBO size           %d\n", v);
    glGetIntegerv(GL_MAX_VERTEX_SHADER_STORAGE_BLOCKS, &v); LOG("max vertex SSBOs       %d\n", v);
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &v); LOG("uniform offset align    %d\n", v);
    glGetIntegerv(GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT, &v); LOG("SSBO offset align       %d\n", v);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v); LOG("max texture             %d\n", v);
    glGetIntegerv(GL_MAX_DRAW_BUFFERS, &v); LOG("max draw buffers        %d\n", v);
    glGetIntegerv(GL_MAX_SAMPLES, &v); LOG("max samples             %d\n", v);
    GLint ne = 0; glGetIntegerv(GL_NUM_EXTENSIONS, &ne);
    const char *want[] = { "GL_EXT_texture_compression_s3tc", "GL_ARB_texture_compression_bptc", "GL_EXT_texture_filter_anisotropic",
                           "GL_ARB_buffer_storage", "GL_ARB_shader_draw_parameters", "GL_ARB_clip_control", "GL_EXT_texture_sRGB", "GL_ARB_texture_swizzle" };
    for (unsigned k = 0; k < sizeof want / sizeof *want; k++){
        int has = 0; for (GLint i = 0; i < ne; i++) if (!strcmp((const char *)glGetStringi(GL_EXTENSIONS, i), want[k])) has = 1;
        LOG("%-34s %s\n", want[k], has ? "yes" : "no");
    }

    GLuint vs = compile(GL_VERTEX_SHADER, VS, "vertex pulling + 64KB UBO"), fs = compile(GL_FRAGMENT_SHADER, FS, "fragment");
    GLuint prog = 0;
    if (vs && fs){
        prog = glCreateProgram(); glAttachShader(prog, vs); glAttachShader(prog, fs); glLinkProgram(prog);
        GLint ok = 0; glGetProgramiv(prog, GL_LINK_STATUS, &ok); char log[1024] = ""; glGetProgramInfoLog(prog, sizeof log, NULL, log);
        LOG("program link %s %s\n", ok ? "ok" : "FAILED", log);
        if (!ok) prog = 0;
    }
    /* vertex data: three vertices, stride 8 (x, y floats); a 64 KB constants window with the block header at row 3 */
    GLuint vbo, ubo, vao, tex;
    float verts[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
    glGenBuffers(1, &vbo); glBindBuffer(GL_SHADER_STORAGE_BUFFER, vbo); glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof verts, verts, GL_STATIC_DRAW);
    static float kwin[4096 * 4];
    { unsigned *h = (unsigned *)&kwin[3 * 4]; h[0] = 8; h[1] = 0; h[2] = 0; h[3] = 0; }   /* stride 8, base 0 */
    kwin[4 * 4 + 0] = 1; kwin[4 * 4 + 1] = 1; kwin[4 * 4 + 2] = 1; kwin[4 * 4 + 3] = 1;   /* the position scale */
    kwin[5 * 4 + 0] = 1; kwin[5 * 4 + 1] = 0.5f; kwin[5 * 4 + 2] = 0; kwin[5 * 4 + 3] = 1; /* the colour */
    glGenBuffers(1, &ubo); glBindBuffer(GL_UNIFORM_BUFFER, ubo); glBufferData(GL_UNIFORM_BUFFER, sizeof kwin, kwin, GL_DYNAMIC_DRAW);
    glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    unsigned white = 0xFFFFFFFFu;
    glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex); glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    GLenum err = glGetError(); LOG("GL error after setup: 0x%x\n", err);

    unsigned frame = 0; double t0 = now_ms(), t_first = t0, worst = 0, last = t0;
    unsigned char px[4] = {0};
    while (appletMainLoop() && frame < 600){
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
        float f = (float)(frame % 120) / 120.0f;
        glViewport(0, 0, 1280, 720);
        glClearColor(0.1f + 0.6f * f, 0.2f, 0.5f - 0.4f * f, 1.0f); glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        if (prog){
            glUseProgram(prog);
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, vbo); glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo);
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex);
            glUniform1i(glGetUniformLocation(prog, "kb"), 3);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
        if (frame == 5) glReadPixels(640, 360, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);   /* the triangle's middle */
        eglSwapBuffers(d, sf);
        double t = now_ms(); if (t - last > worst) worst = t - last; last = t;
        frame++;
    }
    double dt = now_ms() - t0;
    LOG("frames %u in %.0f ms (%.1f fps), slowest %.1f ms\n", frame, dt, frame * 1000.0 / dt, worst);
    LOG("pixel at the triangle's middle: %u %u %u %u (255,128,0 expected: the vertex-pulling shader drew)\n", px[0], px[1], px[2], px[3]);
    LOG("GL error at the end: 0x%x\ndone\n", glGetError());
    (void)t_first;
    if (g_out) fclose(g_out);
    eglMakeCurrent(d, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(d, cx); eglDestroySurface(d, sf); eglTerminate(d);
    return 0;
}
