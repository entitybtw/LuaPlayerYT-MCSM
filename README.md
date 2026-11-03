# LuaPlayerYT-MCSM

### [russian readme version / русская версия readme](./README_RU.md)

## Fork Features

1. **Easy Error Screen Customization** - Simple configuration options for error appearance, sound, font, and background image.
2. **Simplified Build Process** - Uses a `build.sh` script that is easily modifiable and allows adding any necessary compilation commands.
3. **Minimalist Codebase** - Unnecessary libraries have been removed to reduce size and simplify maintenance.
4. **Optimizations** - For example, collect garbage in the pmp_play function, which makes pmp_play more stable
5. **Additional Code Comments** - Added explanatory comments to customization settings for easier modification.
6. **3D meshes (`Mesh`)** - Coloured, optionally textured triangles drawn by the GPU in 3D between the 2D sprites, with a camera, depth, fog and face culling. See [3D meshes](#3d-meshes).

**Note:** This fork is specifically created for the [Minecraft: Story Mode PSP](https://github.com/entitybtw/mcsm_portable) port, meaning all engine optimizations are primarily aimed at optimizing this port and ensuring its stable operation on PSP.

## 3D meshes

```lua
-- a mesh from a .msh file or from Lua tables
local m = Mesh.load("model.msh")
local tri = Mesh.new({ {-1, 0, 0, Color.new(255, 0, 0)}, {1, 0, 0}, {0, 1.5, 0} })
-- vertex: {x, y, z, [color], [u], [v]}; u, v are 0..1 over the picture;
-- a second table of 1-based indices (3 per triangle) is optional

Mesh.camera({ fov = 60, x = 0, y = 1, z = 6, pitch = -10, yaw = 0, roll = 0,
              near = 0.5, far = 1000 })        -- looks along -z, angles in degrees
Mesh.fog(40, 48, Color.new(120, 150, 210))     -- Mesh.fog() turns it off
Mesh.cull("cw")                                -- "none", "cw" or "ccw"

-- every frame
screen.clear()
Image.draw(background, 0, 0)                   -- 2D under the meshes
Mesh.clearDepth()
Mesh.draw(m, { x = 0, y = 0, z = 0, yaw = 30, scale = 1,   -- or sx, sy, sz
               tint = Color.new(255, 255, 255), alpha = 255,
               texture = img, linear = true, ["repeat"] = false, depth = true })
Image.draw(hud, 0, 0)                          -- 2D over the meshes
screen.flip()

Mesh.unload(m)
```

The colours of the vertices are the material and `tint` is the light on it
(a texture is multiplied with both). A `.msh` file is little endian:
`"MSH1"`, u32 vertex count, u32 index count, u32 flags (1: the vertices have
u, v), the vertices as `[float u, v] u32 colour (ABGR) float x, y, z`, then
the indices as u16.

## How to Build?

1. Install PSPSDK as [instructed](https://pspdev.github.io/installation.html).
2. Clone the repository to the target folder using the command:

```bash
git clone https://github.com/entitybtw/LuaPlayerYT-MCSM
```

3. Open the repository folder in the terminal and start the build:

```bash
./build.sh
```

## How to Download the Latest Version Without Building?

The repository has an auto-builder that compiles the current version from the latest code.

To download:

1. Go to the **Actions** tab for the main branch.
2. Select the most recent build (the higher — the newer).
3. In the **Artifacts** section, download `EBOOT.PBP`.

**Note:** These builds are compiled from the latest code, which may be unstable in some situations.
