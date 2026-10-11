// ============================================================================
// i30 N demister-grille cover  —  parametric base v2
// Clean KOSODÉLNÍK (parallelogram) duct + 4-slot grille. No gauge pod (add later).
// All dims in mm. Measured: face 90x53, ~20mm tall, ~45deg lean, 4 slots, 1mm slats.
// ============================================================================

// ---- duct (parallelogram / kosodélník) ----
W     = 90;   // face width  (X)
D     = 53;   // depth       (Y)
H     = 20;   // height      (Z)
LEAN  = 20;   // horizontal offset top vs bottom (=H -> 45deg lean, along X)
WALL  = 2;    // shell wall thickness

// ---- grille ----
N_SLOT   = 4;
PITCH    = 10;              // slot centre-to-centre
SLAT     = 1;              // bar between slots
SLOT_W   = PITCH - SLAT;    // 9mm opening
SLOT_LEN = D * 0.62;       // slot length along depth

$fn = 48;

// sharp rectangular footprint (gives clean parallelogram sides)
module foot(w, d) square([w, d], center = true);

// leaning parallelepiped between a bottom footprint and a top footprint offset by LEAN
module box(w, d, lean, h) {
  hull() {
    linear_extrude(0.01) foot(w, d);
    translate([lean, 0, h]) linear_extrude(0.01) foot(w, d);
  }
}

// hollow shell, open top & bottom
module shell() {
  difference() {
    box(W, D, LEAN, H);
    hull() {
      translate([0, 0, -1])      linear_extrude(0.01) foot(W-2*WALL, D-2*WALL);
      translate([LEAN, 0, H+1])  linear_extrude(0.01) foot(W-2*WALL, D-2*WALL);
    }
  }
}

// grille cap over the top opening, with N_SLOT slots cut through
module grille() {
  span = (N_SLOT-1) * PITCH;
  difference() {
    translate([LEAN, 0, H-WALL]) linear_extrude(WALL) foot(W-2*WALL+0.2, D-2*WALL+0.2);
    for (i = [0:N_SLOT-1])
      translate([LEAN - span/2 + i*PITCH, 0, H-WALL-0.1])
        linear_extrude(WALL+0.2) square([SLOT_W, SLOT_LEN], center = true);
  }
}

union() {
  shell();
  grille();
}
