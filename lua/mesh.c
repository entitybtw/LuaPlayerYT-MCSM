/*
 * Mesh: 3D triangles drawn by the GE between the sprites of glib2d.
 *
 *   Mesh.load(path)                 a .msh file (below)
 *   Mesh.new(vertices, [indices])   vertices: { {x, y, z, [color], [u], [v]}, ... }
 *                                   (color: a Color, white if left out; u, v:
 *                                   0..1 over the picture), indices: a list of
 *                                   1-based vertex numbers, 3 per triangle (left
 *                                   out: the vertices are the triangles)
 *   Mesh.unload(mesh)
 *
 *   Mesh.camera{fov = 60, x, y, z, pitch, yaw, roll, near = 0.5, far = 1000}
 *       The camera looks along -z when its angles are 0. Angles in degrees.
 *   Mesh.fog(near, far, color) / Mesh.fog()   distance fog on / off
 *   Mesh.cull("none" | "cw" | "ccw")          the faces that are not drawn
 *   Mesh.clearDepth()                         once per frame, before the meshes
 *
 *   Mesh.draw(mesh, {x, y, z, pitch, yaw, roll, scale | sx, sy, sz,
 *                    tint = Color, alpha = 0..255, texture = Image,
 *                    linear = false, repeat = false, depth = true})
 *       The colours of the vertices are the material, the tint is the light
 *       that falls on it; a texture is multiplied with both.
 *
 * A .msh file (little endian): "MSH1", u32 vertex count, u32 index count,
 * u32 flags (1: the vertices have u, v), then the vertices as the GE takes
 * them: [float u, v] u32 colour (ABGR), float x, y, z; then the indices, u16.
 */

#include <malloc.h>
#include <math.h>
#include <string.h>
#include <stdio.h>
#include <pspgu.h>
#include <pspgum.h>
#include <pspkernel.h>

#include "common.h"
#include "graphics.h"
#include "../libs/g2d/glib2d.h"

#define MESH_UV 1

typedef struct {
    void *verts;
    u16 *indices;
    int vcount, icount, flags;
} Mesh;

g2dColor *toColor(lua_State *L, int index);

static float cam_fov = 60.f, cam_near = 0.5f, cam_far = 1000.f;
static ScePspFVector3 cam_pos = { 0, 0, 0 }, cam_rot = { 0, 0, 0 };
static bool fog_on = false;
static float fog_near, fog_far;
static u32 fog_color;
static int cull_mode = 0;   // 0: none, 1: clockwise faces, 2: counter-clockwise

static const float DEG = 3.14159265f / 180.f;

static int vertexSize(int flags) {
    return ((flags & MESH_UV) ? 8 : 0) + 4 + 12;
}

static Mesh *checkMesh(lua_State *L, int index) {
    Mesh **m = (Mesh **)luaL_checkudata(L, index, "Mesh");
    if (!*m) luaL_error(L, "Mesh: the mesh is unloaded");
    return *m;
}

static void freeMesh(Mesh *m) {
    if (!m) return;
    free(m->verts);
    free(m->indices);
    free(m);
}

static Mesh *allocMesh(int vcount, int icount, int flags) {
    Mesh *m = (Mesh *)calloc(1, sizeof(Mesh));
    if (!m) return NULL;
    m->vcount = vcount;
    m->icount = icount;
    m->flags = flags;
    m->verts = memalign(16, vcount * vertexSize(flags));
    m->indices = (u16 *)memalign(16, icount * sizeof(u16) + 16);
    if (!m->verts || !m->indices) {
        freeMesh(m);
        return NULL;
    }
    return m;
}

static int pushMesh(lua_State *L, Mesh *m) {
    // The GE reads the buffers from the memory, not from the cache.
    sceKernelDcacheWritebackRange(m->verts, m->vcount * vertexSize(m->flags));
    sceKernelDcacheWritebackRange(m->indices, m->icount * sizeof(u16));
    Mesh **ud = (Mesh **)lua_newuserdata(L, sizeof(Mesh *));
    *ud = m;
    luaL_getmetatable(L, "Mesh");
    lua_setmetatable(L, -2);
    return 1;
}

static int MESH_load(lua_State *L) {
    const char *path = luaL_checkstring(L, 1);
    FILE *f = fopen(path, "rb");
    if (!f) return luaL_error(L, "Mesh.load: can't open \"%s\"", path);

    char magic[4];
    u32 head[3];
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "MSH1", 4) != 0 ||
        fread(head, 4, 3, f) != 3 || head[0] == 0 || head[0] > 65536 || head[1] % 3) {
        fclose(f);
        return luaL_error(L, "Mesh.load: \"%s\" is not a mesh", path);
    }
    Mesh *m = allocMesh(head[0], head[1], head[2]);
    if (!m) {
        fclose(f);
        return luaL_error(L, "Mesh.load: not enough memory for \"%s\"", path);
    }
    size_t vbytes = m->vcount * vertexSize(m->flags);
    if (fread(m->verts, 1, vbytes, f) != vbytes ||
        fread(m->indices, sizeof(u16), m->icount, f) != (size_t)m->icount) {
        fclose(f);
        freeMesh(m);
        return luaL_error(L, "Mesh.load: can't read \"%s\"", path);
    }
    fclose(f);
    for (int i = 0; i < m->icount; i++) {
        if (m->indices[i] >= m->vcount) {
            freeMesh(m);
            return luaL_error(L, "Mesh.load: a bad index in \"%s\"", path);
        }
    }
    return pushMesh(L, m);
}

static float fieldNumber(lua_State *L, int table, int index, float def) {
    lua_rawgeti(L, table, index);
    float v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static int MESH_new(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    int vcount = lua_objlen(L, 1);
    if (vcount <= 0 || vcount > 65536)
        return luaL_error(L, "Mesh.new: 1 to 65536 vertices");
    bool hasIndices = lua_istable(L, 2);
    int icount = hasIndices ? (int)lua_objlen(L, 2) : vcount;
    if (icount <= 0 || icount % 3)
        return luaL_error(L, "Mesh.new: the triangles need 3 vertices each");

    // The mesh has u, v when the first vertex has them.
    lua_rawgeti(L, 1, 1);
    if (!lua_istable(L, -1)) return luaL_error(L, "Mesh.new: a vertex is a table");
    lua_rawgeti(L, -1, 5);
    int flags = lua_isnumber(L, -1) ? MESH_UV : 0;
    lua_pop(L, 2);

    Mesh *m = allocMesh(vcount, icount, flags);
    if (!m) return luaL_error(L, "Mesh.new: not enough memory");

    u8 *p = (u8 *)m->verts;
    for (int i = 0; i < vcount; i++) {
        lua_rawgeti(L, 1, i + 1);
        int t = lua_gettop(L);
        if (!lua_istable(L, t)) {
            freeMesh(m);
            return luaL_error(L, "Mesh.new: vertex %d is not a table", i + 1);
        }
        if (flags & MESH_UV) {
            float uv[2] = { fieldNumber(L, t, 5, 0), fieldNumber(L, t, 6, 0) };
            memcpy(p, uv, 8);
            p += 8;
        }
        lua_rawgeti(L, t, 4);
        u32 color = lua_isuserdata(L, -1) ? *toColor(L, -1) : 0xFFFFFFFF;
        lua_pop(L, 1);
        memcpy(p, &color, 4);
        p += 4;
        float xyz[3] = { fieldNumber(L, t, 1, 0), fieldNumber(L, t, 2, 0), fieldNumber(L, t, 3, 0) };
        memcpy(p, xyz, 12);
        p += 12;
        lua_pop(L, 1);
    }
    for (int i = 0; i < icount; i++) {
        int n = i + 1;
        if (hasIndices) {
            lua_rawgeti(L, 2, i + 1);
            n = lua_tointeger(L, -1);
            lua_pop(L, 1);
        }
        if (n < 1 || n > vcount) {
            freeMesh(m);
            return luaL_error(L, "Mesh.new: index %d is out of the vertices", i + 1);
        }
        m->indices[i] = n - 1;
    }
    return pushMesh(L, m);
}

// A mesh unloaded while a frame is open may still be read by the GE when the
// frame is drawn: it is freed after g2dFlip, which waits for the GE.
static Mesh **pending = NULL;
static int pending_count = 0, pending_size = 0;

static void freePending(void) {
    for (int i = 0; i < pending_count; i++) freeMesh(pending[i]);
    pending_count = 0;
}

static int MESH_unload(lua_State *L) {
    Mesh **m = (Mesh **)luaL_checkudata(L, 1, "Mesh");
    if (!*m) return 0;
    if (g2dFrameOpen()) {
        if (pending_count == pending_size) {
            int size = pending_size ? pending_size * 2 : 8;
            Mesh **grown = (Mesh **)realloc(pending, size * sizeof(Mesh *));
            if (!grown) return luaL_error(L, "Mesh.unload: no memory");
            pending = grown;
            pending_size = size;
        }
        pending[pending_count++] = *m;
    } else {
        freeMesh(*m);
    }
    *m = NULL;
    return 0;
}

static float optField(lua_State *L, int table, const char *name, float def) {
    if (table == 0) return def;
    lua_getfield(L, table, name);
    float v = lua_isnumber(L, -1) ? lua_tonumber(L, -1) : def;
    lua_pop(L, 1);
    return v;
}

static int MESH_camera(lua_State *L) {
    luaL_checktype(L, 1, LUA_TTABLE);
    cam_fov = optField(L, 1, "fov", 60);
    cam_pos.x = optField(L, 1, "x", 0);
    cam_pos.y = optField(L, 1, "y", 0);
    cam_pos.z = optField(L, 1, "z", 0);
    cam_rot.x = optField(L, 1, "pitch", 0) * DEG;
    cam_rot.y = optField(L, 1, "yaw", 0) * DEG;
    cam_rot.z = optField(L, 1, "roll", 0) * DEG;
    cam_near = optField(L, 1, "near", 0.5f);
    cam_far = optField(L, 1, "far", 1000);
    if (cam_near <= 0 || cam_far <= cam_near)
        return luaL_error(L, "Mesh.camera: 0 < near < far");
    return 0;
}

static int MESH_fog(lua_State *L) {
    if (lua_gettop(L) < 3) {
        fog_on = false;
        return 0;
    }
    fog_near = luaL_checknumber(L, 1);
    fog_far = luaL_checknumber(L, 2);
    fog_color = *toColor(L, 3);
    fog_on = true;
    return 0;
}

static int MESH_cull(lua_State *L) {
    const char *mode = luaL_optstring(L, 1, "none");
    if (strcmp(mode, "cw") == 0) cull_mode = 1;
    else if (strcmp(mode, "ccw") == 0) cull_mode = 2;
    else if (strcmp(mode, "none") == 0) cull_mode = 0;
    else return luaL_error(L, "Mesh.cull: \"none\", \"cw\" or \"ccw\"");
    return 0;
}

static int MESH_clearDepth(lua_State *L) {
    g2dBegin3D();
    // glib2d keeps the depth range reversed (near is 65535): far is 0.
    sceGuClearDepth(0);
    sceGuClear(GU_DEPTH_BUFFER_BIT | GU_FAST_CLEAR_BIT);
    sceGuClearDepth(65535);
    return 0;
}

static int MESH_draw(lua_State *L) {
    Mesh *m = checkMesh(L, 1);
    int o = lua_istable(L, 2) ? 2 : 0;

    u32 tint = 0xFFFFFFFF;
    g2dImage *tex = NULL;
    bool linear = false, repeat = false, depth = true;
    if (o) {
        lua_getfield(L, o, "tint");
        if (lua_isuserdata(L, -1)) tint = *toColor(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, o, "texture");
        if (!lua_isnil(L, -1)) tex = GRAPHICS_imageArg(L, lua_gettop(L));
        lua_pop(L, 1);
        lua_getfield(L, o, "linear");
        linear = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, o, "repeat");
        repeat = lua_toboolean(L, -1);
        lua_pop(L, 1);
        lua_getfield(L, o, "depth");
        depth = lua_isnil(L, -1) ? true : lua_toboolean(L, -1);
        lua_pop(L, 1);
    }
    int alpha = (int)optField(L, o, "alpha", 255);
    if (alpha <= 0) return 0;
    if (alpha > 255) alpha = 255;
    tint = (tint & 0x00FFFFFF) | ((((tint >> 24) * alpha) / 255) << 24);

    float scale = optField(L, o, "scale", 1);
    ScePspFVector3 pos = { optField(L, o, "x", 0), optField(L, o, "y", 0), optField(L, o, "z", 0) };
    ScePspFVector3 size = { optField(L, o, "sx", scale), optField(L, o, "sy", scale), optField(L, o, "sz", scale) };
    float pitch = optField(L, o, "pitch", 0) * DEG, yaw = optField(L, o, "yaw", 0) * DEG,
          roll = optField(L, o, "roll", 0) * DEG;

    g2dBegin3D();

    sceGumMatrixMode(GU_PROJECTION);
    sceGumLoadIdentity();
    sceGumPerspective(cam_fov, 480.f / 272.f, cam_near, cam_far);

    // The view is the camera undone: its turn backwards, then its place.
    sceGumMatrixMode(GU_VIEW);
    sceGumLoadIdentity();
    sceGumRotateZ(-cam_rot.z);
    sceGumRotateX(-cam_rot.x);
    sceGumRotateY(-cam_rot.y);
    ScePspFVector3 back = { -cam_pos.x, -cam_pos.y, -cam_pos.z };
    sceGumTranslate(&back);

    sceGumMatrixMode(GU_MODEL);
    sceGumLoadIdentity();
    sceGumTranslate(&pos);
    sceGumRotateY(yaw);
    sceGumRotateX(pitch);
    sceGumRotateZ(roll);
    sceGumScale(&size);

    g2dBindTexture(tex, linear, repeat);
    if (tex) {
        // u, v of 0..1 cover the picture, not the power-of-two texture.
        sceGuTexScale((float)tex->w / tex->tw, (float)tex->h / tex->th);
        sceGuTexOffset(0, 0);
    }
    sceGuEnable(GU_BLEND);
    sceGuEnable(GU_CLIP_PLANES);
    if (depth) {
        sceGuEnable(GU_DEPTH_TEST);
        sceGuDepthFunc(GU_GEQUAL);
        sceGuDepthMask(GU_FALSE);
    } else {
        sceGuDisable(GU_DEPTH_TEST);
    }
    if (cull_mode) {
        sceGuFrontFace(cull_mode == 1 ? GU_CCW : GU_CW);
        sceGuEnable(GU_CULL_FACE);
    } else {
        sceGuDisable(GU_CULL_FACE);
    }

    // No lights: the colour is the vertex colour times the ambient light.
    sceGuEnable(GU_LIGHTING);
    sceGuDisable(GU_LIGHT0);
    sceGuDisable(GU_LIGHT1);
    sceGuDisable(GU_LIGHT2);
    sceGuDisable(GU_LIGHT3);
    sceGuColorMaterial(GU_AMBIENT);
    sceGuAmbient(tint);

    if (fog_on) {
        sceGuFog(fog_near, fog_far, fog_color);
        sceGuEnable(GU_FOG);
    } else {
        sceGuDisable(GU_FOG);
    }

    int type = GU_COLOR_8888 | GU_VERTEX_32BITF | GU_INDEX_16BIT | GU_TRANSFORM_3D;
    if (m->flags & MESH_UV) type |= GU_TEXTURE_32BITF;
    sceGumDrawArray(GU_TRIANGLES, type, m->icount, m->indices, m->verts);

    g2dEnd3D();
    return 0;
}

static const luaL_Reg MESH_methods[] = {
    {"load",       MESH_load},
    {"new",        MESH_new},
    {"unload",     MESH_unload},
    {"camera",     MESH_camera},
    {"fog",        MESH_fog},
    {"cull",       MESH_cull},
    {"clearDepth", MESH_clearDepth},
    {"draw",       MESH_draw},
    {0, 0}
};

int MESH_init(lua_State *L) {
    g2dOnFrameEnd(freePending);
    luaL_newmetatable(L, "Mesh");
    lua_pushcfunction(L, MESH_unload);
    lua_setfield(L, -2, "__gc");
    lua_pop(L, 1);
    luaL_register(L, "Mesh", MESH_methods);
    lua_pop(L, 1);
    return 0;
}
