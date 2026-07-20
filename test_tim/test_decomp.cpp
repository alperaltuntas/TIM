// TIM::IoDecomp unit tests: staggered window/component geometry for
// symmetric, non-symmetric, and masked PE layouts.
//
// The core property under test is the mask-aware write partition: across all
// live tiles, writeComponents() must cover every staggered global point that
// touches at least one live tile EXACTLY once, and points surrounded only by
// eliminated tiles exactly zero times — for every stagger, with and without
// symmetric +1 edges, for arbitrary elimination patterns (realistic masked
// tripolar layouts are the motivating case; the layouts here are small
// synthetic analogues).

#include "io/tim_io_decomp.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace {

using namespace TIM;

// A regular tiling of an nig x njg grid by an lx x ly layout (sizes chosen
// divisible in these tests), with an optional elimination mask.
struct TileGrid {
  int nig, njg;
  Layout lay;
  bool symmetric;
  std::vector<std::uint8_t> mask;  // empty = all live

  // Uniform tiling only; grids in these tests are chosen divisible, and the
  // checker in expectTruePartition() fails loudly if they are not.
  int cellsPerX() const { return nig / lay.ntiles_x; }
  int cellsPerY() const { return njg / lay.ntiles_y; }

  bool live(int tx, int ty) const {
    if (tx < 1 || tx > lay.ntiles_x || ty < 1 || ty > lay.ntiles_y) return false;
    if (mask.empty()) return true;
    return mask[(size_t)(ty - 1) * lay.ntiles_x + (tx - 1)] != 0;
  }

  IndexWindow computeOf(int tx, int ty) const {
    return {(tx - 1) * cellsPerX() + 1, tx * cellsPerX(),
            (ty - 1) * cellsPerY() + 1, ty * cellsPerY()};
  }

  IoDecomp tile(int tx, int ty) const {
    return IoDecomp(IoDecomp::Spec{.nig = nig,
                                   .njg = njg,
                                   .compute = computeOf(tx, ty),
                                   .symmetric = symmetric,
                                   .layout = lay,
                                   .tile_x = tx,
                                   .tile_y = ty,
                                   .tile_mask = mask});
  }

  int tileOfCell(int c, int per) const { return (c - 1) / per + 1; }

  // Does staggered point (i, j) touch any live tile? A point at a staggered
  // index touches cells {i-1, i} in a +1 direction and {i} otherwise.
  bool pointTouchesLive(int i, int j, Stagger s) const {
    const bool px = symmetric && staggeredX(s);
    const bool py = symmetric && staggeredY(s);
    for (int ci = px ? i - 1 : i; ci <= i; ++ci) {
      if (ci < 1 || ci > nig) continue;
      for (int cj = py ? j - 1 : j; cj <= j; ++cj) {
        if (cj < 1 || cj > njg) continue;
        if (live(tileOfCell(ci, cellsPerX()), tileOfCell(cj, cellsPerY())))
          return true;
      }
    }
    return false;
  }
};

// Paints every live tile's writeComponents onto the staggered global grid and
// checks the exactly-once/exactly-zero coverage property.
void expectTruePartition(const TileGrid& g, Stagger s) {
  ASSERT_EQ(g.nig % g.lay.ntiles_x, 0) << "test grids must tile evenly";
  ASSERT_EQ(g.njg % g.lay.ntiles_y, 0) << "test grids must tile evenly";
  const IoDecomp probe = [&] {
    for (int ty = 1; ty <= g.lay.ntiles_y; ++ty)
      for (int tx = 1; tx <= g.lay.ntiles_x; ++tx)
        if (g.live(tx, ty)) return g.tile(tx, ty);
    ADD_FAILURE() << "mask eliminates every tile";
    return IoDecomp();
  }();
  const int gnx = probe.globalNx(s), gny = probe.globalNy(s);
  std::vector<int> painted((size_t)gnx * gny, 0);

  for (int ty = 1; ty <= g.lay.ntiles_y; ++ty) {
    for (int tx = 1; tx <= g.lay.ntiles_x; ++tx) {
      if (!g.live(tx, ty)) continue;
      for (const IndexWindow& cw : g.tile(tx, ty).writeComponents(s)) {
        ASSERT_FALSE(cw.empty());
        ASSERT_GE(cw.is, 1);
        ASSERT_LE(cw.ie, gnx);
        ASSERT_GE(cw.js, 1);
        ASSERT_LE(cw.je, gny);
        for (int j = cw.js; j <= cw.je; ++j)
          for (int i = cw.is; i <= cw.ie; ++i)
            ++painted[(size_t)(j - 1) * gnx + (i - 1)];
      }
    }
  }

  for (int j = 1; j <= gny; ++j) {
    for (int i = 1; i <= gnx; ++i) {
      const int expected = g.pointTouchesLive(i, j, s) ? 1 : 0;
      ASSERT_EQ(painted[(size_t)(j - 1) * gnx + (i - 1)], expected)
          << "staggered point (" << i << "," << j << ") stagger "
          << (int)s << " sym " << g.symmetric << " layout " << g.lay.ntiles_x << "x"
          << g.lay.ntiles_y;
    }
  }
}

void expectTruePartitionAllStaggers(TileGrid g) {
  for (const bool sym : {true, false}) {
    g.symmetric = sym;
    for (const Stagger s : {Stagger::Center, Stagger::EastFace,
                            Stagger::NorthFace, Stagger::Corner})
      expectTruePartition(g, s);
  }
}

// Assert a component list equals the expected pieces, in order.
void expectPieces(const std::vector<IndexWindow>& got,
                  const std::vector<IndexWindow>& want) {
  ASSERT_EQ(got.size(), want.size());
  for (size_t k = 0; k < got.size(); ++k) EXPECT_EQ(got[k], want[k]) << "piece " << k;
}

TEST(Stagger, DirectionHelpers) {
  EXPECT_FALSE(staggeredX(Stagger::Center));
  EXPECT_FALSE(staggeredY(Stagger::Center));
  EXPECT_TRUE(staggeredX(Stagger::EastFace));
  EXPECT_FALSE(staggeredY(Stagger::EastFace));
  EXPECT_FALSE(staggeredX(Stagger::NorthFace));
  EXPECT_TRUE(staggeredY(Stagger::NorthFace));
  EXPECT_TRUE(staggeredX(Stagger::Corner));
  EXPECT_TRUE(staggeredY(Stagger::Corner));
}

TEST(IndexWindow, Basics) {
  const IndexWindow empty;
  EXPECT_TRUE(empty.empty());
  EXPECT_EQ(empty.npts(), 0);
  const IndexWindow w{2, 4, 5, 5};
  EXPECT_FALSE(w.empty());
  EXPECT_EQ(w.ni(), 3);
  EXPECT_EQ(w.nj(), 1);
  EXPECT_EQ(w.npts(), 3);
  EXPECT_TRUE(w.contains(3, 5));
  EXPECT_FALSE(w.contains(3, 4));
  EXPECT_EQ(w, (IndexWindow{2, 4, 5, 5}));
  EXPECT_NE(w, empty);
}

TEST(IoDecomp, DefaultIsInvalidPlaceholder) {
  const IoDecomp none;
  EXPECT_FALSE(none.valid());
}

TEST(IoDecomp, Accessors) {
  const IoDecomp d(IoDecomp::Spec{.nig = 12,
                                  .njg = 8,
                                  .compute = {5, 8, 1, 4},
                                  .symmetric = true,
                                  .layout = {3, 2},
                                  .io_layout = {1, 2},
                                  .tile_x = 2,
                                  .tile_y = 1});
  EXPECT_TRUE(d.valid());
  EXPECT_EQ(d.nig(), 12);
  EXPECT_EQ(d.njg(), 8);
  EXPECT_TRUE(d.symmetric());
  EXPECT_EQ(d.layout(), (Layout{3, 2}));
  EXPECT_EQ(d.ioLayout(), (Layout{1, 2}));
  EXPECT_EQ(d.tileX(), 2);
  EXPECT_EQ(d.tileY(), 1);
  EXPECT_EQ(d.compute(), (IndexWindow{5, 8, 1, 4}));
  EXPECT_EQ(d.comm(), MPI_COMM_NULL);
}

TEST(IoDecomp, GlobalStaggeredSizes) {
  const IoDecomp sym(IoDecomp::Spec{
      .nig = 9, .njg = 6, .compute = {1, 9, 1, 6}, .symmetric = true});
  EXPECT_EQ(sym.globalNx(Stagger::Center), 9);
  EXPECT_EQ(sym.globalNy(Stagger::Center), 6);
  EXPECT_EQ(sym.globalNx(Stagger::EastFace), 10);
  EXPECT_EQ(sym.globalNy(Stagger::EastFace), 6);
  EXPECT_EQ(sym.globalNx(Stagger::NorthFace), 9);
  EXPECT_EQ(sym.globalNy(Stagger::NorthFace), 7);
  EXPECT_EQ(sym.globalNx(Stagger::Corner), 10);
  EXPECT_EQ(sym.globalNy(Stagger::Corner), 7);

  const IoDecomp nonsym(IoDecomp::Spec{
      .nig = 9, .njg = 6, .compute = {1, 9, 1, 6}, .symmetric = false});
  for (const Stagger s : {Stagger::Center, Stagger::EastFace,
                          Stagger::NorthFace, Stagger::Corner}) {
    EXPECT_EQ(nonsym.globalNx(s), 9);
    EXPECT_EQ(nonsym.globalNy(s), 6);
    EXPECT_FALSE(nonsym.plusX(s));
    EXPECT_FALSE(nonsym.plusY(s));
  }
}

TEST(IoDecomp, WindowAndReadComponents) {
  // Interior tile of an unmasked 3x3 layout over 9x9: every rank's staggered
  // window carries the +1 edges, regardless of neighbors.
  const TileGrid g{9, 9, {3, 3}, true, {}};
  const IoDecomp d = g.tile(2, 2);  // compute 4..6 x 4..6
  EXPECT_EQ(d.staggeredWindow(Stagger::Center), (IndexWindow{4, 6, 4, 6}));
  EXPECT_EQ(d.staggeredWindow(Stagger::EastFace), (IndexWindow{4, 7, 4, 6}));
  EXPECT_EQ(d.staggeredWindow(Stagger::NorthFace), (IndexWindow{4, 6, 4, 7}));
  EXPECT_EQ(d.staggeredWindow(Stagger::Corner), (IndexWindow{4, 7, 4, 7}));

  const IndexWindow c = d.compute();  // {4, 6, 4, 6}
  expectPieces(d.readComponents(Stagger::Center), {c});
  expectPieces(d.readComponents(Stagger::EastFace), {c, {7, 7, 4, 6}});
  expectPieces(d.readComponents(Stagger::NorthFace), {c, {4, 6, 7, 7}});
  expectPieces(d.readComponents(Stagger::Corner),
               {c, {7, 7, 4, 6}, {4, 6, 7, 7}, {7, 7, 7, 7}});

  // Masking must NOT change what a live rank reads.
  TileGrid masked = g;
  masked.mask = {1, 1, 1, 1, 1, 0, 1, 1, 1};  // kill (3,2), east of (2,2)
  expectPieces(masked.tile(2, 2).readComponents(Stagger::Corner),
               d.readComponents(Stagger::Corner));
}

TEST(IoDecomp, WriteComponentsUnmasked) {
  const TileGrid g{9, 9, {3, 3}, true, {}};

  // Interior rank: compute block only — shared edges belong to the east/
  // north neighbors.
  expectPieces(g.tile(2, 2).writeComponents(Stagger::Corner), {{4, 6, 4, 6}});

  // Global-east rank (not north-most): compute + east boundary column.
  expectPieces(g.tile(3, 2).writeComponents(Stagger::Corner),
               {{7, 9, 4, 6}, {10, 10, 4, 6}});

  // Global-north rank; its northwest neighbor is outside the layout, so the
  // row spans the full compute width.
  expectPieces(g.tile(2, 3).writeComponents(Stagger::Corner),
               {{4, 6, 7, 9}, {4, 6, 10, 10}});

  // Global NE-corner rank: all four pieces.
  expectPieces(g.tile(3, 3).writeComponents(Stagger::Corner),
               {{7, 9, 7, 9}, {10, 10, 7, 9}, {7, 9, 10, 10}, {10, 10, 10, 10}});

  // Stagger direction gates the pieces.
  expectPieces(g.tile(3, 3).writeComponents(Stagger::EastFace),
               {{7, 9, 7, 9}, {10, 10, 7, 9}});
  expectPieces(g.tile(3, 3).writeComponents(Stagger::Center), {{7, 9, 7, 9}});

  // Non-symmetric grids have no +1 edges to own anywhere.
  TileGrid nonsym = g;
  nonsym.symmetric = false;
  expectPieces(nonsym.tile(3, 3).writeComponents(Stagger::Corner), {{7, 9, 7, 9}});
}

TEST(IoDecomp, WriteComponentsMaskedEdgeClaims) {
  // The geolat_c scenario: an eliminated interior tile. Its live west/south
  // neighbors claim the shared staggered edges the dead tile would have
  // written; the tile's interior stays unwritten (all-land, keeps file fill).
  TileGrid g{9, 9, {3, 3}, true, {}};
  g.mask = {1, 1, 1, 1, 0, 1, 1, 1, 1};  // kill center tile (2,2)

  // West neighbor claims the shared east column.
  expectPieces(g.tile(1, 2).writeComponents(Stagger::Corner),
               {{1, 3, 4, 6}, {4, 4, 4, 6}});

  // South neighbor claims the shared north row, EXCEPT its west endpoint:
  // the live northwest tile (1,2) outranks it and takes that point as part
  // of its own east column.
  expectPieces(g.tile(2, 1).writeComponents(Stagger::Corner),
               {{4, 6, 1, 3}, {5, 6, 4, 4}});

  // The southwest diagonal neighbor does NOT claim the dead tile's SW corner
  // point — the north and east neighbors outrank it.
  expectPieces(g.tile(1, 1).writeComponents(Stagger::Corner), {{1, 3, 1, 3}});

  // For an x-staggered variable only the column transfers.
  expectPieces(g.tile(1, 2).writeComponents(Stagger::EastFace),
               {{1, 3, 4, 6}, {4, 4, 4, 6}});
  expectPieces(g.tile(2, 1).writeComponents(Stagger::EastFace), {{4, 6, 1, 3}});

  // A y-only-staggered point never touches the northwest tile, so the south
  // neighbor's claimed row keeps its full compute width.
  expectPieces(g.tile(2, 1).writeComponents(Stagger::NorthFace),
               {{4, 6, 1, 3}, {4, 6, 4, 4}});

  // Kill the full dead cross around (1,1): now it owns its NE corner point.
  g.mask = {1, 0, 1, 0, 0, 1, 1, 1, 1};  // (2,1) and (1,2) dead too
  expectPieces(g.tile(1, 1).writeComponents(Stagger::Corner),
               {{1, 3, 1, 3}, {4, 4, 1, 3}, {1, 3, 4, 4}, {4, 4, 4, 4}});
}

TEST(IoDecomp, WritePartitionPropertyUnmasked) {
  expectTruePartitionAllStaggers({9, 9, {3, 3}, true, {}});
  expectTruePartitionAllStaggers({10, 8, {5, 4}, true, {}});
  expectTruePartitionAllStaggers({7, 5, {1, 1}, true, {}});  // single tile
}

TEST(IoDecomp, WritePartitionPropertyMasked) {
  // 3x3 layouts: single interior hole, dead NE corner, dead boundary column,
  // dead boundary row, checkerboard cross, single survivor.
  for (const std::vector<std::uint8_t>& mask :
       {std::vector<std::uint8_t>{1, 1, 1, 1, 0, 1, 1, 1, 1},
        std::vector<std::uint8_t>{1, 1, 1, 1, 1, 1, 1, 1, 0},
        std::vector<std::uint8_t>{1, 1, 0, 1, 1, 0, 1, 1, 0},
        std::vector<std::uint8_t>{1, 1, 1, 1, 1, 1, 0, 0, 0},
        std::vector<std::uint8_t>{1, 0, 1, 0, 1, 0, 1, 0, 1},
        std::vector<std::uint8_t>{0, 0, 0, 0, 1, 0, 0, 0, 0}})
    expectTruePartitionAllStaggers({9, 9, {3, 3}, true, mask});

  // A coastline-ish elimination pattern on a 5x4 layout.
  expectTruePartitionAllStaggers({10, 8,
                                  {5, 4},
                                  true,
                                  {1, 1, 1, 1, 0,  //
                                   1, 1, 1, 0, 0,  //
                                   1, 1, 1, 1, 0,  //
                                   0, 1, 1, 1, 1}});
}

TEST(IoDecomp, WritePartitionPropertySingleCellTiles) {
  // 1-cell-wide/-tall tiles collapse the edge pieces onto their endpoint
  // collisions: a dead north neighbor with a live northwest one leaves an
  // EMPTY claimed row on a 1-wide tile (the empty-piece guard), and every
  // shared point is a tile corner.
  expectTruePartitionAllStaggers({2, 2, {2, 2}, true, {1, 1, 1, 0}});
  expectTruePartitionAllStaggers({2, 2, {2, 2}, true, {1, 0, 0, 1}});
  for (const std::vector<std::uint8_t>& mask :
       {std::vector<std::uint8_t>{},
        std::vector<std::uint8_t>{1, 1, 1, 1, 0, 1, 1, 1, 1},
        std::vector<std::uint8_t>{1, 0, 1, 0, 1, 0, 1, 0, 1},
        std::vector<std::uint8_t>{1, 1, 0, 1, 1, 0, 1, 0, 0}})
    expectTruePartitionAllStaggers({3, 3, {3, 3}, true, mask});
  // Single-row and single-column layouts.
  expectTruePartitionAllStaggers({4, 1, {4, 1}, true, {1, 0, 1, 1}});
  expectTruePartitionAllStaggers({1, 4, {1, 4}, true, {1, 0, 1, 1}});
}

TEST(IoDecomp, TileLiveBounds) {
  TileGrid g{9, 9, {3, 3}, true, {1, 1, 1, 1, 0, 1, 1, 1, 1}};
  const IoDecomp d = g.tile(1, 1);
  EXPECT_TRUE(d.tileLive(1, 1));
  EXPECT_FALSE(d.tileLive(2, 2)) << "masked-out tile";
  EXPECT_FALSE(d.tileLive(0, 1)) << "outside the layout";
  EXPECT_FALSE(d.tileLive(4, 1));
  EXPECT_FALSE(d.tileLive(1, 4));
  const IoDecomp unmasked = TileGrid{9, 9, {3, 3}, true, {}}.tile(1, 1);
  EXPECT_TRUE(unmasked.tileLive(3, 3)) << "empty mask means all live";
}

}  // namespace
