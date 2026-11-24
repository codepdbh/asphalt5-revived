/* GLES 1.1 bridge: forwards the engine's GL calls to the Android driver.
 *
 * Calling convention is AAPCS softfp: float arguments arrive as bit patterns
 * in core registers, the fifth and later words on the guest stack.  Client
 * array and texel pointers are guest offsets and become host pointers into
 * g_mem, which stays mapped for the whole process.
 *
 * The engine lays out its UI on a fixed logical canvas (854x480 for 3.4.1).
 * Viewport and scissor rectangles are scaled to the real surface and
 * centred, keeping the original aspect ratio.
 */
#include "runtime.h"

#include <EGL/egl.h>
#include <GLES/gl.h>
#include <GLES/glext.h>

#define A0 (cpu->r[0])
#define A1 (cpu->r[1])
#define A2 (cpu->r[2])
#define A3 (cpu->r[3])
#define STK(i) rd32(cpu->r[13] + 4u * (i))
#define F(i) f32_from_bits(cpu->r[i])

static float g_scale = 1.0f;
static int g_offset_x, g_offset_y, g_view_w = 854, g_view_h = 480;
static int g_logical_w = 854, g_logical_h = 480;
static bool g_scissor_enabled;
static GLuint g_array_buffer, g_element_buffer;

static PFNGLCURRENTPALETTEMATRIXOESPROC p_current_palette;
static PFNGLMATRIXINDEXPOINTEROESPROC p_matrix_index_pointer;
static PFNGLWEIGHTPOINTEROESPROC p_weight_pointer;
static bool g_palette_checked;
/* Last rectangles the engine set, in logical units.  The engine sets its
 * viewport once during Asphalt5Renderer.nativeInit, before Android reports
 * the surface size, so they are re-applied whenever the surface changes. */
static GLint g_guest_viewport[4] = {0, 0, 854, 480};
static GLint g_guest_scissor[4] = {0, 0, 854, 480};
static bool g_viewport_set, g_scissor_set;

static void scale_rect(GLint x, GLint y, GLsizei w, GLsizei h, GLint out[4]);

void a5_gles_set_surface(int width, int height, int logical_width, int logical_height)
{
    g_logical_w = logical_width;
    g_logical_h = logical_height;
    float sx = (float)width / (float)logical_width;
    float sy = (float)height / (float)logical_height;
    g_scale = sx < sy ? sx : sy;
    g_view_w = (int)(logical_width * g_scale + 0.5f);
    g_view_h = (int)(logical_height * g_scale + 0.5f);
    g_offset_x = (width - g_view_w) / 2;
    g_offset_y = (height - g_view_h) / 2;
    A5_INFO("surface %dx%d, logical %dx%d, scale %.3f, offset %d,%d",
            width, height, logical_width, logical_height, g_scale, g_offset_x, g_offset_y);
    GLint r[4];
    if (g_viewport_set) {
        scale_rect(g_guest_viewport[0], g_guest_viewport[1], g_guest_viewport[2], g_guest_viewport[3], r);
        glViewport(r[0], r[1], r[2], r[3]);
    }
    if (g_scissor_set) {
        scale_rect(g_guest_scissor[0], g_guest_scissor[1], g_guest_scissor[2], g_guest_scissor[3], r);
        glScissor(r[0], r[1], r[2], r[3]);
    }
}

void a5_gles_begin_frame(void)
{
    /* Black letterbox bars in every buffer; the engine's state is restored. */
    if (!g_offset_x && !g_offset_y) return;
    GLfloat color[4];
    glGetFloatv(GL_COLOR_CLEAR_VALUE, color);
    if (g_scissor_enabled) glDisable(GL_SCISSOR_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(color[0], color[1], color[2], color[3]);
    if (g_scissor_enabled) glEnable(GL_SCISSOR_TEST);
}

void a5_gles_map_touch(float x, float y, int *gx, int *gy)
{
    /* Touch Y grows downwards like the engine's screen coordinates. */
    float lx = (x - (float)g_offset_x) / g_scale;
    float ly = (y - (float)g_offset_y) / g_scale;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    if (lx > g_logical_w - 1) lx = (float)(g_logical_w - 1);
    if (ly > g_logical_h - 1) ly = (float)(g_logical_h - 1);
    *gx = (int)lx;
    *gy = (int)ly;
}

static void scale_rect(GLint x, GLint y, GLsizei w, GLsizei h, GLint out[4])
{
    out[0] = g_offset_x + (GLint)(x * g_scale + 0.5f);
    out[1] = g_offset_y + (GLint)(y * g_scale + 0.5f);
    out[2] = (GLsizei)(w * g_scale + 0.5f);
    out[3] = (GLsizei)(h * g_scale + 0.5f);
}

static const void *ptr_or_offset(CPU *cpu, uint32_t address, bool buffer_bound, uint32_t min_len)
{
    if (buffer_bound || !address) return (const void *)(uintptr_t)address;
    if (!a5_guest_range(address, min_len)) guest_abort(cpu, "GL pointer 0x%08x outside guest memory", address);
    return G(address);
}

static const GLfloat *floats(CPU *cpu, uint32_t address, uint32_t count)
{
    if (!a5_guest_range(address, count * 4)) guest_abort(cpu, "GL float array 0x%08x", address);
    return (const GLfloat *)G(address);
}

static void load_palette(void)
{
    if (g_palette_checked) return;
    g_palette_checked = true;
    p_current_palette = (PFNGLCURRENTPALETTEMATRIXOESPROC)eglGetProcAddress("glCurrentPaletteMatrixOES");
    p_matrix_index_pointer = (PFNGLMATRIXINDEXPOINTEROESPROC)eglGetProcAddress("glMatrixIndexPointerOES");
    p_weight_pointer = (PFNGLWEIGHTPOINTEROESPROC)eglGetProcAddress("glWeightPointerOES");
    if (!p_current_palette || !p_matrix_index_pointer || !p_weight_pointer) {
        A5_WARN("driver lacks GL_OES_matrix_palette: skinned meshes will not deform");
    }
}

/* ---- state ---------------------------------------------------------- */

/* The main menu's 3D showroom uses a hard-coded 738x442 viewport (854x480
 * layout minus its side panel and top bar), set by the function returning to
 * 0x000fb3bc / 0x000fb3d4 in the audited 3.4.1 engine.  On wider canvases the
 * panel moves right with the screen edge, so the showroom is widened to fill
 * the gap and its projection is narrowed by the same factor (no stretching). */
#define MENU_VIEWPORT_LR 0x000fb3bcu
#define MENU_SCISSOR_LR  0x000fb3d4u
static bool g_menu_view;
static float g_menu_xscale = 1.0f;
static GLenum g_matrix_mode = GL_MODELVIEW;

static GLsizei widen_menu(CPU *cpu, uint32_t lr)
{
    if (cpu->r[14] != lr || A0 || A1 || A2 != 738 || A3 != 442 || g_logical_w <= 854) return (GLsizei)A2;
    return (GLsizei)(738 + (g_logical_w - 854));
}

static void gl_Viewport(CPU *cpu)
{
    GLint r[4];
    GLsizei width = widen_menu(cpu, MENU_VIEWPORT_LR);
    g_menu_view = width != (GLsizei)A2;
    g_menu_xscale = g_menu_view ? 738.0f / (float)width : 1.0f;
    g_viewport_set = true;
    g_guest_viewport[0] = (GLint)A0;
    g_guest_viewport[1] = (GLint)A1;
    g_guest_viewport[2] = width;
    g_guest_viewport[3] = (GLint)A3;
    scale_rect((GLint)A0, (GLint)A1, width, (GLsizei)A3, r);
    glViewport(r[0], r[1], r[2], r[3]);
}

static void gl_Scissor(CPU *cpu)
{
    GLint r[4];
    GLsizei width = widen_menu(cpu, MENU_SCISSOR_LR);
    g_scissor_set = true;
    g_guest_scissor[0] = (GLint)A0;
    g_guest_scissor[1] = (GLint)A1;
    g_guest_scissor[2] = width;
    g_guest_scissor[3] = (GLint)A3;
    scale_rect((GLint)A0, (GLint)A1, width, (GLsizei)A3, r);
    glScissor(r[0], r[1], r[2], r[3]);
}

static void gl_Clear(CPU *cpu)
{
    /* Keep the letterbox bars black: limit unscissored clears to the canvas. */
    if (!g_scissor_enabled && (g_offset_x || g_offset_y)) {
        GLint box[4];
        glGetIntegerv(GL_SCISSOR_BOX, box);
        glEnable(GL_SCISSOR_TEST);
        glScissor(g_offset_x, g_offset_y, g_view_w, g_view_h);
        glClear((GLbitfield)A0);
        glScissor(box[0], box[1], box[2], box[3]);
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    glClear((GLbitfield)A0);
}

static void gl_Enable(CPU *cpu)
{
    if (A0 == GL_SCISSOR_TEST) g_scissor_enabled = true;
    glEnable((GLenum)A0);
}

static void gl_Disable(CPU *cpu)
{
    if (A0 == GL_SCISSOR_TEST) g_scissor_enabled = false;
    glDisable((GLenum)A0);
}

static void gl_ClearColor(CPU *cpu)
{
    /* 2D screens clear to white and cover an 854-wide area with bitmaps.  On
     * wider canvases the uncovered strip would show that white, so it becomes
     * black, matching letterboxing.  3D scenes draw their own background. */
    if (g_logical_w > 854 && F(0) >= 0.99f && F(1) >= 0.99f && F(2) >= 0.99f) {
        glClearColor(0, 0, 0, F(3));
        return;
    }
    glClearColor(F(0), F(1), F(2), F(3));
}
static void gl_ClearDepthf(CPU *cpu) { glClearDepthf(F(0)); }
static void gl_Color4f(CPU *cpu) { glColor4f(F(0), F(1), F(2), F(3)); }
static void gl_Color4ub(CPU *cpu) { glColor4ub((GLubyte)A0, (GLubyte)A1, (GLubyte)A2, (GLubyte)A3); }
static void gl_ColorMask(CPU *cpu) { glColorMask((GLboolean)A0, (GLboolean)A1, (GLboolean)A2, (GLboolean)A3); }
static void gl_AlphaFunc(CPU *cpu) { glAlphaFunc((GLenum)A0, F(1)); }
static void gl_BlendFunc(CPU *cpu) { glBlendFunc((GLenum)A0, (GLenum)A1); }
static void gl_CullFace(CPU *cpu) { glCullFace((GLenum)A0); }
static void gl_FrontFace(CPU *cpu) { glFrontFace((GLenum)A0); }
static void gl_DepthFunc(CPU *cpu) { glDepthFunc((GLenum)A0); }
static void gl_DepthMask(CPU *cpu) { glDepthMask((GLboolean)A0); }
static void gl_ShadeModel(CPU *cpu) { glShadeModel((GLenum)A0); }
static void gl_Hint(CPU *cpu) { glHint((GLenum)A0, (GLenum)A1); }
static void gl_PixelStorei(CPU *cpu) { glPixelStorei((GLenum)A0, (GLint)A1); }
static void gl_Finish(CPU *cpu) { (void)cpu; glFinish(); }
static void gl_EnableClientState(CPU *cpu) { glEnableClientState((GLenum)A0); }
static void gl_DisableClientState(CPU *cpu) { glDisableClientState((GLenum)A0); }
static void gl_ActiveTexture(CPU *cpu) { glActiveTexture((GLenum)A0); }
static void gl_ClientActiveTexture(CPU *cpu) { glClientActiveTexture((GLenum)A0); }
static void gl_Fogf(CPU *cpu) { glFogf((GLenum)A0, F(1)); }
static void gl_Fogfv(CPU *cpu) { glFogfv((GLenum)A0, floats(cpu, A1, A0 == GL_FOG_COLOR ? 4 : 1)); }
static void gl_Lightf(CPU *cpu) { glLightf((GLenum)A0, (GLenum)A1, F(2)); }
static void gl_Lightfv(CPU *cpu) { glLightfv((GLenum)A0, (GLenum)A1, floats(cpu, A2, 4)); }
static void gl_LightModelfv(CPU *cpu) { glLightModelfv((GLenum)A0, floats(cpu, A1, 4)); }
static void gl_Materialf(CPU *cpu) { glMaterialf((GLenum)A0, (GLenum)A1, F(2)); }
static void gl_Materialfv(CPU *cpu) { glMaterialfv((GLenum)A0, (GLenum)A1, floats(cpu, A2, 4)); }
static void gl_TexEnvi(CPU *cpu) { glTexEnvi((GLenum)A0, (GLenum)A1, (GLint)A2); }
static void gl_TexEnvfv(CPU *cpu) { glTexEnvfv((GLenum)A0, (GLenum)A1, floats(cpu, A2, 4)); }

/* ---- matrices ------------------------------------------------------- */

static void gl_MatrixMode(CPU *cpu) { g_matrix_mode = (GLenum)A0; glMatrixMode((GLenum)A0); }
static void gl_LoadIdentity(CPU *cpu) { (void)cpu; glLoadIdentity(); }
static void gl_PushMatrix(CPU *cpu) { (void)cpu; glPushMatrix(); }
static void gl_PopMatrix(CPU *cpu) { (void)cpu; glPopMatrix(); }
static void gl_LoadMatrixf(CPU *cpu)
{
    const GLfloat *m = floats(cpu, A0, 16);
    if (g_menu_view && g_matrix_mode == GL_PROJECTION) {
        GLfloat p[16];
        memcpy(p, m, sizeof(p));
        p[0] *= g_menu_xscale;   /* first row: clip-space x */
        p[4] *= g_menu_xscale;
        p[8] *= g_menu_xscale;
        p[12] *= g_menu_xscale;
        glLoadMatrixf(p);
        return;
    }
    glLoadMatrixf(m);
}
static void gl_Translatef(CPU *cpu) { glTranslatef(F(0), F(1), F(2)); }
static void gl_Rotatef(CPU *cpu) { glRotatef(F(0), F(1), F(2), F(3)); }

static void gl_CurrentPaletteMatrixOES(CPU *cpu)
{
    load_palette();
    if (p_current_palette) p_current_palette((GLuint)A0);
}

/* ---- arrays and draws ----------------------------------------------- */

static void gl_BindBuffer(CPU *cpu)
{
    if (A0 == GL_ARRAY_BUFFER) g_array_buffer = A1;
    else if (A0 == GL_ELEMENT_ARRAY_BUFFER) g_element_buffer = A1;
    glBindBuffer((GLenum)A0, (GLuint)A1);
}

static void gl_VertexPointer(CPU *cpu)
{
    glVertexPointer((GLint)A0, (GLenum)A1, (GLsizei)A2, ptr_or_offset(cpu, A3, g_array_buffer, 4));
}

static void gl_NormalPointer(CPU *cpu)
{
    glNormalPointer((GLenum)A0, (GLsizei)A1, ptr_or_offset(cpu, A2, g_array_buffer, 4));
}

static void gl_ColorPointer(CPU *cpu)
{
    glColorPointer((GLint)A0, (GLenum)A1, (GLsizei)A2, ptr_or_offset(cpu, A3, g_array_buffer, 4));
}

static void gl_TexCoordPointer(CPU *cpu)
{
    glTexCoordPointer((GLint)A0, (GLenum)A1, (GLsizei)A2, ptr_or_offset(cpu, A3, g_array_buffer, 4));
}

static void gl_MatrixIndexPointerOES(CPU *cpu)
{
    load_palette();
    if (p_matrix_index_pointer) {
        p_matrix_index_pointer((GLint)A0, (GLenum)A1, (GLsizei)A2, ptr_or_offset(cpu, A3, g_array_buffer, 1));
    }
}

static void gl_WeightPointerOES(CPU *cpu)
{
    load_palette();
    if (p_weight_pointer) {
        p_weight_pointer((GLint)A0, (GLenum)A1, (GLsizei)A2, ptr_or_offset(cpu, A3, g_array_buffer, 4));
    }
}

static void gl_DrawArrays(CPU *cpu) { glDrawArrays((GLenum)A0, (GLint)A1, (GLsizei)A2); }

static void gl_DrawElements(CPU *cpu)
{
    uint32_t index_size = A2 == GL_UNSIGNED_BYTE ? 1 : 2;
    glDrawElements((GLenum)A0, (GLsizei)A1, (GLenum)A2,
                   ptr_or_offset(cpu, A3, g_element_buffer, (uint32_t)A1 * index_size));
}

/* ---- textures ------------------------------------------------------- */

static void gl_BindTexture(CPU *cpu) { glBindTexture((GLenum)A0, (GLuint)A1); }
static void gl_TexParameteri(CPU *cpu) { glTexParameteri((GLenum)A0, (GLenum)A1, (GLint)A2); }
static void gl_TexParameterf(CPU *cpu) { glTexParameterf((GLenum)A0, (GLenum)A1, F(2)); }

static void gl_GenTextures(CPU *cpu)
{
    if (!a5_guest_range(A1, A0 * 4)) guest_abort(cpu, "glGenTextures target");
    glGenTextures((GLsizei)A0, (GLuint *)G(A1));
}

static void gl_DeleteTextures(CPU *cpu)
{
    if (!a5_guest_range(A1, A0 * 4)) guest_abort(cpu, "glDeleteTextures source");
    glDeleteTextures((GLsizei)A0, (const GLuint *)G(A1));
}

static void gl_TexImage2D(CPU *cpu)
{
    /* target, level, internalformat, width | height, border, format, type, pixels */
    uint32_t pixels = STK(4);
    const void *data = NULL;
    if (pixels) {
        if (!a5_guest_range(pixels, 1)) guest_abort(cpu, "glTexImage2D pixels 0x%08x", pixels);
        data = G(pixels);
    }
    glTexImage2D((GLenum)A0, (GLint)A1, (GLint)A2, (GLsizei)A3, (GLsizei)STK(0), (GLint)STK(1),
                 (GLenum)STK(2), (GLenum)STK(3), data);
}

static void gl_TexSubImage2D(CPU *cpu)
{
    /* target, level, xoffset, yoffset | width, height, format, type, pixels */
    uint32_t pixels = STK(4);
    if (!a5_guest_range(pixels, 1)) guest_abort(cpu, "glTexSubImage2D pixels 0x%08x", pixels);
    glTexSubImage2D((GLenum)A0, (GLint)A1, (GLint)A2, (GLint)A3, (GLsizei)STK(0), (GLsizei)STK(1),
                    (GLenum)STK(2), (GLenum)STK(3), G(pixels));
}

static void gl_CopyTexImage2D(CPU *cpu)
{
    /* target, level, internalformat, x | y, width, height, border.
     * The source origin follows the scaled canvas; the copied size stays the
     * texture size the engine asked for. */
    static bool logged;
    GLint r[4];
    scale_rect((GLint)A3, (GLint)STK(0), (GLsizei)STK(1), (GLsizei)STK(2), r);
    if (!logged) {
        logged = true;
        A5_INFO("glCopyTexImage2D %dx%d used (canvas scale %.2f)", (int)STK(1), (int)STK(2), g_scale);
    }
    glCopyTexImage2D((GLenum)A0, (GLint)A1, (GLenum)A2, r[0], r[1], (GLsizei)STK(1),
                     (GLsizei)STK(2), (GLint)STK(3));
}

static void gl_CompressedTexImage2D(CPU *cpu) { a5_upload_compressed_texture(cpu); }

/* ---- queries -------------------------------------------------------- */

static void gl_GetError(CPU *cpu) { cpu->r[0] = glGetError(); }

static void gl_GetFloatv(CPU *cpu)
{
    if (!a5_guest_range(A1, 64)) guest_abort(cpu, "glGetFloatv target 0x%08x", A1);
    glGetFloatv((GLenum)A0, (GLfloat *)G(A1));
}

static void gl_GetString(CPU *cpu)
{
    static struct { GLenum name; uint32_t guest; } cache[8];
    for (size_t i = 0; i < 8 && cache[i].guest; ++i) {
        if (cache[i].name == (GLenum)A0) { cpu->r[0] = cache[i].guest; return; }
    }
    const char *value = (const char *)glGetString((GLenum)A0);
    char text[8192];
    if (!value) { cpu->r[0] = 0; return; }
    if (A0 == GL_EXTENSIONS && !strstr(value, "GL_IMG_texture_compression_pvrtc")) {
        /* PVRTC1 is decoded on upload (texture_upload.cpp). */
        snprintf(text, sizeof(text), "%s GL_IMG_texture_compression_pvrtc", value);
    } else {
        snprintf(text, sizeof(text), "%s", value);
    }
    uint32_t guest = guest_alloc(strlen(text) + 1);
    memcpy(G(guest), text, strlen(text) + 1);
    for (size_t i = 0; i < 8; ++i) {
        if (!cache[i].guest) { cache[i].name = (GLenum)A0; cache[i].guest = guest; break; }
    }
    cpu->r[0] = guest;
}

/* ---- registry ------------------------------------------------------- */

typedef struct { const char *name; GuestFn fn; } GlEntry;

#define E(n) {"gl" #n, gl_##n}
static const GlEntry k_gl[] = {
    E(ActiveTexture), E(AlphaFunc), E(BindBuffer), E(BindTexture), E(BlendFunc), E(Clear),
    E(ClearColor), E(ClearDepthf), E(ClientActiveTexture), E(Color4f), E(Color4ub), E(ColorMask),
    E(ColorPointer), E(CompressedTexImage2D), E(CopyTexImage2D), E(CullFace),
    E(CurrentPaletteMatrixOES), E(DeleteTextures), E(DepthFunc), E(DepthMask), E(Disable),
    E(DisableClientState), E(DrawArrays), E(DrawElements), E(Enable), E(EnableClientState),
    E(Finish), E(Fogf), E(Fogfv), E(FrontFace), E(GenTextures), E(GetError), E(GetFloatv),
    E(GetString), E(Hint), E(LightModelfv), E(Lightf), E(Lightfv), E(LoadIdentity),
    E(LoadMatrixf), E(Materialf), E(Materialfv), E(MatrixIndexPointerOES), E(MatrixMode),
    E(NormalPointer), E(PixelStorei), E(PopMatrix), E(PushMatrix), E(Rotatef), E(Scissor),
    E(ShadeModel), E(TexCoordPointer), E(TexEnvfv), E(TexEnvi), E(TexImage2D),
    E(TexParameterf), E(TexParameteri), E(TexSubImage2D), E(Translatef), E(VertexPointer),
    E(Viewport), E(WeightPointerOES),
};
#undef E

GuestFn a5_gles_handler(const char *name)
{
    for (size_t i = 0; i < sizeof(k_gl) / sizeof(k_gl[0]); ++i) {
        if (!strcmp(k_gl[i].name, name)) return k_gl[i].fn;
    }
    return NULL;
}

bool a5_gles_dispatch(CPU *cpu, const char *name)
{
    GuestFn fn = a5_gles_handler(name);
    if (fn) fn(cpu);
    return fn != NULL;
}
