// gLib2D by Geecko - A simple, fast, light-weight 2D graphics library.
//
// This work is licensed under the Creative Commons BY-SA 3.0 Unported License.
// See LICENSE for more details.
//
// Please report bugs at : geecko.dev@free.fr

#include "glib2d.h"
#include "../vfpu_math.h"

#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspgu.h>
#include <vram.h>
#include <malloc.h>
#include <math.h>

#define PSP_LINE_SIZE       (512)
#define PIXEL_SIZE          (4)
#define FRAMEBUFFER_SIZE    (PSP_LINE_SIZE*G2D_SCR_H*PIXEL_SIZE)
#define MALLOC_STEP         (128)
#define TRANSFORM_STACK_MAX (64)
#define SLICE_WIDTH         (64.f)
#define M_180_PI            (57.29578f)
#define M_PI_180            (0.017453292f)

#define DEFAULT_SIZE       (10)
#define DEFAULT_COORD_MODE (G2D_UP_LEFT)
#define DEFAULT_X          (0.f)
#define DEFAULT_Y          (0.f)
#define DEFAULT_Z          (0.f)
#define DEFAULT_COLOR      (WHITE)
#define DEFAULT_ALPHA      (0xFF)

#define CURRENT_OBJ obj_list[obj_list_size-1]
#define CURRENT_TRANSFORM transform_stack[transform_stack_size-1]
#define I_OBJ obj_list[i]

typedef enum
{
    RECTS, LINES, QUADS, POINTS
} Obj_Type;

typedef struct
{
    float x, y, z;
    float rot, rot_sin, rot_cos;
    float scale_w, scale_h;
} Transform;

typedef struct
{
    float x, y, z;
    float rot_x, rot_y; // Rotation center
    float rot, rot_sin, rot_cos;
    int crop_x, crop_y;
    int crop_w, crop_h;
    float scale_w, scale_h;
    g2dColor color;
    g2dAlpha alpha;
} Object;


// * Main vars *
static int *list;
static bool init = false, start = false, zclear = true, scissor = false;
static Transform transform_stack[TRANSFORM_STACK_MAX];
static int transform_stack_size;
static float global_scale = 1.f;
// * Object vars *
static Object *obj_list = NULL, obj;
static Obj_Type obj_type;
static int obj_list_size;
static bool obj_begin = false, obj_line_strip;
static bool obj_use_z, obj_use_vert_color, obj_use_blend, obj_use_rot,
obj_use_tex_linear, obj_use_tex_repeat, obj_use_int;
static g2dCoord_Mode obj_coord_mode;
static int obj_colors_count;
static g2dImage *obj_tex;
// The palette of the indexed texture that is drawn at the moment (it is
// uploaded to the GE only when it changes).
static g2dColor *last_clut = NULL;
static int obj_camera_x, obj_camera_y;
static bool obj_use_camera = false;

g2dImage g2d_draw_buffer = { 512, 512, G2D_SCR_W, G2D_SCR_H,
                             (float)G2D_SCR_W / G2D_SCR_H, false, false,
                             (g2dColor *)FRAMEBUFFER_SIZE, G2D_TEX_8888, NULL, 0, 0 },
    g2d_disp_buffer = { 512, 512, G2D_SCR_W, G2D_SCR_H,
                        (float)G2D_SCR_W / G2D_SCR_H, false, false,
                        (g2dColor *)0, G2D_TEX_8888, NULL, 0, 0 };

// * Internal functions *

void _g2dStart() {
    if (!init) g2dInit();

    sceKernelDcacheWritebackAll();
    sceGuStart(GU_DIRECT, list);
    start = true;
}


// Vertex order: [texture uv] [color] [vertex]
void *_g2dSetVertex(void *vp, int i, float vx, float vy) {
    short *v_p_short = vp;
    g2dColor *v_p_color;
    float *v_p_float;

    // Texture
    if (obj_tex != NULL) {
        *(v_p_short++) = I_OBJ.crop_x + vx * I_OBJ.crop_w;
        *(v_p_short++) = I_OBJ.crop_y + vy * I_OBJ.crop_h;
    }

    // Color
    v_p_color = (g2dColor *)v_p_short;

    if (obj_use_vert_color) {
        *(v_p_color++) = I_OBJ.color;
    }

    // Coord
    v_p_float = (float *)v_p_color;

    v_p_float[0] = I_OBJ.x + (obj_type == RECTS ? vx * I_OBJ.scale_w : 0.f);
    v_p_float[1] = I_OBJ.y + (obj_type == RECTS ? vy * I_OBJ.scale_h : 0.f);

    // Then apply the rotation
    if (obj_use_rot && obj_type == RECTS) {
        float tx = v_p_float[0] - I_OBJ.rot_x, ty = v_p_float[1] - I_OBJ.rot_y;
        v_p_float[0] = I_OBJ.rot_x - I_OBJ.rot_sin * ty + I_OBJ.rot_cos * tx,
            v_p_float[1] = I_OBJ.rot_y + I_OBJ.rot_cos * ty + I_OBJ.rot_sin * tx;
    }

    if (obj_use_int) {
        v_p_float[0] = floorf(v_p_float[0]);
        v_p_float[1] = floorf(v_p_float[1]);
    }
    v_p_float[2] = I_OBJ.z;

    v_p_float += 3;

    return (void *)v_p_float;
}

// * Main functions *

void g2dInit() {
    if (init) return;

    // Display list allocation
    list = malloc(512 * 1024);

    // Init & setup GU
    sceGuInit();
    sceGuStart(GU_DIRECT, list);

    sceGuDrawBuffer(GU_PSM_8888, g2d_draw_buffer.data, PSP_LINE_SIZE);
    sceGuDispBuffer(G2D_SCR_W, G2D_SCR_H, g2d_disp_buffer.data, PSP_LINE_SIZE);
    sceGuDepthBuffer((void *)(FRAMEBUFFER_SIZE * 2), PSP_LINE_SIZE);
    sceGuOffset(2048 - (G2D_SCR_W / 2), 2048 - (G2D_SCR_H / 2));
    sceGuViewport(2048, 2048, G2D_SCR_W, G2D_SCR_H);

    g2d_draw_buffer.data = vabsptr(g2d_draw_buffer.data);
    g2d_disp_buffer.data = vabsptr(g2d_disp_buffer.data);

    g2dResetScissor();
    sceGuDepthRange(65535, 0);
    sceGuClearDepth(65535);
    sceGuAlphaFunc(GU_GREATER, 0, 0xff);
    sceGuDepthFunc(GU_LEQUAL);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuShadeModel(GU_SMOOTH);

    sceGuDisable(GU_CULL_FACE);
    sceGuDisable(GU_CLIP_PLANES);
    sceGuDisable(GU_DITHER);
    sceGuEnable(GU_ALPHA_TEST);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuEnable(GU_BLEND);

    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);

    init = true;
}


void g2dTerm() {
    if (!init) return;

    sceGuTerm();

    free(list);

    init = false;
}

void g2dClear(g2dColor color) {
    if (!start) _g2dStart();

    sceGuClearColor(color);
    sceGuClear(GU_COLOR_BUFFER_BIT | GU_FAST_CLEAR_BIT
        | (zclear ? GU_DEPTH_BUFFER_BIT : 0));
    zclear = false;
}


void g2dClearZ() {
    if (!start) _g2dStart();

    sceGuClear(GU_DEPTH_BUFFER_BIT | GU_FAST_CLEAR_BIT);
    zclear = true;
}


void _g2dBeginCommon() {
    if (!start) _g2dStart();

    obj_list_size = 0;
    obj_list = realloc(obj_list, MALLOC_STEP * sizeof(Object));

    obj_use_z = false;
    obj_use_vert_color = false;
    obj_use_blend = false;
    obj_use_rot = false;
    obj_use_int = false;
    obj_colors_count = 0;
    g2dReset();

    obj_begin = true;
}


void g2dBeginRects(g2dImage *tex) {
    if (obj_begin) return;

    obj_type = RECTS;
    obj_tex = tex;
    _g2dBeginCommon();
}


void g2dBeginLines(g2dLine_Mode mode) {
    if (obj_begin) return;

    obj_type = LINES;
    obj_tex = NULL;
    obj_line_strip = (mode & G2D_STRIP);
    _g2dBeginCommon();
}


void g2dBeginQuads(g2dImage *tex) {
    if (obj_begin) return;

    obj_type = QUADS;
    obj_tex = tex;
    _g2dBeginCommon();
}


void g2dBeginPoints() {
    if (obj_begin) return;

    obj_type = POINTS;
    obj_tex = NULL;
    _g2dBeginCommon();
}


void _g2dEndRects() {
    // Define vertices properties
    int prim = (obj_use_rot ? GU_TRIANGLES : GU_SPRITES),
        v_obj_nbr = (obj_use_rot ? 6 : 2),
        v_nbr,
        v_coord_size = 3,
        v_tex_size = (obj_tex != NULL ? 2 : 0),
        v_color_size = (obj_use_vert_color ? 1 : 0),
        v_size = v_tex_size * sizeof(short) +
        v_color_size * sizeof(g2dColor) +
        v_coord_size * sizeof(float),
        v_type = GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        i;

    if (obj_tex != NULL)    v_type |= GU_TEXTURE_16BIT;
    if (obj_use_vert_color) v_type |= GU_COLOR_8888;

    // Count how many vertices to allocate.
    if (obj_tex == NULL || obj_use_rot) // No slicing
    {
        v_nbr = v_obj_nbr * obj_list_size;
    } else // Can use texture slicing for tremendous performance :)
    {
        for (v_nbr = 0, i = 0; i < obj_list_size; i++) {
            v_nbr += v_obj_nbr * ceilf(I_OBJ.crop_w / SLICE_WIDTH);
        }
    }

    // Allocate vertex list memory
    void *v = sceGuGetMemory(v_nbr * v_size), *vi = v;

    // Build the vertex list
    for (i = 0; i < obj_list_size; i += 1) {
        if (!obj_use_rot) {
            if (obj_tex == NULL) {
                vi = _g2dSetVertex(vi, i, 0.f, 0.f);
                vi = _g2dSetVertex(vi, i, 1.f, 1.f);
            } else // Use texture slicing
            {
                float u, step = SLICE_WIDTH / I_OBJ.crop_w;
                for (u = 0.f; u < 1.f; u += step) {
                    vi = _g2dSetVertex(vi, i, u, 0.f);
                    vi = _g2dSetVertex(vi, i, ((u + step) > 1.f ? 1.f : u + step), 1.f);
                }
            }
        } else // Rotation : draw 2 triangles per obj
        {
            vi = _g2dSetVertex(vi, i, 0.f, 0.f);
            vi = _g2dSetVertex(vi, i, 1.f, 0.f);
            vi = _g2dSetVertex(vi, i, 0.f, 1.f);
            vi = _g2dSetVertex(vi, i, 0.f, 1.f);
            vi = _g2dSetVertex(vi, i, 1.f, 0.f);
            vi = _g2dSetVertex(vi, i, 1.f, 1.f);
        }
    }

    // Then put it in the display list.
    sceGuDrawArray(prim, v_type, v_nbr, NULL, v);
}


void _g2dEndLines() {
    // Define vertices properties
    int prim = (obj_line_strip ? GU_LINE_STRIP : GU_LINES),
        v_obj_nbr = (obj_line_strip ? 1 : 2),
        v_nbr = v_obj_nbr * (obj_line_strip ? obj_list_size
            : obj_list_size / 2),
        v_coord_size = 3,
        v_color_size = (obj_use_vert_color ? 1 : 0),
        v_size = v_color_size * sizeof(g2dColor) +
        v_coord_size * sizeof(float),
        v_type = GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        i;

    if (obj_use_vert_color) v_type |= GU_COLOR_8888;

    // Allocate vertex list memory
    void *v = sceGuGetMemory(v_nbr * v_size), *vi = v;

    // Build the vertex list
    if (obj_line_strip) {
        vi = _g2dSetVertex(vi, 0, 0.f, 0.f);
        for (i = 1; i < obj_list_size; i += 1) {
            vi = _g2dSetVertex(vi, i, 0.f, 0.f);
        }
    } else {
        for (i = 0; i + 1 < obj_list_size; i += 2) {
            vi = _g2dSetVertex(vi, i, 0.f, 0.f);
            vi = _g2dSetVertex(vi, i + 1, 0.f, 0.f);
        }
    }

    // Then put it in the display list.
    sceGuDrawArray(prim, v_type, v_nbr, NULL, v);
}


void _g2dEndQuads() {
    // Define vertices properties
    int prim = GU_TRIANGLES,
        v_obj_nbr = 6,
        v_nbr = v_obj_nbr * (obj_list_size / 4),
        v_coord_size = 3,
        v_tex_size = (obj_tex != NULL ? 2 : 0),
        v_color_size = (obj_use_vert_color ? 1 : 0),
        v_size = v_tex_size * sizeof(short) +
        v_color_size * sizeof(g2dColor) +
        v_coord_size * sizeof(float),
        v_type = GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        i;

    if (obj_tex != NULL)    v_type |= GU_TEXTURE_16BIT;
    if (obj_use_vert_color) v_type |= GU_COLOR_8888;

    // Allocate vertex list memory
    void *v = sceGuGetMemory(v_nbr * v_size), *vi = v;

    // Build the vertex list
    for (i = 0; i + 3 < obj_list_size; i += 4) {
        vi = _g2dSetVertex(vi, i, 0.f, 0.f);
        vi = _g2dSetVertex(vi, i + 1, 1.f, 0.f);
        vi = _g2dSetVertex(vi, i + 3, 0.f, 1.f);
        vi = _g2dSetVertex(vi, i + 3, 0.f, 1.f);
        vi = _g2dSetVertex(vi, i + 1, 1.f, 0.f);
        vi = _g2dSetVertex(vi, i + 2, 1.f, 1.f);
    }

    // Then put it in the display list.
    sceGuDrawArray(prim, v_type, v_nbr, NULL, v);
}


void _g2dEndPoints() {
    // Define vertices properties
    int prim = GU_POINTS,
        v_obj_nbr = 1,
        v_nbr = v_obj_nbr * obj_list_size,
        v_coord_size = 3,
        v_color_size = (obj_use_vert_color ? 1 : 0),
        v_size = v_color_size * sizeof(g2dColor) +
        v_coord_size * sizeof(float),
        v_type = GU_VERTEX_32BITF | GU_TRANSFORM_2D,
        i;

    if (obj_use_vert_color) v_type |= GU_COLOR_8888;

    // Allocate vertex list memory
    void *v = sceGuGetMemory(v_nbr * v_size), *vi = v;

    // Build the vertex list
    for (i = 0; i < obj_list_size; i += 1) {
        vi = _g2dSetVertex(vi, i, 0.f, 0.f);
    }

    // Then put it in the display list.
    sceGuDrawArray(prim, v_type, v_nbr, NULL, v);
}


void g2dEnd() {
    if (!obj_begin || obj_list_size <= 0) {
        obj_begin = false;
        return;
    }

    // Manage pspgu extensions
    if (obj_use_z)          sceGuEnable(GU_DEPTH_TEST);
    else                    sceGuDisable(GU_DEPTH_TEST);
    if (obj_use_blend)      sceGuEnable(GU_BLEND);
    else                    sceGuDisable(GU_BLEND);
    if (obj_use_vert_color) sceGuColor(WHITE);
    else                    sceGuColor(obj_list[0].color);
    if (obj_tex == NULL)    sceGuDisable(GU_TEXTURE_2D);
    else {
        sceGuEnable(GU_TEXTURE_2D);
        if (obj_use_tex_linear) sceGuTexFilter(GU_LINEAR, GU_LINEAR);
        else                    sceGuTexFilter(GU_NEAREST_MIPMAP_NEAREST, GU_NEAREST_MIPMAP_NEAREST);
        if (obj_use_tex_repeat) sceGuTexWrap(GU_REPEAT, GU_REPEAT);
        else                    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
        // Load texture
        if (obj_tex->format == G2D_TEX_8888) {
            // A normal texture: the palette of the GE is not used any
            // more, so it has to be uploaded again the next time.
            last_clut = NULL;
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            sceGuTexMode(GU_PSM_8888, 0, 0, obj_tex->swizzled);
            sceGuTexImage(0, obj_tex->tw, obj_tex->th, obj_tex->tw, obj_tex->data);
        } else if (obj_tex->format == G2D_TEX_5650 || obj_tex->format == G2D_TEX_5551) {
            // A 16-bit texture: twice less memory than a 32-bit one.
            // The 16-bit formats of the GE have almost no room for the
            // alpha (in 5650 there is none at all), so the alpha of the
            // texture is not used: the picture is blended with the alpha
            // of the colour of the vertex (the fade of the screens).
            last_clut = NULL;
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGB);
            if (obj_tex->format == G2D_TEX_5650) {
                sceGuTexMode(GU_PSM_5650, 0, 0, obj_tex->swizzled);
            } else {
                sceGuTexMode(GU_PSM_5551, 0, 0, obj_tex->swizzled);
            }
            sceGuTexImage(0, obj_tex->tw, obj_tex->th, obj_tex->tw, obj_tex->data);
        } else {
            // An indexed texture: the palette is uploaded to the GE and
            // the picture stays as the indices of the palette.
            // The palette of the last drawn texture is kept, so it is
            // uploaded only when it really changes (the upload of a
            // palette is not free).
            sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
            if (obj_tex->clut != NULL && obj_tex->clut_size > 0) {
                if (last_clut != obj_tex->clut) {
                    sceGuClutMode(GU_PSM_8888, 0, 0xFF, 0);
                    sceGuClutLoad((obj_tex->clut_size + 7) / 8, obj_tex->clut);
                    last_clut = obj_tex->clut;
                }
            }
            if (obj_tex->format == G2D_TEX_T4) {
                sceGuTexMode(GU_PSM_T4, 0, 0, obj_tex->swizzled);
                // The width of the buffer is in pixels (two pixels in one
                // byte), so it is the same as the width of the texture.
                sceGuTexImage(0, obj_tex->tw, obj_tex->th, obj_tex->tw, obj_tex->data);
            } else {
                sceGuTexMode(GU_PSM_T8, 0, 0, obj_tex->swizzled);
                sceGuTexImage(0, obj_tex->tw, obj_tex->th, obj_tex->tw, obj_tex->data);
            }
        }
    }

    switch (obj_type) {
    case RECTS:  _g2dEndRects();  break;
    case LINES:  _g2dEndLines();  break;
    case QUADS:  _g2dEndQuads();  break;
    case POINTS: _g2dEndPoints(); break;
    }

    sceGuColor(WHITE);
    sceGuEnable(GU_BLEND);

    obj_begin = false;
    if (obj_use_z) zclear = true;
}


void g2dReset() {
    g2dResetCoord();
    g2dResetScale();
    g2dResetColor();
    g2dResetAlpha();
    g2dResetRotation();
    g2dResetCrop();
    g2dResetTex();
}


void g2dFlip(g2dFlip_Mode mode) {
    if (scissor) g2dResetScissor();

    sceGuFinish();
    sceGuSync(0, 0);
    if (mode & G2D_VSYNC) sceDisplayWaitVblankStart();

    g2d_disp_buffer.data = g2d_draw_buffer.data;
    g2d_draw_buffer.data = vabsptr(sceGuSwapBuffers());

    start = false;
}


void g2dAdd() {
    if (!obj_begin) return;
    if (obj.scale_w == 0 || obj.scale_h == 0) return;

    if (!(obj_list_size % MALLOC_STEP)) {
        obj_list = realloc(obj_list, (obj_list_size + MALLOC_STEP) * sizeof(Object));
    }

    obj_list_size++;
    if (obj_use_camera) {
        obj.x -= obj_camera_x;
        obj.y -= obj_camera_y;
    }
    obj.rot_x = obj.x;
    obj.rot_y = obj.y;
    CURRENT_OBJ = obj;

    // Coord mode stuff
    CURRENT_OBJ.x -= (obj_coord_mode == G2D_UP_RIGHT ||
        obj_coord_mode == G2D_DOWN_RIGHT ?
        CURRENT_OBJ.scale_w :
        (obj_coord_mode == G2D_CENTER ?
            CURRENT_OBJ.scale_w / 2 : 0));
    CURRENT_OBJ.y -= (obj_coord_mode == G2D_DOWN_LEFT ||
        obj_coord_mode == G2D_DOWN_RIGHT ?
        CURRENT_OBJ.scale_h :
        (obj_coord_mode == G2D_CENTER ?
            CURRENT_OBJ.scale_h / 2 : 0));

    // Alpha stuff
    CURRENT_OBJ.color = G2D_MODULATE(CURRENT_OBJ.color, 255, obj.alpha);
}


void g2dPush() {
    if (transform_stack_size >= TRANSFORM_STACK_MAX) return;
    transform_stack_size++;
    CURRENT_TRANSFORM.x = obj.x;
    CURRENT_TRANSFORM.y = obj.y;
    CURRENT_TRANSFORM.z = obj.z;
    CURRENT_TRANSFORM.rot = obj.rot;
    CURRENT_TRANSFORM.rot_sin = obj.rot_sin;
    CURRENT_TRANSFORM.rot_cos = obj.rot_cos;
    CURRENT_TRANSFORM.scale_w = obj.scale_w;
    CURRENT_TRANSFORM.scale_h = obj.scale_h;
}


void g2dPop() {
    if (transform_stack_size <= 0) return;
    obj.x = CURRENT_TRANSFORM.x;
    obj.y = CURRENT_TRANSFORM.y;
    obj.z = CURRENT_TRANSFORM.z;
    obj.rot = CURRENT_TRANSFORM.rot;
    obj.rot_sin = CURRENT_TRANSFORM.rot_sin;
    obj.rot_cos = CURRENT_TRANSFORM.rot_cos;
    obj.scale_w = CURRENT_TRANSFORM.scale_w;
    obj.scale_h = CURRENT_TRANSFORM.scale_h;
    if (obj.rot != 0.f) obj_use_rot = true;
    if (obj.z != 0.f) obj_use_z = true;
    transform_stack_size--;
}

// * Coord functions *

void g2dResetCoord() {
    obj_coord_mode = DEFAULT_COORD_MODE;
    obj.x = DEFAULT_X;
    obj.y = DEFAULT_Y;
    obj.z = DEFAULT_Z;
}


void g2dSetCoordMode(g2dCoord_Mode mode) {
    if (mode > G2D_CENTER) return;
    obj_coord_mode = mode;
}


void g2dGetCoordXYZ(float *x, float *y, float *z) {
    if (x != NULL) *x = obj.x;
    if (y != NULL) *y = obj.y;
    if (z != NULL) *z = obj.z;
}


void g2dSetCoordXY(float x, float y) {
    obj.x = x * global_scale;
    obj.y = y * global_scale;
    obj.z = 0.f;
}


void g2dSetCoordXYZ(float x, float y, float z) {
    obj.x = x * global_scale;
    obj.y = y * global_scale;
    obj.z = z * global_scale;
    if (z != 0.f) obj_use_z = true;
}


void g2dSetCoordXYRelative(float x, float y) {
    float inc_x = x, inc_y = y;
    if (obj.rot_cos != 1.f) {
        inc_x = -obj.rot_sin * y + obj.rot_cos * x;
        inc_y = obj.rot_cos * y + obj.rot_sin * x;
    }
    obj.x += inc_x * global_scale;
    obj.y += inc_y * global_scale;
}


void g2dSetCoordXYZRelative(float x, float y, float z) {
    g2dSetCoordXYRelative(x, y);
    obj.z += z * global_scale;
    if (z != 0.f) obj_use_z = true;
}


void g2dSetCoordInteger(bool use) {
    obj_use_int = use;
}

// * Scale functions *

void g2dResetGlobalScale() {
    global_scale = 1.f;
}


void g2dResetScale() {
    if (obj_tex == NULL) {
        obj.scale_w = DEFAULT_SIZE;
        obj.scale_h = DEFAULT_SIZE;
    } else {
        obj.scale_w = obj_tex->w;
        obj.scale_h = obj_tex->h;
    }

    obj.scale_w *= global_scale;
    obj.scale_h *= global_scale;
}


void g2dGetGlobalScale(float *scale) {
    if (scale != NULL) *scale = global_scale;
}


void g2dGetScaleWH(float *w, float *h) {
    if (w != NULL) *w = obj.scale_w;
    if (h != NULL) *h = obj.scale_h;
}


void g2dSetGlobalScale(float scale) {
    global_scale = scale;
}


void g2dSetScale(float w, float h) {
    g2dResetScale();
    g2dSetScaleRelative(w, h);
}


void g2dSetScaleWH(float w, float h) {
    obj.scale_w = w * global_scale;
    obj.scale_h = h * global_scale;
    // A trick to prevent an unexpected behavior when mirroring with GU_SPRITES.
    if (obj.scale_w < 0 || obj.scale_h < 0) obj_use_rot = true;
}


void g2dSetScaleRelative(float w, float h) {
    obj.scale_w *= w;
    obj.scale_h *= h;

    if (obj.scale_w < 0 || obj.scale_h < 0) obj_use_rot = true;
}


void g2dSetScaleWHRelative(float w, float h) {
    obj.scale_w += w * global_scale;
    obj.scale_h += h * global_scale;

    if (obj.scale_w < 0 || obj.scale_h < 0) obj_use_rot = true;
}

// * Color functions *

void g2dResetColor() {
    obj.color = DEFAULT_COLOR;
}


void g2dResetAlpha() {
    obj.alpha = DEFAULT_ALPHA;
}


void g2dGetAlpha(g2dAlpha *alpha) {
    if (alpha != NULL) *alpha = obj.alpha;
}


void g2dSetColor(g2dColor color) {
    obj.color = color;
    if (++obj_colors_count > 1) obj_use_vert_color = true;
    if (G2D_GET_A(obj.color) < 255) obj_use_blend = true;
}


void g2dSetAlpha(g2dAlpha alpha) {
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;
    obj.alpha = alpha;
    if (++obj_colors_count > 1) obj_use_vert_color = true;
    if (obj.alpha < 255) obj_use_blend = true;
}


void g2dSetAlphaRelative(int alpha) {
    g2dSetAlpha(obj.alpha + alpha);
}

// * Rotations functions *

void g2dResetRotation() {
    obj.rot = 0.f;
    obj.rot_sin = 0.f;
    obj.rot_cos = 1.f;
}


void g2dGetRotationRad(float *radians) {
    if (radians != NULL) *radians = obj.rot;
}


void g2dGetRotation(float *degrees) {
    if (degrees != NULL) *degrees = obj.rot * M_180_PI;
}


void g2dSetRotationRad(float radians) {
    if (radians == obj.rot) return;
    obj.rot = radians;
    obj.rot_sin = vfpu_sinf(radians);
    obj.rot_cos = vfpu_cosf(radians);
    if (radians != 0.f) obj_use_rot = true;
}


void g2dSetRotation(float degrees) {
    g2dSetRotationRad(degrees * M_PI_180);
}


void g2dSetRotationRadRelative(float radians) {
    g2dSetRotationRad(obj.rot + radians);
}


void g2dSetRotationRelative(float degrees) {
    g2dSetRotationRadRelative(degrees * M_PI_180);
}

// * Crop functions *

void g2dResetCrop() {
    if (obj_tex == NULL) return;
    obj.crop_x = 0;
    obj.crop_y = 0;
    obj.crop_w = obj_tex->w;
    obj.crop_h = obj_tex->h;
}


void g2dGetCropXY(int *x, int *y) {
    if (obj_tex == NULL) return;
    if (x != NULL) *x = obj.crop_x;
    if (y != NULL) *y = obj.crop_y;
}


void g2dGetCropWH(int *w, int *h) {
    if (obj_tex == NULL) return;
    if (w != NULL) *w = obj.crop_w;
    if (h != NULL) *h = obj.crop_h;
}


void g2dSetCropXY(int x, int y) {
    if (obj_tex == NULL) return;
    obj.crop_x = x;
    obj.crop_y = y;
}


void g2dSetCropWH(int w, int h) {
    if (obj_tex == NULL) return;
    obj.crop_w = w;
    obj.crop_h = h;
}


void g2dSetCropXYRelative(int x, int y) {
    if (obj_tex == NULL) return;
    g2dSetCropXY(obj.crop_x + x, obj.crop_y + y);
}


void g2dSetCropWHRelative(int w, int h) {
    if (obj_tex == NULL) return;
    g2dSetCropWH(obj.crop_w + w, obj.crop_h + h);
}

// * Texture functions *

void g2dResetTex() {
    if (obj_tex == NULL) return;
    obj_use_tex_repeat = false;
    obj_use_tex_linear = false;
    if (obj_tex->can_blend) obj_use_blend = true;
}


void g2dSetTexRepeat(bool use) {
    if (obj_tex == NULL) return;
    obj_use_tex_repeat = use;
}


void g2dSetTexBlend(bool use) {
    if (obj_tex == NULL) return;
    if (!obj_tex->can_blend) return;
    obj_use_blend = use;
}


void g2dSetTexLinear(bool use) {
    if (obj_tex == NULL) return;
    obj_use_tex_linear = use;
}

// * Texture management *

int _getNextPower2(int n) {
    int p = 1;
    while ((p <<= 1) < n);
    return p;
}

void _swizzle(unsigned char *dest, unsigned char *source, int width, int height) {
    int i, j;
    int rowblocks = (width / 16);
    int rowblocks_add = (rowblocks - 1) * 128;
    unsigned int block_address = 0;
    unsigned int *img = (unsigned int *)source;
    for (j = 0; j < height; j++, block_address += 16) {
        unsigned int *block = (unsigned int *)(dest + block_address);
        for (i = 0; i < rowblocks; i++) {
            *block++ = *img++;
            *block++ = *img++;
            *block++ = *img++;
            *block++ = *img++;
            block += 28;
        }
        if ((j & 0x7) == 0x7) block_address += rowblocks_add;
    }
}

// The textures of the GE are stored in blocks of 8×8 pixels, so the height
// of the data is rounded up to a multiple of 8 (the extra rows are not
// drawn, but they have to be there, or the picture is shifted).
int _g2dTexHeight(int h) {
    return (h + 7) & ~7;
}

// Creates a texture with a buffer of the given size. The size of the
// buffer of the indexed and of the 16-bit pictures is much smaller than the
// 32-bit one, so the big buffer is never allocated for them (on the 32 MB
// of the PSP the big buffer would be a hole in the heap of the game, and
// the memory of it is lost until the heap is freed).
static g2dImage *_g2dTexCreateSized(int w, int h, bool can_blend, size_t data_size) {
    g2dImage *tex = malloc(sizeof(g2dImage));
    if (tex == NULL) return NULL;

    tex->tw = _getNextPower2(w);
    tex->th = _getNextPower2(h);
    tex->w = w;
    tex->h = h;
    tex->ratio = (float)w / h;
    tex->swizzled = false;
    tex->can_blend = can_blend;
    tex->format = G2D_TEX_8888;
    tex->clut = NULL;
    tex->clut_size = 0;

    tex->data_size = data_size;
    tex->data = calloc(data_size, 1);
    if (tex->data == NULL) { free(tex); return NULL; }

    return tex;
}

g2dImage *_g2dTexCreate(int w, int h, bool can_blend) {
    g2dImage *tex = malloc(sizeof(g2dImage));
    if (tex == NULL) return NULL;

    tex->tw = _getNextPower2(w);
    tex->th = _getNextPower2(h);
    tex->w = w;
    tex->h = h;
    tex->ratio = (float)w / h;
    tex->swizzled = false;
    tex->can_blend = can_blend;
    tex->format = G2D_TEX_8888;
    tex->clut = NULL;
    tex->clut_size = 0;

    tex->data_size = (size_t)tex->tw * tex->th * sizeof(g2dColor);
    tex->data = malloc(tex->data_size);
    if (tex->data == NULL) { free(tex); return NULL; }
    memset(tex->data, 0, tex->data_size);

    return tex;
}


void g2dTexFree(g2dImage **tex) {
    if (tex == NULL) return;
    if (*tex == NULL) return;
    if ((*tex)->clut != NULL) {
        // The palette of this texture is not in the GE any more, so it has
        // to be uploaded again the next time it is used.
        if (last_clut == (*tex)->clut) last_clut = NULL;
        free((*tex)->clut);
    }
    free((*tex)->data);
    free((*tex));
    *tex = NULL;
}


#ifdef USE_PNG
// Loads a PNG with a palette (an indexed picture). The picture is kept as
// the indices of the palette, so it takes four (T8) or eight (T4) times
// less memory than a 32-bit one.
//
// The PNG is read twice: the first time to get the palette (and to count
// the colours), the second time to get the indices. The palette of the
// texture is built in the format of the GE: a 32-bit colour with the
// alpha in the highest byte.
g2dImage *_g2dTexLoadPNGIndexed(png_structp png_ptr, png_infop info_ptr,
                                 png_uint_32 width, png_uint_32 height,
                                 int bit_depth) {
    png_colorp palette = NULL;
    int palette_size = 0;
    png_get_PLTE(png_ptr, info_ptr, &palette, &palette_size);
    if (palette == NULL || palette_size <= 0) return NULL;

    png_bytep trans_alpha = NULL;
    int num_trans = 0;
    png_get_tRNS(png_ptr, info_ptr, &trans_alpha, &num_trans, NULL);

    int clut_size = palette_size;
    if (clut_size > 256) clut_size = 256;

    // The GE loads the palette by the blocks of 8 colours (32 bytes), so the
    // buffer must hold all the blocks of the palette (a palette of 100
    // colours is 13 blocks, that is 416 bytes, and not 400).
    int clut_blocks = (clut_size + 7) / 8;
    // The memory of the GE needs the 16-byte alignment.
    size_t clut_bytes = (size_t)clut_blocks * 8 * sizeof(g2dColor);
    g2dColor *clut = (g2dColor *)memalign(16, clut_bytes);
    if (clut == NULL) return NULL;
    memset(clut, 0, clut_bytes);

    for (int i = 0; i < clut_size; i++) {
        int a = 255;
        if (trans_alpha != NULL && i < num_trans) a = trans_alpha[i];
        clut[i] = G2D_RGBA(palette[i].red, palette[i].green, palette[i].blue, a);
    }

    // The 4-bit format needs 16 colours in the palette, the 8-bit one
    // needs 256 of them.
    int format = (bit_depth == 4) ? G2D_TEX_T4 : G2D_TEX_T8;

    // The data of an indexed texture is one byte (T8) or one nibble (T4)
    // per pixel, so it is much smaller than the 32-bit data. The 32-bit
    // buffer is never allocated for it (on the 32 MB of the PSP the big
    // buffer would be a hole in the heap of the game).
    //
    // The width of the texture is always a power of two, so it is even and
    // the rows of a 4-bit picture fit exactly into tw/2 bytes.
    int tw = _getNextPower2((int)width);
    int th = _getNextPower2((int)height);
    size_t data_size = (format == G2D_TEX_T4)
        ? (size_t)tw * th / 2
        : (size_t)tw * th;

    g2dImage *tex = _g2dTexCreateSized(width, height, true, data_size);
    if (tex == NULL) { free(clut); return NULL; }
    tex->format = format;
    tex->clut = clut;
    tex->clut_size = clut_size;

    u8 *data = (u8 *)tex->data;

    // The rows of the PNG are read one by one and packed into the texture.
    // libpng expands a 4-bit picture to one byte per pixel (png_set_packing),
    // so both cases are read the same way.
    png_bytep row = (png_bytep)malloc(width > 0 ? width : 1);
    if (row == NULL) {
        // The palette belongs to the texture, g2dTexFree() frees it.
        g2dTexFree(&tex);
        return NULL;
    }

    if (format == G2D_TEX_T8) {
        for (png_uint_32 y = 0; y < height; y++) {
            png_read_row(png_ptr, row, NULL);
            for (png_uint_32 x = 0; x < width; x++) {
                data[x + y * tex->tw] = row[x] & 0xFF;
            }
        }
    } else {
        // Two pixels in one byte: the even one in the low nibble.
        int row_bytes = tex->tw / 2;
        for (png_uint_32 y = 0; y < height; y++) {
            png_read_row(png_ptr, row, NULL);
            for (png_uint_32 x = 0; x < width; x += 2) {
                u8 v = row[x] & 0x0F;
                if (x + 1 < width) v |= (row[x + 1] & 0x0F) << 4;
                data[(x / 2) + y * row_bytes] = v;
            }
        }
    }
    free(row);

    return tex;
}

// Loads a picture without an alpha channel (an RGB one) as a 16-bit
// texture: RGB565 or RGB555 with one bit of the alpha. Such a texture takes
// twice less memory than a 32-bit one.
//
// The colour of the texture is stored in the format of the GE: the lowest
// byte is the red one, the highest byte is the alpha.
g2dImage *_g2dTexLoadPNG16(png_structp png_ptr, png_infop info_ptr,
                            png_uint_32 width, png_uint_32 height,
                            int format) {
    // The data of a 16-bit texture is two bytes per pixel. The 32-bit
    // buffer is never allocated for it (on the 32 MB of the PSP the big
    // buffer would be a hole in the heap of the game).
    size_t data_size = (size_t)_getNextPower2((int)width) *
                       _getNextPower2((int)height) * 2;

    g2dImage *tex = _g2dTexCreateSized(width, height, true, data_size);
    if (tex == NULL) return NULL;

    tex->format = format;

    u16 *data = (u16 *)tex->data;

    u32 *line = (u32 *)malloc((size_t)width * 4);
    if (line == NULL) {
        g2dTexFree(&tex);
        return NULL;
    }

    for (png_uint_32 y = 0; y < height; y++) {
        png_read_row(png_ptr, (u8 *)line, NULL);
        for (png_uint_32 x = 0; x < width; x++) {
            u32 c = line[x];
            int r = c & 0xFF, g = (c >> 8) & 0xFF, b = (c >> 16) & 0xFF, a = (c >> 24) & 0xFF;
            u16 v;
            if (format == G2D_TEX_5650) {
                // The 16-bit formats of the GE store the blue in the
                // highest bits of the word and the red in the lowest ones.
                v = (u16)(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
            } else {
                // R5G5B5A1: the highest bit of the word is the alpha.
                v = (u16)(((b >> 3) << 11) | ((g >> 3) << 6) | (r >> 3) << 1 |
                           (a >= 128 ? 0x8000 : 0));
            }
            data[x + y * tex->tw] = v;
        }
    }
    free(line);

    return tex;
}

g2dImage *_g2dTexLoadPNG(FILE *fp) {
    png_structp png_ptr;
    png_infop info_ptr;
    unsigned int sig_read = 0;
    png_uint_32 width, height;
    int bit_depth, color_type, interlace_type;
    u32 x, y, *line;
    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    png_set_error_fn(png_ptr, NULL, NULL, NULL);
    info_ptr = png_create_info_struct(png_ptr);
    png_init_io(png_ptr, fp);
    png_set_sig_bytes(png_ptr, sig_read);
    png_read_info(png_ptr, info_ptr);
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type,
        &interlace_type, NULL, NULL);
    png_set_strip_16(png_ptr);
    png_set_packing(png_ptr);

    // A picture with a palette is kept as it is (as the indices of the
    // palette), so it takes much less memory than a 32-bit one.
    if (color_type == PNG_COLOR_TYPE_PALETTE && (bit_depth == 4 || bit_depth == 8) &&
        !interlace_type) {
        g2dImage *tex = _g2dTexLoadPNGIndexed(png_ptr, info_ptr, width, height, bit_depth);
        if (tex != NULL) {
            png_read_end(png_ptr, info_ptr);
            png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
            return tex;
        }
        // The indexed picture could not be read: it is read again as a
        // normal one (the state of the decoder is restored below).
        png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
        png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
        png_set_error_fn(png_ptr, NULL, NULL, NULL);
        info_ptr = png_create_info_struct(png_ptr);
        png_init_io(png_ptr, fp);
        png_set_sig_bytes(png_ptr, sig_read);
        png_read_info(png_ptr, info_ptr);
        png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type,
            &interlace_type, NULL, NULL);
        png_set_strip_16(png_ptr);
        png_set_packing(png_ptr);
    }

    // A picture without a transparency is loaded as a 16-bit one (twice
    // less memory). A picture with a transparency keeps the 32-bit format,
    // because the 16-bit ones have almost no room for the alpha.
    if (color_type == PNG_COLOR_TYPE_RGB && !interlace_type) {
        // The rows of an RGB picture are three bytes per pixel, so they
        // must be expanded to four bytes (the alpha is added) before they
        // are read.
        png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
        g2dImage *tex = _g2dTexLoadPNG16(png_ptr, info_ptr, width, height,
                                         G2D_TEX_5650);
        if (tex != NULL) {
            png_read_end(png_ptr, info_ptr);
            png_destroy_read_struct(&png_ptr, &info_ptr, NULL);
            return tex;
        }
    }

    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);
    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);
    png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);
    g2dImage *tex = _g2dTexCreate(width, height, true);
    line = malloc(width * 4);
    for (y = 0; y < height; y++) {
        png_read_row(png_ptr, (u8 *)line, NULL);
        for (x = 0; x < width; x++) {
            u32 color = line[x];
            tex->data[x + y * tex->tw] = color;
        }
    }
    free(line);
    png_read_end(png_ptr, info_ptr);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    return tex;
}

void readData(png_structp png_ptr, png_bytep data, png_size_t length) {
    CIMGContext *ctx = (CIMGContext *)png_get_io_ptr(png_ptr);

    if (ctx->pos + length > ctx->size) {
        png_error(png_ptr, "Read beyond end of data");
    }

    memcpy(data, ctx->data + ctx->pos, length);
    ctx->pos += length;
}

g2dImage *_g2dTexLoadPNGfromC(const unsigned char *data, size_t size) {
    png_structp png_ptr;
    png_infop info_ptr;
    png_uint_32 width, height;
    int bit_depth, color_type, interlace_type;
    u32 x, y, *line;

    png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png_ptr) return NULL;

    info_ptr = png_create_info_struct(png_ptr);
    if (!info_ptr) {
        png_destroy_read_struct(&png_ptr, NULL, NULL);
        return NULL;
    }

    CIMGContext ctx = { data, size, 0 };
    png_set_read_fn(png_ptr, &ctx, readData);
    png_set_sig_bytes(png_ptr, 0);
    png_read_info(png_ptr, info_ptr);
    png_get_IHDR(png_ptr, info_ptr, &width, &height, &bit_depth, &color_type, &interlace_type, NULL, NULL);
    png_set_strip_16(png_ptr);
    png_set_packing(png_ptr);
    if (color_type == PNG_COLOR_TYPE_PALETTE)
        png_set_palette_to_rgb(png_ptr);

    if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
        png_set_tRNS_to_alpha(png_ptr);

    png_set_filler(png_ptr, 0xff, PNG_FILLER_AFTER);

    g2dImage *tex = _g2dTexCreate(width, height, true);
    line = malloc(width * 4);
    for (y = 0; y < height; y++) {
        png_read_row(png_ptr, (u8 *)line, NULL);
        for (x = 0; x < width; x++) {
            u32 color = line[x];
            tex->data[x + y * tex->tw] = color;
        }
    }
    free(line);
    png_read_end(png_ptr, info_ptr);
    png_destroy_read_struct(&png_ptr, &info_ptr, NULL);

    return tex;
}
#endif

#ifdef USE_JPEG
g2dImage *_g2dTexLoadJPEG(FILE *fp) {
    struct jpeg_decompress_struct dinfo;
    struct jpeg_error_mgr jerr;
    dinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&dinfo);
    jpeg_stdio_src(&dinfo, fp);
    jpeg_read_header(&dinfo, TRUE);
    int width = dinfo.image_width;
    int height = dinfo.image_height;
    jpeg_start_decompress(&dinfo);
    g2dImage *tex = _g2dTexCreate(width, height, false);
    u8 *line = (u8 *)malloc(width * 3);
    if (dinfo.jpeg_color_space == JCS_GRAYSCALE) {
        while (dinfo.output_scanline < dinfo.output_height) {
            int y = dinfo.output_scanline, x;
            jpeg_read_scanlines(&dinfo, &line, 1);
            for (x = 0; x < width; x++) {
                g2dColor c = line[x];
                tex->data[x + tex->tw * y] = c | (c << 8) | (c << 16) | 0xff000000;
            }
        }
    } else {
        while (dinfo.output_scanline < dinfo.output_height) {
            int y = dinfo.output_scanline, x;
            jpeg_read_scanlines(&dinfo, &line, 1);
            u8 *linePointer = line;
            for (x = 0; x < width; x++) {
                g2dColor c = *(linePointer++);
                c |= (*(linePointer++)) << 8;
                c |= (*(linePointer++)) << 16;
                tex->data[x + tex->tw * y] = c | 0xff000000;
            }
        }
    }
    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);
    free(line);

    return tex;
}

void _jpegReadFromMemory(j_decompress_ptr cinfo, CIMGContext *ctx) {
    struct jpeg_source_mgr *src = cinfo->src;

    if (ctx->pos >= ctx->size) {
        // EOF
        src->next_input_byte = NULL;
        src->bytes_in_buffer = 0;
    } else {
        src->next_input_byte = ctx->data + ctx->pos;
        src->bytes_in_buffer = ctx->size - ctx->pos;
        ctx->pos = ctx->size;
    }
}

g2dImage *_g2dTexLoadJPEGfromC(const unsigned char *data, size_t size) {
    struct jpeg_decompress_struct dinfo;
    struct jpeg_error_mgr jerr;

    dinfo.err = jpeg_std_error(&jerr);
    jpeg_create_decompress(&dinfo);
    jpeg_mem_src(&dinfo, (unsigned char *)data, size);
    jpeg_read_header(&dinfo, TRUE);

    int width = dinfo.image_width;
    int height = dinfo.image_height;

    jpeg_start_decompress(&dinfo);

    g2dImage *tex = _g2dTexCreate(width, height, false);
    if (!tex) {
        jpeg_destroy_decompress(&dinfo);
        return NULL;
    }

    u8 *line = (u8 *)malloc(width * 3);
    if (!line) {
        g2dTexFree(&tex);
        jpeg_destroy_decompress(&dinfo);
        return NULL;
    }

    if (dinfo.jpeg_color_space == JCS_GRAYSCALE) {
        while (dinfo.output_scanline < dinfo.output_height) {
            int y = dinfo.output_scanline;
            jpeg_read_scanlines(&dinfo, &line, 1);

            for (int x = 0; x < width; x++) {
                g2dColor c = line[x];
                tex->data[x + tex->tw * y] = c | (c << 8) | (c << 16) | 0xff000000;
            }
        }
    } else {
        while (dinfo.output_scanline < dinfo.output_height) {
            int y = dinfo.output_scanline;
            jpeg_read_scanlines(&dinfo, &line, 1);
            u8 *linePointer = line;

            for (int x = 0; x < width; x++) {
                g2dColor c = *(linePointer++);
                c |= (*(linePointer++)) << 8;
                c |= (*(linePointer++)) << 16;
                tex->data[x + tex->tw * y] = c | 0xff000000;
            }
        }
    }

    free(line);
    jpeg_finish_decompress(&dinfo);
    jpeg_destroy_decompress(&dinfo);

    return tex;
}
#endif

#ifdef USE_BMP
g2dImage *_g2dTexLoadBMP(FILE *fp) {
    BMPHeader header;
    fread(&header, sizeof(BMPHeader), 1, fp);

    if (header.signature != 0x4D42)
        return NULL;


    if (header.bitsPerPixel != 24 && header.bitsPerPixel != 32)
        return NULL;


    g2dImage *tex = _g2dTexCreate(header.width, abs(header.height), true);
    if (!tex) return NULL;

    fseek(fp, header.dataOffset, SEEK_SET);

    int rowSize = ((header.width * header.bitsPerPixel + 31) / 32) * 4;
    u8 *rowData = malloc(rowSize);
    int y;

    for (y = 0; y < abs(header.height); y++) {
        fread(rowData, 1, rowSize, fp);
        u8 *pixel = rowData;
        int x;

        for (x = 0; x < header.width; x++) {
            u32 color = (pixel[0] << 16) | (pixel[1] << 8) | pixel[2] | 0xFF000000;
            if (header.bitsPerPixel == 32)
                pixel += 4;
            else // 24-bit
                pixel += 3;

            int targetY = (header.height > 0) ? (header.height - 1 - y) : y;
            tex->data[x + targetY * tex->tw] = color;
        }
    }

    free(rowData);
    return tex;
}

g2dImage *_g2dTexLoadBMPfromC(const unsigned char *data, size_t size) {
    CIMGContext ctx = { data, size, 0 };

    if (ctx.size - ctx.pos < 54) return NULL;

    u16 signature = *(u16 *)(ctx.data + ctx.pos);
    ctx.pos += 2;
    if (signature != 0x4D42) return NULL;

    ctx.pos += 8;

    u32 dataOffset = *(u32 *)(ctx.data + ctx.pos);
    ctx.pos += 4;

    u32 headerSize = *(u32 *)(ctx.data + ctx.pos);
    ctx.pos += 4;
    if (headerSize < 40) return NULL;

    u32 width = *(u32 *)(ctx.data + ctx.pos);
    ctx.pos += 4;
    u32 height = *(u32 *)(ctx.data + ctx.pos);
    ctx.pos += 4;

    u16 planes = *(u16 *)(ctx.data + ctx.pos);
    ctx.pos += 2;
    if (planes != 1) return NULL;

    u16 bitsPerPixel = *(u16 *)(ctx.data + ctx.pos);
    ctx.pos += 2;
    if (bitsPerPixel != 24 && bitsPerPixel != 32) return NULL;

    ctx.pos = 54;

    g2dImage *tex = _g2dTexCreate(width, abs(height), true);
    if (!tex) return NULL;

    ctx.pos = dataOffset;

    int rowSize = ((width * bitsPerPixel + 31) / 32) * 4;
    int y;

    for (y = 0; y < abs(height); y++) {
        if (ctx.pos + rowSize > ctx.size) {
            g2dTexFree(&tex);
            return NULL;
        }

        const u8 *rowData = ctx.data + ctx.pos;
        ctx.pos += rowSize;

        const u8 *pixel = rowData;
        int x;

        for (x = 0; x < width; x++) {
            u32 color = (pixel[0] << 16) | (pixel[1] << 8) | pixel[2] | 0xFF000000;
            if (bitsPerPixel == 32)
                pixel += 4;
            else // 24-bit
                pixel += 3;

            int targetY = (height > 0) ? (height - 1 - y) : y;
            tex->data[x + targetY * tex->tw] = color;
        }
    }

    return tex;
}
#endif

#ifdef USE_TGA
g2dImage *_g2dTexLoadTGA(FILE *fp) {
    TGAHeader header;
    if (fread(&header, sizeof(TGAHeader), 1, fp) != 1)  return NULL;

    if (header.imageType != 2 && header.imageType != 10) return NULL;

    if (header.bpp != 24 && header.bpp != 32) return NULL;

    fseek(fp, header.idLength, SEEK_CUR);

    g2dImage *tex = _g2dTexCreate(header.width, header.height, true);
    if (!tex) return NULL;

    int pixelSize = header.bpp / 8;
    int imageSize = header.width * header.height * pixelSize;
    u8 *imageData = malloc(imageSize);
    if (!imageData) {
        g2dTexFree(&tex);
        return NULL;
    }

    if (header.imageType == 2)
        fread(imageData, 1, imageSize, fp);
    else if (header.imageType == 10) { // RLE-сжатие
        u8 *ptr = imageData;
        int remaining = imageSize;

        while (remaining > 0) {
            u8 chunkHeader;
            if (fread(&chunkHeader, 1, 1, fp) != 1) break;

            int chunkSize = (chunkHeader & 0x7F) + 1;
            int chunkBytes = chunkSize * pixelSize;

            if (chunkHeader & 0x80) { // RLE-пакет
                u8 pixel[4];
                if (fread(pixel, 1, pixelSize, fp) != pixelSize) break;

                for (int i = 0; i < chunkSize; i++) {
                    if (remaining < pixelSize) break;
                    memcpy(ptr, pixel, pixelSize);
                    ptr += pixelSize;
                    remaining -= pixelSize;
                }
            } else { // RAW-пакет
                int readBytes = chunkBytes < remaining ? chunkBytes : remaining;
                if (fread(ptr, 1, readBytes, fp) != readBytes) break;
                ptr += readBytes;
                remaining -= readBytes;
            }
        }
    }

    // Обработка пиксельных данных
    u8 *pixel = imageData;
    for (int y = 0; y < header.height; y++) {
        int targetY = (header.descriptor & 0x20) ? y : (header.height - 1 - y);

        for (int x = 0; x < header.width; x++) {
            u32 color;
            if (header.bpp == 32)
                color = pixel[2] | (pixel[1] << 8) | (pixel[0] << 16) | (pixel[3] << 24);
            else // 24-bit
                color = pixel[2] | (pixel[1] << 8) | (pixel[0] << 16) | 0xFF000000;

            tex->data[x + targetY * tex->tw] = color;
            pixel += pixelSize;
        }
    }

    free(imageData);
    return tex;
}

g2dImage *_g2dTexLoadTGAfromC(const unsigned char *data, size_t size) {
    CIMGContext ctx = { data, size, 0 };

    if (ctx.size - ctx.pos < 18) return NULL;

    u8 idLength = ctx.data[ctx.pos++];
    u8 imageType = ctx.data[ctx.pos++];

    ctx.pos += 5;

    u16 width = *(u16 *)(ctx.data + ctx.pos);
    ctx.pos += 2;
    u16 height = *(u16 *)(ctx.data + ctx.pos);
    ctx.pos += 2;

    u8 bpp = ctx.data[ctx.pos++];
    u8 descriptor = ctx.data[ctx.pos++];

    if (imageType != 2 && imageType != 10) return NULL;

    if (bpp != 24 && bpp != 32) return NULL;

    ctx.pos += idLength;

    g2dImage *tex = _g2dTexCreate(width, height, true);
    if (!tex) return NULL;

    int pixelSize = bpp / 8;
    int imageSize = width * height * pixelSize;

    if (imageType == 2) {
        if (ctx.pos + imageSize > ctx.size) {
            g2dTexFree(&tex);
            return NULL;
        }

        const u8 *imageData = ctx.data + ctx.pos;
        ctx.pos += imageSize;

        const u8 *pixel = imageData;
        for (int y = 0; y < height; y++) {
            int targetY = (descriptor & 0x20) ? y : (height - 1 - y);

            for (int x = 0; x < width; x++) {
                u32 color;
                if (bpp == 32)
                    color = pixel[2] | (pixel[1] << 8) | (pixel[0] << 16) | (pixel[3] << 24);
                else // 24-bit
                    color = pixel[2] | (pixel[1] << 8) | (pixel[0] << 16) | 0xFF000000;

                tex->data[x + targetY * tex->tw] = color;
                pixel += pixelSize;
            }
        }
    } else if (imageType == 10) {
        u8 *imageData = malloc(imageSize);
        if (!imageData) {
            g2dTexFree(&tex);
            return NULL;
        }

        u8 *ptr = imageData;
        int remaining = imageSize;

        while (remaining > 0 && ctx.pos < ctx.size) {
            if (ctx.pos >= ctx.size) break;

            u8 chunkHeader = ctx.data[ctx.pos++];
            int chunkSize = (chunkHeader & 0x7F) + 1;
            int chunkBytes = chunkSize * pixelSize;

            if (chunkHeader & 0x80) {
                if (ctx.pos + pixelSize > ctx.size) break;

                u8 pixel[4];
                memcpy(pixel, ctx.data + ctx.pos, pixelSize);
                ctx.pos += pixelSize;

                for (int i = 0; i < chunkSize; i++) {
                    if (remaining < pixelSize) break;
                    memcpy(ptr, pixel, pixelSize);
                    ptr += pixelSize;
                    remaining -= pixelSize;
                }
            } else {
                int readBytes = (chunkBytes < remaining) ? chunkBytes : remaining;
                if (ctx.pos + readBytes > ctx.size) break;

                memcpy(ptr, ctx.data + ctx.pos, readBytes);
                ptr += readBytes;
                ctx.pos += readBytes;
                remaining -= readBytes;
            }
        }


        u8 *pixel = imageData;
        for (int y = 0; y < height; y++) {
            int targetY = (descriptor & 0x20) ? y : (height - 1 - y);

            for (int x = 0; x < width; x++) {
                u32 color;
                if (bpp == 32)
                    color = pixel[2] | (pixel[1] << 8) | (pixel[0] << 16) | (pixel[3] << 24);
                else  // 24-bit
                    color = pixel[2] | (pixel[1] << 8) | (pixel[0] << 16) | 0xFF000000;

                tex->data[x + targetY * tex->tw] = color;
                pixel += pixelSize;
            }
        }

        free(imageData);
    }

    return tex;
}
#endif

g2dImage *g2dTexLoad(char path[], unsigned char *data, size_t size, g2dTex_Mode mode) {
    if (path == NULL && data == NULL && size == 0) return NULL;

    g2dImage *tex = NULL;

    if (path == NULL) {
        if (size >= 8 && memcmp(data, "\x89PNG\r\n\x1a\n", 8) == 0) {
#ifdef USE_PNG
            tex = _g2dTexLoadPNGfromC(data, size);
#endif
        } else if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) {
#ifdef USE_JPEG
            tex = _g2dTexLoadJPEGfromC(data, size);
#endif
        } else if (size >= 2 && data[0] == 0x42 && data[1] == 0x4D) {
#ifdef USE_BMP
            tex = _g2dTexLoadBMPfromC(data, size);
#endif
        } else if (size >= 18) {
            // Check for TGA (no magic number, check fields)
            u8 imageType = data[2];
            if (imageType == 2 || imageType == 10) { // Uncompressed true-color
#ifdef USE_TGA
                tex = _g2dTexLoadTGAfromC(data, size);
#endif
            }
        }
    } else {
        FILE *fp = fopen(path, "rb");
        if (fp == NULL) return NULL;

        const char *ext = strrchr(path, '.');
        if (ext != NULL) {
            ext++; // Skip the dot
#ifdef USE_PNG
            if (strcasecmp(ext, "png") == 0) {
                tex = _g2dTexLoadPNG(fp);
            }
#endif

#ifdef USE_JPEG
            if (strcasecmp(ext, "jpg") == 0 || strcasecmp(ext, "jpeg") == 0) {
                tex = _g2dTexLoadJPEG(fp);
            }
#endif

#ifdef USE_BMP
            if (strcasecmp(ext, "bmp") == 0) {
                tex = _g2dTexLoadBMP(fp);
            }
#endif

#ifdef USE_TGA
            if (strcasecmp(ext, "tga") == 0) {
                tex = _g2dTexLoadTGA(fp);
            }
#endif
        }

        fclose(fp);
    }

    if (tex == NULL) return NULL;

    if (tex->data == NULL) {
        g2dTexFree(&tex);
        return NULL;
    }

    if (tex->w > 512 || tex->h > 512) {
        g2dTexFree(&tex);
        return NULL;
    }

    sceKernelDcacheWritebackAll();

    if ((mode & G2D_SWIZZLE) && (tex->w >= 16 || tex->h >= 16) &&
        tex->format == G2D_TEX_8888) {
        u8 *tmp = malloc(tex->tw * tex->th * PIXEL_SIZE);
        _swizzle(tmp, (u8 *)tex->data, tex->tw * PIXEL_SIZE, tex->th);
        free(tex->data);
        tex->data = (g2dColor *)tmp;
        tex->swizzled = true;
        sceKernelDcacheWritebackAll();
    } else tex->swizzled = false;

    return tex;
}

g2dImage *g2dTexCreatePlaceholder() {
    // Create a 64x64 image with alpha blending support
    g2dImage *tex = _g2dTexCreate(64, 64, true);
    if (tex == NULL) return NULL;

    // Define the colors
    g2dColor dark_gray = DARKGRAY;    // 0xFF3F3F3F
    g2dColor light_gray = LITEGRAY;   // 0xFFBFBFBF

    // Size of each checkerboard square
    int square_size = 8;

    // Fill the texture with checkerboard pattern
    for (int y = 0; y < tex->h; y++) {
        for (int x = 0; x < tex->w; x++) {
            // Determine which square we're in
            int square_x = x / square_size;
            int square_y = y / square_size;

            // Use alternating colors based on square position
            // Creates a chessboard pattern
            if ((square_x + square_y) % 2 == 0) {
                tex->data[x + y * tex->tw] = dark_gray;
            } else {
                tex->data[x + y * tex->tw] = light_gray;
            }
        }
    }

    // Write back to cache for performance
    sceKernelDcacheWritebackAll();

    return tex;
}

// * Scissor functions *

void g2dResetScissor() {
    g2dSetScissor(0, 0, G2D_SCR_W, G2D_SCR_H);
    scissor = false;
}


void g2dSetScissor(int x, int y, int w, int h) {
    sceGuScissor(x, y, w, h);
    scissor = true;
}

void set_pixel(g2dImage *tex, int x, int y, g2dColor color) {
    if (x >= 0 && x < tex->w && y >= 0 && y < tex->h) {
        tex->data[x + y * tex->tw] = color;
    }
}

g2dColor get_pixel(g2dImage *tex, int x, int y) {
    if (x >= 0 && x < tex->w && y >= 0 && y < tex->h) {
        return tex->data[x + y * tex->tw];
    }
    return 0; // или какое-то значение по умолчанию
}

void draw_circle(g2dImage *tex, int x, int y, int w, int h, g2dColor color) {
    for (int i = 0; i < w; i++) {
        set_pixel(tex, x + i, y, color);         // верхняя линия
        set_pixel(tex, x + i, y + h - 1, color); // нижняя линия
    }
    for (int i = 0; i < h; i++) {
        set_pixel(tex, x, y + i, color);         // левая линия
        set_pixel(tex, x + w - 1, y + i, color); // правая линия
    }
    sceKernelDcacheWritebackAll();
}

// * Camera *

void g2dSetUseCamera(bool use) {
    obj_use_camera = use;
}

bool g2dGetUseCamera() {
    return obj_use_camera;
}

void g2dSetCameraXY(int x, int y) {
    obj_camera_x = x;
    obj_camera_y = y;
}

int g2dGetCameraX() { return obj_camera_x; }

int g2dGetCameraY() { return obj_camera_y; }

// EOF
