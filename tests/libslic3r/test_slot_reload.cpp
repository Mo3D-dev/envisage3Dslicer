#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "libslic3r/GCode/SlotReloadPlanner.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <algorithm>
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

// The 6-colour test cube: four 0.4 mm nozzles, six project colours, slot sharing on Auto.
static DynamicPrintConfig six_filament_project(SlotReloadMode mode, const std::string &printer_model)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4, 0.4, 0.4};
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = false;
    config.option<ConfigOptionEnum<SlotReloadMode>>("slot_reload_mode")->value = mode;
    config.option<ConfigOptionString>("printer_model")->value = printer_model;
    config.option<ConfigOptionStrings>("filament_colour")->values = {
        "#008000", "#F72323", "#FFFFFF", "#898989", "#00C1AE", "#F4E2C1"};
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75, 1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_settings_id", true)->values = {
        "Generic PLA @FF C5P", "Generic PLA @FF C5P", "Generic PLA @FF C5P",
        "Generic PLA @FF C5P", "Generic PLA @FF C5P", "Generic PLA @FF C5P"};
    return config;
}

TEST_CASE("Creator 5 Pro keeps six filament colours and four nozzles", "[SlotReload]")
{
    DynamicPrintConfig config = six_filament_project(SlotReloadMode::Auto, "Flashforge Creator 5 Pro");
    Preset::normalize(config);

    const auto colours = config.option<ConfigOptionStrings>("filament_colour")->values;
    REQUIRE(colours.size() == 6);
    CHECK(colours[0] == "#008000");
    CHECK(colours[3] == "#898989");
    CHECK(colours[4] == "#00C1AE");
    CHECK(colours[5] == "#F4E2C1");
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 4);
    CHECK(config.option<ConfigOptionFloats>("filament_diameter")->values.size() == 6);
    // The 3MF importer treats a part extruder above this list as invalid and rewrites it to 1.
    CHECK(config.option<ConfigOptionStrings>("filament_settings_id")->values.size() == 6);
}

TEST_CASE("Slot sharing off clamps a multi-tool project to its nozzles", "[SlotReload]")
{
    DynamicPrintConfig config = six_filament_project(SlotReloadMode::Off, "Flashforge Creator 5 Pro");
    Preset::normalize(config);
    CHECK(config.option<ConfigOptionStrings>("filament_settings_id")->values.size() == 4);
    CHECK(config.option<ConfigOptionFloats>("filament_diameter")->values.size() == 4);
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 4);
}

TEST_CASE("Auto leaves other printers clamped to their nozzles", "[SlotReload]")
{
    DynamicPrintConfig config = six_filament_project(SlotReloadMode::Auto, "Some Other Printer");
    Preset::normalize(config);
    CHECK(config.option<ConfigOptionStrings>("filament_settings_id")->values.size() == 4);
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 4);
}

TEST_CASE("On keeps extra filaments for any printer", "[SlotReload]")
{
    DynamicPrintConfig config = six_filament_project(SlotReloadMode::On, "Some Other Printer");
    Preset::normalize(config);
    CHECK(config.option<ConfigOptionStrings>("filament_settings_id")->values.size() == 6);
    CHECK(config.option<ConfigOptionStrings>("filament_colour")->values[5] == "#F4E2C1");
    CHECK(config.option<ConfigOptionFloats>("nozzle_diameter")->values.size() == 4);
}

TEST_CASE("A six-colour project replaces a leftover mixed filament", "[SlotReload]")
{
    const std::vector<std::string> colours = {"#008000", "#F72323", "#FFFFFF", "#898989", "#00C1AE", "#F4E2C1"};

    PresetBundle bundle;
    bundle.project_config.option<ConfigOptionBools>("filament_is_mixed", true)->values = {0, 0, 0, 0, 1};
    bundle.project_config.option<ConfigOptionStrings>("filament_mixed_components", true)->values = {"", "", "", "", "1,2"};
    bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values = {"#26A69A", "#D27E3C", "#B28AE4", "#C5D752"};

    DynamicPrintConfig config = six_filament_project(SlotReloadMode::Auto, "Flashforge Creator 5 Pro");
    config.option<ConfigOptionStrings>("filament_type", true)->values.assign(6, "PLA");
    config.option<ConfigOptionInts>("filament_self_index", true)->values = {1, 2, 3, 4, 5, 6};
    config.option<ConfigOptionStrings>("filament_extruder_variant", true)->values.assign(6, "Direct Drive Standard");
    Preset::normalize(config);
    bundle.load_config_model("Envisage3D_test_cube_6col.3mf", std::move(config));

    REQUIRE(bundle.filament_presets.size() == 6);
    CHECK(bundle.project_config.option<ConfigOptionStrings>("filament_colour")->values == colours);
    const auto &mixed = bundle.project_config.option<ConfigOptionBools>("filament_is_mixed")->values;
    REQUIRE(mixed.size() == 6);
    CHECK(std::find(mixed.begin(), mixed.end(), static_cast<unsigned char>(1)) == mixed.end());
}
