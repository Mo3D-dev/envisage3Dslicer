#pragma once

#include <string>
#include <vector>

namespace Slic3r {

// One printed layer and the filament ids (0-based) that extrude on it.
struct SlotReloadLayer {
    double                print_z{0.};
    std::vector<unsigned> filaments;
};

enum class SlotReloadProblem {
    None,
    TooManyOnOneLayer,
    CannotShareSlots,
    TooManyFilaments
};

struct SlotReloadSwap {
    int      slot{0}; // 0-based physical tool
    unsigned from_filament{0};
    unsigned to_filament{0};
};

struct SlotReloadPause {
    double                       print_z{0.};
    int                          layer_number{0}; // 1-based position in the planned layers
    int                          layer_index{0};
    std::vector<SlotReloadSwap>  swaps;
};

// One record written into the sliced 3MF colour-change list.
struct SlotReloadDirective {
    double      print_z{0.};
    bool        pause{false};
    int         extruder_1based{1};
    std::string color;
};

// A static filament-to-slot assignment plus the fewest layer-start pauses that
// load every incoming spool after the previous spool on that slot has finished.
struct SlotReloadPlan {
    bool                 remap{false};
    SlotReloadProblem    problem{SlotReloadProblem::None};
    int                  problem_layer_number{0};
    double               problem_print_z{0.};
    int                  problem_colour_count{0};
    int                  slot_count{0};
    // filament id -> 0-based slot, or -1 when that filament is never used
    std::vector<int>     filament_to_slot;
    // slot -> filament loaded before the print starts. Empty slots stay unused.
    std::vector<unsigned> initial_filament;
    std::vector<SlotReloadPause> pauses;
    std::string          swap_sheet;
    // Header comment lines, without the leading ';'.
    std::vector<std::string> marker_lines;
};

inline constexpr unsigned SLOT_RELOAD_EMPTY = static_cast<unsigned>(-1);

// slot_count is the number of physical toolheads. Filament ids at or above
// that count, and ids that share a toolhead, are remapped. An impossible
// layer or an overlap that cannot be coloured with slot_count slots sets
// problem and leaves remap false.
SlotReloadPlan plan_slot_reloads(const std::vector<SlotReloadLayer> &layers,
                                 const std::vector<std::string>     &filament_colors,
                                 int                                 slot_count);

struct SlotReloadExtract {
    std::string                      swap_sheet;
    std::vector<SlotReloadDirective> directives;
};

// Reads SLOT_RELOAD_SHEET and SLOT_RELOAD lines out of a gcode file's text.
SlotReloadExtract extract_slot_reload_markers(const std::string &gcode);

} // namespace Slic3r
