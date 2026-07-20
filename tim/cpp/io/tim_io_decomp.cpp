/**
 * @file tim_io_decomp.cpp
 * @brief TIM::IoDecomp construction-time validation and the read/write
 * component geometry, including mask-aware staggered edge ownership.
 */

#include "tim_io_decomp.hpp"

#include "core/tim_error.hpp"

#include <string>
#include <vector>

namespace TIM {

IoDecomp::IoDecomp(const Spec& spec)
    : nig_(spec.nig),
      njg_(spec.njg),
      c_(spec.compute),
      symmetric_(spec.symmetric),
      layout_(spec.layout),
      io_layout_(spec.io_layout),
      tile_x_(spec.tile_x),
      tile_y_(spec.tile_y),
      tile_mask_(spec.tile_mask),
      comm_(spec.comm) {
  if (nig_ < 1 || njg_ < 1)
    fatal("global sizes must be positive, got " + std::to_string(nig_) +
              "x" + std::to_string(njg_));
  if (c_.empty())
    fatal("compute window is empty");
  if (c_.is < 1 || c_.ie > nig_ || c_.js < 1 || c_.je > njg_)
    fatal("compute window [" + std::to_string(c_.is) + ".." +
              std::to_string(c_.ie) + "]x[" + std::to_string(c_.js) + ".." +
              std::to_string(c_.je) + "] exceeds the " + std::to_string(nig_) +
              "x" + std::to_string(njg_) + " global grid");
  if (layout_.ntiles_x < 1 || layout_.ntiles_y < 1)
    fatal("layout must be at least 1x1");
  if (io_layout_.ntiles_x < 1 || io_layout_.ntiles_y < 1)
    fatal("io_layout must be at least 1x1");
  if (tile_x_ < 1 || tile_x_ > layout_.ntiles_x || tile_y_ < 1 ||
      tile_y_ > layout_.ntiles_y)
    fatal("tile (" + std::to_string(tile_x_) + "," +
              std::to_string(tile_y_) + ") outside the " +
              std::to_string(layout_.ntiles_x) + "x" + std::to_string(layout_.ntiles_y) +
              " layout");
  // The tile position must agree with the compute window: a rank sits on a
  // layout boundary exactly when its compute window reaches the global edge.
  // This is what writeComponents' edge ownership relies on, and it catches a
  // Spec whose layout/tile fields were left at their defaults for a subdomain
  // window (every rank would then claim the +1 edges — duplicate DOFs).
  if ((tile_x_ == 1) != (c_.is == 1) ||
      (tile_x_ == layout_.ntiles_x) != (c_.ie == nig_) ||
      (tile_y_ == 1) != (c_.js == 1) ||
      (tile_y_ == layout_.ntiles_y) != (c_.je == njg_))
    fatal("tile (" + std::to_string(tile_x_) + "," +
              std::to_string(tile_y_) + ") of the " +
              std::to_string(layout_.ntiles_x) + "x" + std::to_string(layout_.ntiles_y) +
              " layout disagrees with compute window [" +
              std::to_string(c_.is) + ".." + std::to_string(c_.ie) + "]x[" +
              std::to_string(c_.js) + ".." + std::to_string(c_.je) +
              "] on the " + std::to_string(nig_) + "x" + std::to_string(njg_) +
              " global grid");
  if (!tile_mask_.empty()) {
    if (tile_mask_.size() != (size_t)layout_.ntiles_x * layout_.ntiles_y)
      fatal("tile mask has " + std::to_string(tile_mask_.size()) +
                " entries for a " + std::to_string(layout_.ntiles_x) + "x" +
                std::to_string(layout_.ntiles_y) + " layout");
    if (!tileLive(tile_x_, tile_y_))
      fatal("this rank's tile (" + std::to_string(tile_x_) + "," +
                std::to_string(tile_y_) + ") is masked out");
  }
}

bool IoDecomp::tileLive(int tx, int ty) const {
  if (tx < 1 || tx > layout_.ntiles_x || ty < 1 || ty > layout_.ntiles_y) return false;
  if (tile_mask_.empty()) return true;
  return tile_mask_[(size_t)(ty - 1) * layout_.ntiles_x + (tx - 1)] != 0;
}

std::vector<IndexWindow> IoDecomp::readComponents(Stagger s) const {
  std::vector<IndexWindow> out;
  out.push_back(c_);
  if (plusX(s)) out.push_back({c_.ie + 1, c_.ie + 1, c_.js, c_.je});
  if (plusY(s)) out.push_back({c_.is, c_.ie, c_.je + 1, c_.je + 1});
  if (plusX(s) && plusY(s))
    out.push_back({c_.ie + 1, c_.ie + 1, c_.je + 1, c_.je + 1});
  return out;
}

std::vector<IndexWindow> IoDecomp::writeComponents(Stagger s) const {
  std::vector<IndexWindow> out;
  out.push_back(c_);
  const bool px = plusX(s), py = plusY(s);
  if (!px && !py) return out;

  const bool east = neighborLive(1, 0);
  const bool north = neighborLive(0, 1);
  const bool northeast = neighborLive(1, 1);
  const bool northwest = neighborLive(-1, 1);

  // East edge column: every point on it is outranked only by the east
  // neighbor (its south endpoint also touches the south/southeast tiles, but
  // both rank BELOW this tile), so one liveness test covers the whole line.
  if (px && !east) out.push_back({c_.ie + 1, c_.ie + 1, c_.js, c_.je});

  // North edge row: when the variable is x-staggered too, the row's WEST
  // endpoint also touches the northwest tile, which outranks this one
  // (north-most, then east-most), so a live northwest neighbor claims that
  // single point as part of its own east column. Without x-staggering the
  // point touches only this tile and the (dead) north one.
  if (py && !north) {
    const IndexWindow row{(px && northwest) ? c_.is + 1 : c_.is, c_.ie, c_.je + 1,
                     c_.je + 1};
    if (!row.empty()) out.push_back(row);
  }

  // NE corner point: owned by the first live tile in the order
  // [northeast, north, east, this]; a live north neighbor takes it via its
  // east column, a live east neighbor via its north row.
  if (px && py && !east && !north && !northeast)
    out.push_back({c_.ie + 1, c_.ie + 1, c_.je + 1, c_.je + 1});

  return out;
}

}  // namespace TIM
