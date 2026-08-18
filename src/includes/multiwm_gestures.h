//=============================================================================
// Move/size gesture tracking + classification
//=============================================================================



enum class MoveSizeIntent {
    None,
    Move,
    Resize,
};

struct MoveSizeGesture {
  MoveSizeIntent intent = MoveSizeIntent::None;
  RECT start{};
  RECT end{};
  POINT dropPoint{};
  bool hasRects = false;
  bool hasDropPoint = false;
};

static inline bool Differs(LONG a, LONG b, LONG tol = 1) {
    return (a > b) ? (a - b > tol) : (b - a > tol);
}

// Classify the user's *gesture*, not whether position and size both happened to
// change. WinUI/DWM windows can perturb one frame edge slightly during a pure
// move, while a legitimate top/left-edge resize necessarily changes position
// and size together. Opposite edges moving together is therefore the stronger
// signal for translation.
MoveSizeIntent ClassifyMoveSizeIntent(const RECT& before, const RECT& after, LONG tol = 1) {
    const LONG dl = after.left - before.left;
    const LONG dr = after.right - before.right;
    const LONG dt = after.top - before.top;
    const LONG db = after.bottom - before.bottom;

    const LONG maxEdgeDelta = std::max(
        std::max(std::abs(dl), std::abs(dr)),
        std::max(std::abs(dt), std::abs(db)));
    if (maxEdgeDelta <= tol) {
        Wh_Log(L"Move/size intent: none");
        return MoveSizeIntent::None;
    }

    // Twice the center displacement versus actual width/height deformation.
    // Use 64-bit intermediates so adding two LONG deltas cannot overflow.
    const long long moveScore = std::max(
        std::llabs(static_cast<long long>(dl) + dr),
        std::llabs(static_cast<long long>(dt) + db));
    const long long resizeScore = std::max(
        std::llabs(static_cast<long long>(dr) - dl),
        std::llabs(static_cast<long long>(db) - dt));

    // A translation must dominate deformation by a healthy margin. A normal
    // one-edge resize produces roughly equal scores, so it remains Resize.
    if (moveScore > resizeScore * 2) {
        Wh_Log(L"Move/size intent: move (move=%lld resize=%lld)", moveScore, resizeScore);
        return MoveSizeIntent::Move;
    }

    Wh_Log(L"Move/size intent: resize (move=%lld resize=%lld)", moveScore, resizeScore);
    return MoveSizeIntent::Resize;
}






