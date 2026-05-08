# LuaPlayerYT-MCSM

### [russian readme version / русская версия readme](./README_RU.md)

## Fork Features

1. **Easy Error Screen Customization** - Simple configuration options for error appearance, sound, font, and background image.
2. **Simplified Build Process** - Uses a `build.sh` script that is easily modifiable and allows adding any necessary compilation commands.
3. **Minimalist Codebase** - Unnecessary libraries have been removed to reduce size and simplify maintenance.
4. **Optimizations** - For example, collect garbage in the pmp_play function, which makes pmp_play more stable
5. **Additional Code Comments** - Added explanatory comments to customization settings for easier modification.

**Note:** This fork is specifically created for the [Minecraft: Story Mode PSP](https://github.com/entitybtw/mcsm_portable) port, meaning all engine optimizations are primarily aimed at optimizing this port and ensuring its stable operation on PSP.

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
