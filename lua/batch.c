#include "batch.h"

extern g2dImage **toG2D(lua_State *L, int index);
// extern g2dColor *toColor(lua_State *L, int index);

static int L_addDraw(lua_State *L) {
    int args = lua_gettop(L);
    if (args != 3 && args != 5 && args != 9)
        return luaL_error(L, "Batch.addDraw(img, x, y, [w, h], [srcx, srcy, srcw, srch]) takes 3, 5 or 9 arguments");

    g2dImage **p = toG2D(L, 1);
    g2dImage *img = p ? *p : NULL;
    // Исключаем добавление nil/освобождённого кадра вместо краша.
    if (!img)
        return 0;
    int x = luaL_checkint(L, 2), y = luaL_checkint(L, 3);
    int w = luaL_optint(L, 4, img->w), h = luaL_optint(L, 5, img->h);
    int srcx = luaL_optint(L, 6, 0), srcy = luaL_optint(L, 7, 0);
    int srcw = luaL_optint(L, 8, img->w), srch = luaL_optint(L, 9, img->h);

    int idx = BatchAddDraw(img, x, y, w, h, srcx, srcy, srcw, srch);
    if (idx == -1)
        return luaL_error(L, "Batch.addDraw() - failed to create index, reached %d elements", BATCH_MAX);
    lua_pushinteger(L, idx);
    return 1;
}

static int L_render(lua_State *L) {
    if (!BatchRender())
        return luaL_error(L, "Batch.render() something went wrong");

    return 0;
}

static int L_clear(lua_State *L) {
    BatchClear();
    return 0;
}

static const luaL_Reg L_methods[] = {
    {"addDraw",         L_addDraw},
    // TODO: создать функции под установку через индекс crop, 
    //       rotation, alpha, color, blend и т.д.
    {"render",          L_render},
    {"clear",           L_clear},
    {0, 0}
};

int BATCH_init(lua_State *L) {
    luaL_register(L, "Batch", L_methods);

    return 0;
}
