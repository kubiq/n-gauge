# Gauge pod — printed parts

The source of truth is an **Onshape** document (not public yet); these STLs
are exports from it, kept so a part can be reprinted without opening the CAD.

- Exported: 2026-10-06

| File | Part |
|---|---|
| `i30n_pod_left.stl` | Pod shell, left-hand side |
| `i30n_vent_left.stl` | Vent piece, left-hand side (carries the N logo) |
| `i30n_internals.stl` | Internal mount for the boards |

When re-exporting, overwrite the file here and update the date, so it is
always clear which export is current. Slicing notes (supports, orientation)
are in `PRINTING_GUIDE.md` at the repo root.
