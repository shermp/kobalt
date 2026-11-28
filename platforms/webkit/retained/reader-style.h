#pragma once

namespace retained {
inline constexpr char readerStyle[] = R"CSS(
    html { overflow: hidden !important; margin: 0 !important; background: white !important;
           font-size: var(--reader-font-size, 18px) !important; }
    body {
        box-sizing: border-box !important;
        margin: 0 !important;
        padding: 0 !important;
        height: 100vh !important;
        width: 100vw !important;
        column-width: 100vw !important;
        column-gap: 0 !important;
        column-fill: auto !important;
        font-size: var(--reader-font-size, 18px) !important;
    }
    img, svg { max-width: 100% !important; max-height: calc(100vh - 1em) !important; }
)CSS";
inline constexpr char policy[] =
        "default-src 'none'; style-src 'unsafe-inline' kobalt-epub:; "
        "img-src kobalt-epub:; font-src kobalt-epub:; base-uri 'none'; form-action 'none'; "
        "frame-ancestors 'none'";
} // namespace retained
