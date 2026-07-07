#pragma once
// TIM::Decomp2D — decomposition-metadata value type (prototype design pass).
//
// The single vocabulary every TIM I/O component uses for "where is my data".
// Constructed once at the Fortran boundary from FMS mpp metadata; later it can
// be produced from AMReX distribution maps without touching any consumer.
// All indices are 1-based global (Fortran convention); consumers convert.

namespace TIM {

// Staggering of a variable relative to h-points. The only currency for grid
// position inside TIM; FMS position flags are mapped at the Fortran boundary.
enum class Stagger : int { Center = 0, EastFace = 1, NorthFace = 2, Corner = 3 };

inline bool staggeredX(Stagger s) {
  return s == Stagger::EastFace || s == Stagger::Corner;
}
inline bool staggeredY(Stagger s) {
  return s == Stagger::NorthFace || s == Stagger::Corner;
}

// A rank's rectangular index window, 1-based inclusive global indices.
struct Window {
  int is = 1, ie = 0, js = 1, je = 0;
  int ni() const { return ie - is + 1; }
  int nj() const { return je - js + 1; }
  long long npts() const { return (long long)ni() * nj(); }
  bool empty() const { return ie < is || je < js; }
};

class Decomp2D {
 public:
  Decomp2D() = default;
  Decomp2D(int nig, int njg, int isc, int iec, int jsc, int jec, bool symmetric)
      : nig_(nig), njg_(njg), c_{isc, iec, jsc, jec}, symmetric_(symmetric) {}

  int nig() const { return nig_; }
  int njg() const { return njg_; }
  bool symmetric() const { return symmetric_; }

  // +1 in x/y for this stagger on a symmetric grid?
  bool plusX(Stagger s) const { return symmetric_ && staggeredX(s); }
  bool plusY(Stagger s) const { return symmetric_ && staggeredY(s); }

  // Global axis sizes for a variable at this stagger.
  int globalNx(Stagger s) const { return nig_ + (plusX(s) ? 1 : 0); }
  int globalNy(Stagger s) const { return njg_ + (plusY(s) ? 1 : 0); }

  // This rank's compute window on the CENTER grid.
  Window center() const { return {c_.is, c_.ie, c_.js, c_.je}; }

  // The full staggered window this rank's arrays cover: [isc..iec+px] x
  // [jsc..jec+py] in staggered file indices. Windows of neighboring ranks
  // OVERLAP at shared edges (the FMS convention); use readComponents() /
  // writePartition() for maps PIO will accept.
  Window window(Stagger s) const {
    return {c_.is, c_.ie + (plusX(s) ? 1 : 0), c_.js, c_.je + (plusY(s) ? 1 : 0)};
  }

  // Up to 4 STRICTLY DISJOINT pieces whose union is window(s): main block,
  // east strip, north strip, NE point. PIO decompositions cannot express the
  // overlapping window directly (BOX rearranger heap-corrupts; verified in
  // prototype/pio_spike/probe_overlap.cpp), so reads fetch these pieces.
  // Returns the number of valid components written into out[0..3].
  int readComponents(Stagger s, Window out[4]) const {
    int n = 0;
    out[n++] = center();
    if (plusX(s)) out[n++] = {c_.ie + 1, c_.ie + 1, c_.js, c_.je};
    if (plusY(s)) out[n++] = {c_.is, c_.ie, c_.je + 1, c_.je + 1};
    if (plusX(s) && plusY(s))
      out[n++] = {c_.ie + 1, c_.ie + 1, c_.je + 1, c_.je + 1};
    return n;
  }

  // A TRUE PARTITION of the staggered global grid for writes (every point
  // exactly once): each rank writes its center block, and the east/north-most
  // ranks additionally write the +1 edge line.
  Window writePartition(Stagger s) const {
    return {c_.is, c_.ie + ((plusX(s) && c_.ie == nig_) ? 1 : 0),
            c_.js, c_.je + ((plusY(s) && c_.je == njg_) ? 1 : 0)};
  }

 private:
  int nig_ = 0, njg_ = 0;
  Window c_;
  bool symmetric_ = false;
};

}  // namespace TIM
