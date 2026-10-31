# WaveGrid

A water surface moved on the CPU, driven from a script: a flat grid of points (x across, z along, every `step`), raised and lowered each frame by a swell (sines across the water, summed) and by rings spreading from points (each a short train of waves riding out and dying away). It fades to nothing at the grid's rim, so the water never pulls away from its walls. The sums are done in C++: in Lua, a few thousand points a frame would cost too much on GameCube.

The heights are written into the meshes of its layers (`AddLayer`). Each layer is a grid mesh of exactly the grid's points, in order: row by row along z, each row x ascending (`GetNumColumns` x `GetNumRows` vertices, no vertex colours). The GPU draws one frame while the CPU works on the next, so each layer is two nodes, each with its own copy of the mesh. Each frame the copy not drawn last frame is written and shown, and the other hidden.

On GameCube the layer meshes must stay float vertices: keep them out of `GFX_SetQuantizedMeshes` with `GFX_SetQuantizeFilter` (in the game's `OctPreInitialize`), or `AddLayer` refuses them.

The grid lives as long as the script holds it.

```lua
local grid = WaveGrid.Create({ minX = -10, maxX = 10, minZ = -8, maxZ = 8, step = 0.5, ringHeight = 0.3 })
grid:AddWave(1.0, 0.3, 5.0, 0.012, 0.7)          -- a gentle swell
grid:AddLayer(world:FindNode("Water"), world:FindNode("WaterB"), true)
-- every frame:
grid:Update(deltaTime)
-- a splash:
grid:Ring(x, z, 1.0)
-- something floating:
local h = grid:HeightAt(x, z)
```

---
### Create
Create a grid. The table's fields (all optional but the bounds and `step`):

| Field | Default | |
|---|---|---|
| `minX`, `maxX`, `minZ`, `maxZ` | 0, 1, 0, 1 | the grid's bounds (its first and last points) |
| `step` | 1 | between the points |
| `edgeFade` | 0.75 | in from the rim to full height |
| `shadeGain` | 1 | the slope lit layers' normals see, of the real one (more: deeper-looking waves) |
| `ringSpeed` | 2 | a second a ring's waves ride out |
| `ringWavelength` | 3 | between a ring's crests |
| `ringWidth` | 3 | either side of a ring's middle it reaches |
| `ringLife` | 2.5 | seconds for a ring to die to a third |
| `ringHeight` | 0.3 | a full ring's height (strength 1) |
| `ringCutoff` | 0.04 | a ring is dropped once this faint |
| `maxRings` | 6 | rings at once (a new one takes the oldest's place) |

Sig: `grid = WaveGrid.Create(desc)`
 - Arg: `table desc` The fields above
 - Ret: `WaveGrid grid` The grid
---
### AddWave
Add a sine to the swell. Speed 1 is deep water's for the wavelength.

Sig: `WaveGrid:AddWave(dirX, dirZ, wavelength, height, speed = 1, phase = 0)`
 - Arg: `number dirX, dirZ` Its direction
 - Arg: `number wavelength` Between its crests
 - Arg: `number height` Its height
 - Arg: `number speed` Of deep water's
 - Arg: `number phase` Its start (radians)
---
### AddLayer
Add a layer: two StaticMesh3D nodes, each showing its own copy of a grid mesh (see above). Lit: its normals are written too, smoothly shaded. Hides the second node. False, with the reason logged, if the meshes don't fit the grid.

Sig: `ok = WaveGrid:AddLayer(nodeA, nodeB, lit = false)`
 - Arg: `StaticMesh3D nodeA, nodeB` The layer's two nodes
 - Arg: `boolean lit` Write its normals
 - Ret: `boolean ok` Added
---
### Ring
Start a ring spreading from a point (off the grid: none).

Sig: `WaveGrid:Ring(x, z, strength = 1)`
 - Arg: `number x, z` Where
 - Arg: `number strength` 1 a full ring (`ringHeight`)
---
### HeightAt
The water's height at a point over its rest, as drawn this frame (blended between the points). 0 off the grid.

Sig: `height = WaveGrid:HeightAt(x, z)`
 - Arg: `number x, z` Where
 - Ret: `number height` Its height
---
### Update
Move the water a frame: the heights, the normals, and the next copy of each layer written and shown. Call it once a frame.

Sig: `WaveGrid:Update(deltaTime)`
 - Arg: `number deltaTime` Seconds since the last frame
---
### GetNumColumns
Sig: `columns = WaveGrid:GetNumColumns()`
 - Ret: `integer columns` Points along x
---
### GetNumRows
Sig: `rows = WaveGrid:GetNumRows()`
 - Ret: `integer rows` Points along z
---
