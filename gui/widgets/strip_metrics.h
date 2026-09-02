#pragma once

namespace pipeeq::strip {

// Metrics shared by the two rows of strips: the sends above and the mixer
// channels below.
//
// They are deliberately the same width. The two rows are read together - a send
// level and the channel it feeds - and two columns of faders at slightly
// different widths looks like a mistake rather than a distinction. They were
// 74 px and 66 px respectively, which is exactly the sort of difference that is
// invisible until the two are scaled and drift apart.
inline constexpr int kBaseWidth = 66;

// The most a strip widens by on a roomy window. Some, but not much: past this
// the meters are all a strip has to show for the space, and a mixer with eight
// enormous channels reads worse than one with eight legible ones.
inline constexpr double kMaxWidthScale = 2.0;

// Fader width at scale 1. Scaled with the strip, so the proportions hold and
// the grip gets easier to hit rather than staying a thin column in a wide
// strip.
inline constexpr int kFaderWidth = 24;

// Spacing between the sends column and the EQ beside it, and between the send
// strips themselves.
inline constexpr int kBodySpacing = 10;
inline constexpr int kSendSpacing = 5;

// The EQ curve never gets less than this share of the panel's width. Below
// roughly this a curve stops being readable as a shape, which is the only thing
// a preview is for.
inline constexpr double kEqMinimumShare = 0.40;

// A strip and a half. Stops the arithmetic below from collapsing the column on
// an absurdly narrow panel; well below any width the window permits.
inline constexpr int kMinimumSendsWidth = 110;

struct SendsPlan {
    double scale; // applied to every send strip
    int width;    // the sends column's width
};

// How wide the sends column is, and how much its strips may widen.
//
// The sends match the mixer strips below by default - two rows of faders at
// different widths reads as a mistake rather than a distinction - but the sends
// grow with the number of inputs while the EQ does not. So the EQ keeps
// kEqMinimumShare and the sends take the requested scale only as far as what is
// left allows, falling back towards their natural width rather than being
// clipped.
//
// Pure, and separated out for that reason: it is arithmetic with a boundary
// that only bites at input counts and window sizes a screenshot is unlikely to
// be taken at.
inline SendsPlan planSends(int panelWidth, int horizontalMargins, int sendCount, int headerWidth,
                            double requestedScale) {
    const int body = panelWidth - horizontalMargins - kBodySpacing;
    const int allowed = body - static_cast<int>(panelWidth * kEqMinimumShare + 0.5);

    double scale = requestedScale < 1.0 ? 1.0 : requestedScale;
    if (scale > kMaxWidthScale) {
        scale = kMaxWidthScale;
    }
    if (sendCount > 0 && allowed > 0) {
        const int spacings = kSendSpacing * (sendCount - 1);
        const double natural = static_cast<double>(sendCount) * kBaseWidth;
        // Never below natural: past that the answer is to scroll the row, which
        // it already does, not to shrink the strips into illegibility.
        const double fits = (allowed - spacings) / natural;
        if (fits < scale) {
            scale = fits < 1.0 ? 1.0 : fits;
        }
    }

    const int stripsWidth =
        sendCount > 0
            ? sendCount * static_cast<int>(kBaseWidth * scale + 0.5) + kSendSpacing * (sendCount - 1)
            : 0;
    int width = stripsWidth > headerWidth ? stripsWidth : headerWidth;
    if (allowed > 0 && width > allowed) {
        width = allowed;
    }
    if (width < kMinimumSendsWidth) {
        width = kMinimumSendsWidth;
    }
    if (body > kMinimumSendsWidth && width > body) {
        width = body;
    }
    return {scale, width};
}

} // namespace pipeeq::strip
