#pragma once
/**
 * @file tim_io_decomp.hpp
 * @brief TIM::IoDecomp — per-compute-rank I/O decomposition geometry (value type).
 *
 * The single vocabulary every TIM I/O and diag component uses for "where is
 * my slab of a 2-D field in global file index space". Deliberately DECOUPLED
 * from the computational domain: FMS tangled domain2D through its I/O and
 * diagnostics; in TIM those layers see only this value type (and the DofMaps
 * built from it), never the model's domain machinery. The values are PRODUCED
 * elsewhere (today at the Fortran boundary, from FMS mpp metadata) and only
 * CONSUMED here. All indices are 1-based global (Fortran convention); consumers
 * convert.
 *
 * This module provides GEOMETRY ONLY. DOF-list generation (the flat offsets
 * PIO decompositions are built from) lives in a separate DOF-map layer, which
 * consumes the staggeredWindow / readComponents / writeComponents families
 * below. No PIO, no netCDF here; the MPI communicator is carried as opaque
 * metadata (no MPI calls are made).
 *
 * Masked PE layouts are first-class geometry: an IoDecomp knows the PE layout,
 * this rank's tile coordinates in it, and which tiles are eliminated
 * (all-land, no rank assigned). Shared staggered edges bordering eliminated
 * tiles are claimed by a live neighbor (see writeComponents) instead of
 * silently becoming fill holes in written files.
 *
 * Collectivity: every method is rank-local; nothing here communicates.
 */

#include <mpi.h>

#include <cstdint>
#include <vector>

namespace TIM {

/// @brief Staggering of a variable relative to h-points.
enum class Stagger : int {
  Center = 0,     ///< h-points (cell centers).
  EastFace = 1,   ///< u-points (east cell faces).
  NorthFace = 2,  ///< v-points (north cell faces).
  Corner = 3      ///< q-points (cell corners).
};

/// @brief Is the stagger offset in x (u/q-points)?
/// @param s The stagger.
/// @return true for EastFace and Corner.
constexpr bool staggeredX(Stagger s) {
  return s == Stagger::EastFace || s == Stagger::Corner;
}

/// @brief Is the stagger offset in y (v/q-points)?
/// @param s The stagger.
/// @return true for NorthFace and Corner.
constexpr bool staggeredY(Stagger s) {
  return s == Stagger::NorthFace || s == Stagger::Corner;
}

/// @brief A rectangular index window, 1-based inclusive global indices.
struct IndexWindow {
  int is = 1;  ///< First i index (inclusive).
  int ie = 0;  ///< Last i index (inclusive).
  int js = 1;  ///< First j index (inclusive).
  int je = 0;  ///< Last j index (inclusive).

  /// @brief Extent in i.
  /// @return Number of columns.
  constexpr int ni() const { return ie - is + 1; }

  /// @brief Extent in j.
  /// @return Number of rows.
  constexpr int nj() const { return je - js + 1; }

  /// @brief Does the window contain no points?
  /// @return true when either extent is empty.
  constexpr bool empty() const { return ie < is || je < js; }

  /// @brief Number of points in the window.
  /// @return ni()*nj(), or 0 when empty.
  constexpr long long npts() const {
    return empty() ? 0 : (long long)ni() * nj();
  }

  /// @brief Is (i, j) inside the window?
  /// @param i Global i index.
  /// @param j Global j index.
  /// @return true when both indices are within the window.
  constexpr bool contains(int i, int j) const {
    return i >= is && i <= ie && j >= js && j <= je;
  }

  /// @brief Member-wise equality.
  friend constexpr bool operator==(const IndexWindow&, const IndexWindow&) = default;
};

/// @brief A processor grid shape: PE layout or I/O layout.
struct Layout {
  int ntiles_x = 1;  ///< Number of tiles in x.
  int ntiles_y = 1;  ///< Number of tiles in y.

  /// @brief Member-wise equality.
  friend constexpr bool operator==(const Layout&, const Layout&) = default;
};

/// @brief Where one compute rank's slab of a 2-D field lives in global file
/// (I/O) index space (value type).
class IoDecomp {
 public:
  /// @brief Aggregate constructor input; designated initializers keep call
  /// sites readable.
  ///
  /// Geometry is validated by the IoDecomp constructor; in particular, the
  /// tile coordinates must be consistent with the compute window (a rank is
  /// on a layout boundary exactly when its window reaches the global edge),
  /// so a producer cannot leave layout/tile_x/tile_y at their defaults for a
  /// subdomain window.
  struct Spec {
    int nig = 0;             ///< Global center-grid size in x.
    int njg = 0;             ///< Global center-grid size in y.
    IndexWindow compute;     ///< This rank's compute window.
    bool symmetric = false;  ///< Symmetric memory model (staggered +1 edges).
    Layout layout{1, 1};     ///< PE layout (tiles in x, y).
    Layout io_layout{1, 1};  ///< I/O layout hint (iotask policy input).
    int tile_x = 1;          ///< This rank's tile x coordinate, 1-based.
    int tile_y = 1;          ///< This rank's tile y coordinate, 1-based.
    /// @brief layout.ntiles_x*layout.ntiles_y entries, x-fastest; nonzero = tile is live
    /// (has a rank). Empty means every tile is live (unmasked layout).
    std::vector<std::uint8_t> tile_mask;
    MPI_Comm comm = MPI_COMM_NULL;  ///< Carried, never used to communicate.
  };

  /// @brief Default-built IoDecomp is the invalid "no decomposition" placeholder.
  IoDecomp() = default;

  /// @brief Validates the spec (extents within the global grid, tile within
  /// layout and consistent with the compute window, mask sized to the
  /// layout, own tile live); amrex::Abort on inconsistency.
  /// @param spec The geometry to adopt.
  explicit IoDecomp(const Spec& spec);

  /// @brief Was this built from a Spec (as opposed to default-built)?
  /// @return true for a usable decomposition.
  bool valid() const { return nig_ > 0; }

  /// @brief Global center-grid size in x.
  /// @return Number of cells.
  int nig() const { return nig_; }

  /// @brief Global center-grid size in y.
  /// @return Number of cells.
  int njg() const { return njg_; }

  /// @brief Symmetric memory model (staggered +1 edges)?
  /// @return true for symmetric decompositions.
  bool symmetric() const { return symmetric_; }

  /// @brief The PE (compute) layout.
  /// @return Tiles in x, y.
  Layout layout() const { return layout_; }

  /// @brief The I/O layout hint.
  /// @return I/O groups in x, y.
  Layout ioLayout() const { return io_layout_; }

  /// @brief This rank's tile x coordinate (1-based).
  /// @return Tile column in the layout.
  int tileX() const { return tile_x_; }

  /// @brief This rank's tile y coordinate (1-based).
  /// @return Tile row in the layout.
  int tileY() const { return tile_y_; }

  /// @brief The communicator of the ranks sharing this decomposition (opaque metadata).
  /// @return The handle supplied at construction.
  MPI_Comm comm() const { return comm_; }

  /// @brief Is the tile at 1-based layout coordinates (tx, ty) live?
  ///
  /// Coordinates outside the layout are not live (the global boundary
  /// behaves like an eliminated neighbor).
  /// @param tx Tile column.
  /// @param ty Tile row.
  /// @return true for an in-layout, non-eliminated tile.
  bool tileLive(int tx, int ty) const;

  /// @brief +1 point in x for this stagger on a symmetric grid?
  /// @param s The stagger.
  /// @return true when symmetric and staggeredX(s).
  bool plusX(Stagger s) const { return symmetric_ && staggeredX(s); }

  /// @brief +1 point in y for this stagger on a symmetric grid?
  /// @param s The stagger.
  /// @return true when symmetric and staggeredY(s).
  bool plusY(Stagger s) const { return symmetric_ && staggeredY(s); }

  /// @brief Global axis size in x for a variable at this stagger.
  /// @param s The stagger.
  /// @return nig(), +1 on symmetric x-staggered grids.
  int globalNx(Stagger s) const { return nig_ + (plusX(s) ? 1 : 0); }

  /// @brief Global axis size in y for a variable at this stagger.
  /// @param s The stagger.
  /// @return njg(), +1 on symmetric y-staggered grids.
  int globalNy(Stagger s) const { return njg_ + (plusY(s) ? 1 : 0); }

  /// @brief This rank's compute window on the CENTER grid.
  /// @return The compute extents.
  IndexWindow compute() const { return c_; }

  /// @brief The full staggered window this rank's arrays cover:
  /// [isc..iec+px] x [jsc..jec+py] in staggered file indices.
  ///
  /// Windows of neighboring ranks OVERLAP at shared edges (the FMS convention);
  /// use readComponents() / writeComponents() for maps PIO will accept.
  /// @param s The stagger.
  /// @return The staggered window.
  IndexWindow staggeredWindow(Stagger s) const {
    return {c_.is, c_.ie + (plusX(s) ? 1 : 0), c_.js, c_.je + (plusY(s) ? 1 : 0)};
  }

  // The component methods below are meaningful only on a valid() IoDecomp;
  // on the default-built placeholder they return a single empty window.

  /// @brief Up to 4 STRICTLY DISJOINT pieces whose union is staggeredWindow(s):
  /// compute block, east strip, north strip, NE point, in that fixed order.
  ///
  /// PIO decompositions cannot express the overlapping window directly (it is
  /// rejected with EINVAL, and corrupts the heap under the BOX rearranger), so
  /// reads fetch these pieces — each piece index forms its own non-overlapping
  /// global map. Independent of the tile mask: a live rank always reads its
  /// whole window.
  /// @param s The stagger.
  /// @return The disjoint read pieces (1..4).
  std::vector<IndexWindow> readComponents(Stagger s) const;

  /// @brief Up to 4 STRICTLY DISJOINT pieces this rank WRITES: compute
  /// block, then — on symmetric staggered grids — any +1 edge lines it owns,
  /// in the fixed order [compute, east column, north row, NE corner point].
  ///
  /// Across all live ranks the pieces form a TRUE PARTITION of every
  /// staggered global point that touches at least one live tile (each
  /// written exactly once; points surrounded only by eliminated tiles are
  /// written by nobody and keep the file fill value).
  ///
  /// Ownership rule: a shared staggered point belongs to its compute-window
  /// owner if that tile is live; otherwise to the north-most, then east-most,
  /// LIVE tile that can reach the point through its own edge pieces (east
  /// column / north row / NE corner). Tiles outside the layout or masked out
  /// are dead. The edge-piece qualifier matters at a claimed point's endpoints:
  /// e.g. a north row's east endpoint with a dead north neighbor and a live NE
  /// neighbor stays with this tile, because NE's pieces cannot reach it — so
  /// the plain "north-most-then-east-most" order alone would mis-assign it. On
  /// an unmasked layout this reduces to the classic convention (interior shared
  /// edges belong to the east/north neighbor's compute block; the global
  /// east/north-most ranks write the +1 boundary lines). On masked layouts,
  /// edges bordering eliminated tiles fall to the live neighbor — the mask-aware
  /// edge ownership that prevents write holes at eliminated-tile borders (e.g.
  /// geolat_c).
  /// @param s The stagger.
  /// @return The disjoint write pieces (1..4).
  std::vector<IndexWindow> writeComponents(Stagger s) const;

 private:
  // Liveness of the tile offset (dx, dy) from this rank's tile.
  bool neighborLive(int dx, int dy) const {
    return tileLive(tile_x_ + dx, tile_y_ + dy);
  }

  int nig_ = 0, njg_ = 0;
  IndexWindow c_;
  bool symmetric_ = false;
  Layout layout_{1, 1};
  Layout io_layout_{1, 1};
  int tile_x_ = 1, tile_y_ = 1;
  std::vector<std::uint8_t> tile_mask_;
  MPI_Comm comm_ = MPI_COMM_NULL;
};

}  // namespace TIM
