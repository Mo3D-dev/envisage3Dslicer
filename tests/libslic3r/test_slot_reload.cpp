#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/GCode/SlotReloadPlanner.hpp"

#include <string>
#include <vector>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

static std::vector<SlotReloadLayer> cube_layers()
{
    // The printed 6-colour cube: green and red share the opening layers, then
    // one colour at a time, with teal and beige only after the first four finish.
    std::vector<SlotReloadLayer> layers(89);
    for (int i = 0; i < 89; ++i) {
        layers[i].print_z = (i + 1) * 0.28;
        auto add = [&](unsigned filament) { layers[i].filaments.push_back(filament); };
        if (i == 0)
            add(0);
        if (i >= 1 && i <= 4) {
            add(0);
            add(1);
        }
        if (i >= 5 && i <= 17)
            add(1);
        if (i >= 18 && i <= 32)
            add(0);
        if (i >= 33 && i <= 47)
            add(2);
        if (i >= 48 && i <= 64)
            add(3);
        if (i >= 65 && i <= 82)
            add(5);
        if (i >= 83 && i <= 88)
            add(4);
    }
    return layers;
}

TEST_CASE("Six colours on four slots pause once at the reload layer", "[SlotReload]")
{
    const std::vector<std::string> colors = {"#008000", "#F72323", "#FFFFFF", "#898989", "#00C1AE", "#F4E2C1"};
    const SlotReloadPlan plan = plan_slot_reloads(cube_layers(), colors, 4);

    REQUIRE(plan.problem == SlotReloadProblem::None);
    REQUIRE(plan.remap);
    REQUIRE(plan.pauses.size() == 1);
    CHECK(plan.pauses[0].layer_number == 66);
    CHECK_THAT(plan.pauses[0].print_z, WithinAbs(18.48, 0.02));
    REQUIRE(plan.pauses[0].swaps.size() == 2);
    for (int filament = 0; filament < 6; ++filament) {
        CHECK(plan.filament_to_slot[filament] >= 0);
        CHECK(plan.filament_to_slot[filament] < 4);
    }
    CHECK(plan.filament_to_slot[0] != plan.filament_to_slot[1]);
    CHECK(plan.swap_sheet.find("#00C1AE") != std::string::npos);
    CHECK(plan.swap_sheet.find("#F4E2C1") != std::string::npos);

    std::string marked = ";HEADER\n";
    for (const std::string &line : plan.marker_lines)
        marked += ";" + line + "\n";
    const SlotReloadExtract extracted = extract_slot_reload_markers(marked);
    CHECK(extracted.swap_sheet.find("Load at start:") == 0);
    CHECK(extracted.directives.size() == 3);
}

TEST_CASE("A layer with more colours than slots is rejected", "[SlotReload]")
{
    SlotReloadLayer crowded;
    crowded.print_z    = 1.0;
    crowded.filaments  = {0, 1, 2, 3, 4};
    const SlotReloadPlan plan = plan_slot_reloads({crowded}, {"#111111", "#222222", "#333333", "#444444", "#555555"}, 4);
    CHECK(plan.problem == SlotReloadProblem::TooManyOnOneLayer);
    CHECK(plan.problem_layer_number == 1);
    CHECK(plan.problem_colour_count == 5);
    CHECK_FALSE(plan.remap);
}

TEST_CASE("Four colours on four slots keep their own toolheads", "[SlotReload]")
{
    std::vector<SlotReloadLayer> layers(2);
    layers[0].print_z   = 0.28;
    layers[0].filaments = {0, 1};
    layers[1].print_z   = 0.56;
    layers[1].filaments = {2, 3};
    const SlotReloadPlan plan = plan_slot_reloads(layers, {"#008000", "#F72323", "#FFFFFF", "#898989"}, 4);
    REQUIRE(plan.problem == SlotReloadProblem::None);
    CHECK_FALSE(plan.remap);
    CHECK(plan.pauses.empty());
    CHECK(plan.filament_to_slot[0] == 0);
    CHECK(plan.filament_to_slot[3] == 3);
}
