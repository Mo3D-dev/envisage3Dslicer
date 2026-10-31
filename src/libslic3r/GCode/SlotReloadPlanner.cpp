#include "SlotReloadPlanner.hpp"

#include <algorithm>
#include <limits>
#include <sstream>
#include <utility>

namespace Slic3r {
namespace {

constexpr int kMaxTrackedFilaments = 64;
constexpr int kExhaustiveFilaments = 10;

struct Occupancy {
    std::vector<int> layers;
};

bool overlaps(const Occupancy &a, const Occupancy &b)
{
    size_t i = 0;
    size_t j = 0;
    while (i < a.layers.size() && j < b.layers.size()) {
        if (a.layers[i] == b.layers[j])
            return true;
        if (a.layers[i] < b.layers[j])
            ++i;
        else
            ++j;
    }
    return false;
}

struct ScoredAssignment {
    int              pauses{std::numeric_limits<int>::max()};
    int              lateness{std::numeric_limits<int>::min()};
    int              earliness{std::numeric_limits<int>::max()};
    int              swaps{std::numeric_limits<int>::max()};
    std::vector<int> assign;
};

struct Interval {
    int      start{0};
    int      end{0};
    int      slot{0};
    unsigned from_filament{0};
    unsigned to_filament{0};
};

struct SlotUse {
    int      first{0};
    int      last{0};
    unsigned filament{0};
};

std::string color_of(const std::vector<std::string> &colors, unsigned filament)
{
    if (filament < colors.size() && !colors[filament].empty())
        return colors[filament];
    return "#000000";
}

std::string format_z(double z)
{
    std::ostringstream os;
    os.setf(std::ios::fixed);
    os.precision(2);
    os << z;
    return os.str();
}

// Returns false when two filaments on one slot are used on the same layer.
bool score_assignment(const std::vector<int> &assign, const std::vector<Occupancy> &occ, int slot_count,
                      const std::vector<unsigned> &used, int &pause_count, int &lateness, int &earliness, int &swap_count,
                      std::vector<Interval> &intervals_out, std::vector<std::vector<SlotUse>> &uses_out)
{
    uses_out.assign(slot_count, {});
    for (unsigned filament : used) {
        const int slot = assign[filament];
        if (slot < 0 || slot >= slot_count || occ[filament].layers.empty())
            return false;
        SlotUse use;
        use.first    = occ[filament].layers.front();
        use.last     = occ[filament].layers.back();
        use.filament = filament;
        uses_out[slot].push_back(use);
    }
    intervals_out.clear();
    for (int slot = 0; slot < slot_count; ++slot) {
        auto &uses = uses_out[slot];
        std::sort(uses.begin(), uses.end(), [](const SlotUse &a, const SlotUse &b) { return a.first < b.first; });
        for (size_t i = 1; i < uses.size(); ++i) {
            if (uses[i - 1].last >= uses[i].first)
                return false;
            Interval interval;
            interval.start         = uses[i - 1].last + 1;
            interval.end           = uses[i].first;
            interval.slot          = slot;
            interval.from_filament = uses[i - 1].filament;
            interval.to_filament   = uses[i].filament;
            intervals_out.push_back(interval);
        }
    }
    std::vector<Interval> ordered = intervals_out;
    std::sort(ordered.begin(), ordered.end(), [](const Interval &a, const Interval &b) {
        return a.end < b.end || (a.end == b.end && a.slot < b.slot);
    });
    std::vector<char> stabbed(ordered.size(), 0);
    std::vector<int>  load_layer(occ.size(), 0);
    pause_count = 0;
    lateness    = 0;
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (stabbed[i])
            continue;
        const int point = ordered[i].end;
        ++pause_count;
        lateness += point;
        for (size_t j = 0; j < ordered.size(); ++j) {
            if (stabbed[j] || ordered[j].start > point || point > ordered[j].end)
                continue;
            stabbed[j] = 1;
            load_layer[ordered[j].to_filament] = point;
        }
    }
    earliness = 0;
    for (unsigned filament : used)
        earliness += std::max(0, occ[filament].layers.front() - load_layer[filament]);
    swap_count = (int) intervals_out.size();
    return true;
}

bool better_score(int pauses, int lateness, int earliness, int swaps, const std::vector<int> &assign, const ScoredAssignment &best)
{
    if (pauses != best.pauses)
        return pauses < best.pauses;
    // Same number of pauses: do them as late as the spools allow.
    if (lateness != best.lateness)
        return lateness > best.lateness;
    if (earliness != best.earliness)
        return earliness < best.earliness;
    if (swaps != best.swaps)
        return swaps < best.swaps;
    return assign < best.assign;
}

void search_exhaustive(size_t index, const std::vector<unsigned> &used, const std::vector<Occupancy> &occ, int slot_count,
                       std::vector<int> &assign, ScoredAssignment &best)
{
    if (index == used.size()) {
        int pauses    = 0;
        int lateness   = 0;
        int earliness = 0;
        int swaps     = 0;
        std::vector<Interval>             intervals;
        std::vector<std::vector<SlotUse>> uses;
        if (!score_assignment(assign, occ, slot_count, used, pauses, lateness, earliness, swaps, intervals, uses))
            return;
        if (better_score(pauses, lateness, earliness, swaps, assign, best)) {
            best.pauses    = pauses;
            best.lateness  = lateness;
            best.earliness = earliness;
            best.swaps     = swaps;
            best.assign    = assign;
        }
        return;
    }
    const unsigned filament = used[index];
    for (int slot = 0; slot < slot_count; ++slot) {
        bool conflict = false;
        for (size_t earlier = 0; earlier < index; ++earlier) {
            const unsigned other = used[earlier];
            if (assign[other] == slot && overlaps(occ[filament], occ[other])) {
                conflict = true;
                break;
            }
        }
        if (conflict)
            continue;
        assign[filament] = slot;
        search_exhaustive(index + 1, used, occ, slot_count, assign, best);
    }
    assign[filament] = -1;
}

void search_greedy(const std::vector<unsigned> &used, const std::vector<Occupancy> &occ, int slot_count, std::vector<int> &assign)
{
    std::vector<unsigned> order = used;
    std::sort(order.begin(), order.end(), [&](unsigned a, unsigned b) {
        return occ[a].layers.front() < occ[b].layers.front();
    });
    for (unsigned filament : order) {
        int best_slot = -1;
        int best_last = std::numeric_limits<int>::max();
        for (int slot = 0; slot < slot_count; ++slot) {
            bool conflict = false;
            int  last     = -1;
            for (unsigned other : used) {
                if (assign[other] != slot)
                    continue;
                if (overlaps(occ[filament], occ[other])) {
                    conflict = true;
                    break;
                }
                last = std::max(last, occ[other].layers.back());
            }
            if (conflict)
                continue;
            // Prefer a slot whose previous filament finished earliest, then the lowest slot.
            if (best_slot < 0 || last < best_last) {
                best_slot = slot;
                best_last = last;
            }
        }
        if (best_slot < 0)
            return;
        assign[filament] = best_slot;
    }
}

} // namespace

SlotReloadPlan plan_slot_reloads(const std::vector<SlotReloadLayer> &layers,
                                 const std::vector<std::string>     &filament_colors,
                                 int                                 slot_count)
{
    SlotReloadPlan plan;
    plan.slot_count = slot_count;
    if (slot_count < 1)
        slot_count = 1;

    int max_id = -1;
    for (const SlotReloadLayer &layer : layers)
        for (unsigned filament : layer.filaments)
            max_id = std::max(max_id, (int) filament);
    if (max_id < 0)
        return plan;

    std::vector<Occupancy> occ(max_id + 1);
    for (int layer_index = 0; layer_index < (int) layers.size(); ++layer_index) {
        std::vector<unsigned> unique = layers[layer_index].filaments;
        std::sort(unique.begin(), unique.end());
        unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
        if ((int) unique.size() > slot_count) {
            plan.problem               = SlotReloadProblem::TooManyOnOneLayer;
            plan.problem_layer_number  = layer_index + 1;
            plan.problem_print_z       = layers[layer_index].print_z;
            plan.problem_colour_count  = (int) unique.size();
            plan.slot_count            = slot_count;
            return plan;
        }
        for (unsigned filament : unique) {
            if ((int) filament >= kMaxTrackedFilaments) {
                plan.problem    = SlotReloadProblem::TooManyFilaments;
                plan.slot_count = slot_count;
                return plan;
            }
            occ[filament].layers.push_back(layer_index);
        }
    }

    std::vector<unsigned> used;
    for (int filament = 0; filament <= max_id; ++filament)
        if (!occ[filament].layers.empty())
            used.push_back((unsigned) filament);

    plan.filament_to_slot.assign(max_id + 1, -1);
    std::vector<int> assign(max_id + 1, -1);
    if ((int) used.size() <= kExhaustiveFilaments) {
        ScoredAssignment best;
        search_exhaustive(0, used, occ, slot_count, assign, best);
        if (best.assign.empty()) {
            plan.problem    = SlotReloadProblem::CannotShareSlots;
            plan.slot_count = slot_count;
            return plan;
        }
        assign = std::move(best.assign);
    } else {
        search_greedy(used, occ, slot_count, assign);
        for (unsigned filament : used) {
            if (assign[filament] < 0) {
                plan.problem    = SlotReloadProblem::CannotShareSlots;
                plan.slot_count = slot_count;
                return plan;
            }
        }
    }

    int pauses    = 0;
    int lateness  = 0;
    int earliness = 0;
    int swaps     = 0;
    std::vector<Interval>             intervals;
    std::vector<std::vector<SlotUse>> uses;
    if (!score_assignment(assign, occ, slot_count, used, pauses, lateness, earliness, swaps, intervals, uses)) {
        plan.problem    = SlotReloadProblem::CannotShareSlots;
        plan.slot_count = slot_count;
        return plan;
    }

    plan.filament_to_slot = assign;
    plan.slot_count       = slot_count;
    plan.initial_filament.assign(slot_count, SLOT_RELOAD_EMPTY);
    for (int slot = 0; slot < slot_count; ++slot)
        if (!uses[slot].empty())
            plan.initial_filament[slot] = uses[slot].front().filament;

    bool needs_remap = false;
    for (unsigned filament : used) {
        if ((int) filament >= slot_count || assign[filament] != (int) filament)
            needs_remap = true;
    }
    plan.remap = needs_remap;

    // Stab the swap windows so one pause covers every swap that is legal there.
    std::vector<Interval> ordered = intervals;
    std::sort(ordered.begin(), ordered.end(), [](const Interval &a, const Interval &b) {
        return a.end < b.end || (a.end == b.end && a.slot < b.slot);
    });
    std::vector<char> stabbed(ordered.size(), 0);
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (stabbed[i])
            continue;
        const int point = ordered[i].end;
        SlotReloadPause pause;
        pause.layer_index  = point;
        pause.layer_number = point + 1;
        pause.print_z      = layers[point].print_z;
        for (size_t j = 0; j < ordered.size(); ++j) {
            if (stabbed[j] || ordered[j].start > point || ordered[j].end < point)
                continue;
            stabbed[j] = 1;
            SlotReloadSwap swap;
            swap.slot          = ordered[j].slot;
            swap.from_filament = ordered[j].from_filament;
            swap.to_filament   = ordered[j].to_filament;
            pause.swaps.push_back(swap);
        }
        std::sort(pause.swaps.begin(), pause.swaps.end(), [](const SlotReloadSwap &a, const SlotReloadSwap &b) {
            return a.slot < b.slot;
        });
        plan.pauses.push_back(std::move(pause));
    }
    std::sort(plan.pauses.begin(), plan.pauses.end(), [](const SlotReloadPause &a, const SlotReloadPause &b) {
        return a.layer_index < b.layer_index;
    });

    std::string sheet = "Load at start:";
    bool        any_initial = false;
    for (int slot = 0; slot < slot_count; ++slot) {
        const unsigned filament = plan.initial_filament[slot];
        if (filament == SLOT_RELOAD_EMPTY)
            continue;
        any_initial = true;
        sheet += " Slot " + std::to_string(slot + 1) + " " + color_of(filament_colors, filament) + ",";
    }
    if (!any_initial)
        sheet += " none.";
    else if (!sheet.empty() && sheet.back() == ',')
        sheet.back() = '.';

    std::vector<std::string> lines;
    lines.push_back("SLOT_RELOAD_SHEET " + sheet);

    for (const SlotReloadPause &pause : plan.pauses) {
        std::ostringstream row;
        row << "PAUSE before layer " << pause.layer_number << " (Z " << format_z(pause.print_z) << " mm):";
        for (const SlotReloadSwap &swap : pause.swaps) {
            row << " Slot " << (swap.slot + 1) << " " << color_of(filament_colors, swap.from_filament) << " -> "
                << color_of(filament_colors, swap.to_filament) << ",";
        }
        std::string row_text = row.str();
        if (!row_text.empty() && row_text.back() == ',')
            row_text.back() = '.';
        sheet += "\n" + row_text;
        lines.push_back("SLOT_RELOAD_SHEET " + row_text);
        lines.push_back("SLOT_RELOAD z=" + format_z(pause.print_z) + " kind=pause extruder=1 color=");
        for (const SlotReloadSwap &swap : pause.swaps) {
            lines.push_back("SLOT_RELOAD z=" + format_z(pause.print_z) + " kind=color extruder=" + std::to_string(swap.slot + 1) +
                            " color=" + color_of(filament_colors, swap.to_filament));
        }
    }
    plan.swap_sheet   = sheet;
    plan.marker_lines = std::move(lines);
    return plan;
}

SlotReloadExtract extract_slot_reload_markers(const std::string &gcode)
{
    SlotReloadExtract extracted;
    std::istringstream in(gcode);
    std::string        line;
    std::ostringstream sheet;
    bool               any_sheet = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string body = (!line.empty() && line[0] == ';') ? line.substr(1) : line;
        const std::string sheet_key = "SLOT_RELOAD_SHEET ";
        const std::string reload_key = "SLOT_RELOAD ";
        if (body.compare(0, sheet_key.size(), sheet_key) == 0) {
            if (any_sheet)
                sheet << '\n';
            sheet << body.substr(sheet_key.size());
            any_sheet = true;
        } else if (body.compare(0, reload_key.size(), reload_key) == 0) {
            const std::string rest = body.substr(reload_key.size());
            SlotReloadDirective directive;
            std::istringstream fields(rest);
            std::string        token;
            while (fields >> token) {
                const auto eq = token.find('=');
                if (eq == std::string::npos)
                    continue;
                const std::string key   = token.substr(0, eq);
                const std::string value = token.substr(eq + 1);
                if (key == "z")
                    directive.print_z = std::stod(value);
                else if (key == "kind")
                    directive.pause = value == "pause";
                else if (key == "extruder")
                    directive.extruder_1based = std::stoi(value);
                else if (key == "color")
                    directive.color = value;
            }
            extracted.directives.push_back(std::move(directive));
        }
    }
    extracted.swap_sheet = sheet.str();
    return extracted;
}

} // namespace Slic3r
