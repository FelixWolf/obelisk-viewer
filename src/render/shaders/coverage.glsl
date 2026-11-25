// Alpha masks. Multisampled opaque draws (ALPHA_TO_COVERAGE defined) turn
// the mask's edge into how many of the pixel's samples are covered, so it's
// antialiased as edges of geometry are; others just cut it.

// How much of the pixel a mask shows (at most 0: none of it), by its alpha
// and its cutoff (not over 0: no mask).
float maskCoverage(float alpha, float cutoff)
{
#ifdef ALPHA_TO_COVERAGE
    // The edge a pixel wide, whatever the texture's scale.
    const float width = max(fwidth(alpha), 1e-4);
    return cutoff <= 0.0 ? 1.0 : clamp((alpha - cutoff) / width + 0.5, 0.0, 1.0);
#else
    return alpha < cutoff ? 0.0 : 1.0;
#endif
}

// The alpha to write: to blend by, or the coverage.
float outputAlpha(float alpha, float coverage)
{
#ifdef ALPHA_TO_COVERAGE
    return coverage;
#else
    return alpha;
#endif
}
