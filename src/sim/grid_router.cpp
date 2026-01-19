#include "sim/grid_router.h"
#include "sim/factory.h"
#include <cstddef>
#include <queue>
#include <utility>

namespace sim {

namespace {
std::vector<std::pair<int, int>> assignment_order = {
    {0, 0}, {1, 0}, {1, -1}, {0, -1},
    {-1, 0}, {2, 0}, {2, -1}, {-1, -1},
    {1, -2}, {0, -2}
};

} // namespace

GRID_ROUTER::GRID_ROUTER(configuration::FACTORY_ALLOCATION& alloc, double freq_khz)
    :OPERABLE("GRID_ROUTER", freq_khz)
{
    // Initialize grid with factories
    grid_.resize(4, std::vector<Cell>(3, {0, nullptr}));

    origin_ = {1,2};

    int index = 0;
    for (auto* f : alloc.first_level) {
        auto [dx, dy] = assignment_order[index++];
        grid_[origin_.first + dx][origin_.second + dy].factory = f;
    }
}

T_FACTORY_BASE* GRID_ROUTER::request_magic_state()
{
    std::queue<std::pair<int,int>> bfs;
    std::vector<std::vector<std::pair<int, int>>> path(
        grid_.size(), std::vector<std::pair<int, int>>(
            grid_[0].size(), {-1, -1}));
    
    bfs.push(origin_);
    bfs.push({origin_.first+1, origin_.second});
    path[origin_.first][origin_.second] = {0,0};
    path[origin_.first+1][origin_.second] = {0,0};


    while (!bfs.empty()) {
        auto [x, y] = bfs.front();
        bfs.pop();

        Cell& cell = grid_[x][y];
        if (cell.factory==nullptr || cell.busy_cycles > 0)
            continue;

        if (cell.factory->buffer_occupancy() > 0) {
            // Find path back to origin and mark cells as busy
            cell.busy_cycles = 2;
            cell.factory->disable();
            std::pair<int, int> curr = path[x][y];
            while (curr != std::make_pair(0,0)) {
                auto [px, py] = path[curr.first][curr.second];
                Cell& cell = grid_[curr.first][curr.second];
                cell.busy_cycles = 1;
                cell.factory->disable();
                curr = {px, py};
            }
            return cell.factory;
        }

        // Explore neighbors
        std::vector<std::pair<int, int>> neighbors = {
            {x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}
        };

        for (auto [nx, ny] : neighbors) {
            if (nx >= 0 && nx < (int)grid_.size() && 
                ny >= 0 && ny < (int)grid_[0].size() &&
                path[nx][ny] == std::make_pair(-1, -1)) {
                path[nx][ny] = {x, y};
                bfs.push({nx, ny});
            }
        }
    }
    return nullptr;
}

long GRID_ROUTER::operate() {
    for (auto& row : grid_) {
        for (auto& cell : row) {
            if (cell.busy_cycles > 0) {
                cell.busy_cycles--;
                if (cell.busy_cycles == 0) {
                    cell.factory->enable();
                }
            }
        }
    }
    return 1;
}

void GRID_ROUTER::print_deadlock_info(std::ostream& out) const {
    out << "GRID_ROUTER State:\n";
    for (size_t i = 0; i < grid_.size(); ++i) {
        for (size_t j = 0; j < grid_[i].size(); ++j) {
            const Cell& cell = grid_[i][j];
            out << "Cell (" << i << ", " << j << "): "
                << "Busy Cycles = " << cell.busy_cycles << ", "
                << "Factory = " << (cell.factory ? cell.factory->name : "None") << "\n";
        }
    }
}

void GRID_ROUTER::print_grid(std::ostream& out) const {
    out << "GRID_ROUTER Grid State:\n";
    for (size_t i = 0; i < grid_.size(); ++i) {
        for (size_t j = 0; j < grid_[i].size(); ++j) {
            const Cell& cell = grid_[i][j];
            out << "[Busy: " << cell.busy_cycles << " ";
            out << (cell.factory!=nullptr && cell.factory->buffer_occupancy() > 0? "M] ": " ]");
        }
        out << "\n";
    }
}

} // namespace sim