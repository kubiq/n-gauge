# Printing Guide — i30 N Vent Mod (Ender 3 V2, Cura)

Settings and fixes for printing the vent parts reliably and fast.

## Speed optimization (Cura)
Biggest time savers, in order of impact:

1. **Layer Height** (Quality) — **0.28 mm** for test/fit prints (was 0.16 → ~40 % faster).
   Drop to **0.2 mm** only for the final good-looking version (smoother bezel/vanes).
2. **Infill** — **10 %**, pattern **Lightning** (builds infill only where needed → fast, less filament).
3. **Print Speed** — **65–70 mm/s** max at 0.28 mm layers.
   - ⚠ Don't go higher at thick layers: flow = layer × width × speed. 0.28 × 0.4 × 70 ≈ 7.8 mm³/s
     (near a stock hotend's ~10–12 limit). Faster = under-extrusion.
   - To keep surface quality: raise only **Infill Speed** + **Inner Wall Speed**, keep
     **Outer Wall Speed** ~40–50.
4. **Wall Line Count** — **2** (0.8 mm) is plenty for this non-structural part.

## Supports (Cura)
- **Structure: Tree** (far less material/time than Normal).
- **Support Wall Line Count: 1**, density minimal — already lean.
- **Support Overhang Angle: ~55°** (higher = fewer supported areas = faster; too high risks the
  pod underside sagging).
- **Support Placement:** try **Touching Buildplate**; if the pod underside sags, switch back to
  **Everywhere**.

## ⚠ Ender 3 V2 first-layer reliability (the "squished here / floating there" fix)
Cause: **bed warp + level drift** (not the model). Fixes, cheapest/highest-impact first:

**Free (do every time):**
1. **Level HOT** — heat bed to **60 °C** (nozzle ~200) *first*, then paper-drag level at
   **5 points** (4 corners + centre), **2–3 passes**. Bed expands when hot; leveling cold is off.
2. **Cura first-layer settings:**
   - Initial Layer Speed: **20 mm/s**
   - Initial Layer Height: **0.24 mm** (hides small level errors)
   - Initial Fan Speed: **0**
   - Initial Layer Flow: **105 %**
   - Build Plate Adhesion: **Brim** (sticks better + shows where leveling is off)
3. **Clean the bed** with **isopropyl alcohol** (finger grease kills adhesion).

**Cheap upgrades (permanent fixes):**
4. **Silicone bed spacers** (~$5) replacing the stock springs → stops level drift between prints.
   *Biggest bang for buck.*
5. **CR Touch / BLTouch** (~$35) → **auto bed-mesh leveling**, which compensates for the warp that
   manual 4-point leveling can't. The real cure for "some areas squished, some floating."

**Reality:** the V2's stock bed isn't flat, so manual 4-point leveling can never fully fix a warped
centre — a CR Touch mesh is what solves it for good.

## STL export notes (from the Blender build)
- The `.blend` had leftover objects (**`Plane`**, **`floor`**) — exclude/delete them before export,
  or they add stray geometry that makes Cura flag "not watertight" (crosshatch).
- The model can also carry tiny **stray 2-face bits** (from boolean/loop-select history) that
  Blender doesn't flag but break the STL. Clean STL to print: **`vent42_clean.stl`**
  (watertight, single body, 6656 faces, crisp — no voxel remesh needed).
- If a future export still crosshatches: keep only the largest watertight component
  (done here with trimesh: `split → max by faces → export`).

## Material
- **PLA** for test/fit prints.
- **ASA or PETG** for the final (dash heat + UV; PLA softens in a parked car).
