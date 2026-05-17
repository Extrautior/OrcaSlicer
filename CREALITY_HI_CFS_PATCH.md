# Creality Hi CFS OrcaSlicer Patch

This branch is based on OrcaSlicer 2.3.2 and adds the Creality Hi / CFS support that was tested against the local Creality Hi-3/4C printer flow.

## What This Patch Adds

- Creality Print style Device page inside OrcaSlicer for Creality printers.
- CFS filament mapping support in the print/upload dialog.
- CFS sync and printer metadata handling for Creality Hi printers.
- Native bridge for Device page local-file export/download.
- Camera URL handling for the Creality WebRTC endpoint.
- Creality-compatible G-code metadata for printer-side layer and time display:
  - `;:<layer_z>` layer markers for Creality hosts.
  - fallback `;AFTER_LAYER_CHANGE` markers when the profile field is empty.
  - `;TIME_ELAPSED:` comments in generated G-code.
- Creality flush-volume behavior:
  - flush volumes are calculated using the selected multiplier.
  - generated config keeps `flush_multiplier = 1` for Creality hosts while emitting updated `flush_volumes_matrix`.

## Recommended Profile Notes

For Creality Hi printers, use the Creality Print style machine start G-code in the printer profile. This avoids the extra hot wait and heavy purge that caused ooze near the cutter/poop area in the Orca profile.

```gcode
M140 S0
M104 S0 
START_PRINT EXTRUDER_TEMP=[nozzle_temperature_initial_layer] BED_TEMP=[bed_temperature_initial_layer_single]
T[initial_no_support_extruder]
M104 S[nozzle_temperature_initial_layer]
M204 S2000
G1 Z3 F600
M83
G1 Y150 F12000
G1 X0 F12000
G1 Z0.2 F600
G1 X0 Y150 F6000
G1 E0.8 F300
G1 X0 Y0 E9 F{filament_max_volumetric_speed[initial_extruder]/0.3*60}
G1 X150 Y0 E9 F{filament_max_volumetric_speed[initial_extruder]/0.3*60}
G92 E0
G1 Z1 F600
```

Layer-change G-code can remain empty in the profile because the patch adds the Creality-compatible fallback for Creality print hosts.

## Updating For A New OrcaSlicer Release

When OrcaSlicer updates, rebase or cherry-pick this branch onto the new stable version, then check these areas first:

- `src/slic3r/GUI/PrinterWebView.*`
- `src/slic3r/Utils/CrealityPrint.*`
- `src/slic3r/GUI/Plater.*`
- `src/slic3r/GUI/PrintHostDialogs.*`
- `src/libslic3r/GCode.cpp`
- `src/libslic3r/GCode/GCodeProcessor.*`
- `src/libslic3r/Print.cpp`

After rebuilding, verify:

- Device page opens directly to the single detected printer.
- Camera loads and can recover after a refresh.
- Local Files export opens the Windows save dialog and downloads the selected file.
- Print dialog still shows CFS mapping and the jump-to-device option.
- Color/filament selector in the object list changes color reliably.
- Newly sliced Creality-host G-code includes `flush_volumes_changed`, updated `flush_volumes_matrix`, `;TIME_ELAPSED:`, and Creality layer markers.

