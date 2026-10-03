// Material 3 colour science, self-contained.
//
// a port of Google's material-color-utilities: CAM16, the HCT solver, tonal and
// core palettes. no ImGui dependency, so the maths unit-tests without a context.
// operates on packed ARGB (`0xAARRGGBB`) like upstream, so hexes match the
// official Material Theme Builder output.

#ifndef IMGUI_M3_COLOR_H_INCLUDED
#define IMGUI_M3_COLOR_H_INCLUDED

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace ImGuiM3Palette
{
    inline double signum(double v) { return v < 0.0 ? -1.0 : (v == 0.0 ? 0.0 : 1.0); }

    inline double sanitizeDegrees(double d)
    {
        d = std::fmod(d, 360.0);
        return d < 0.0 ? d + 360.0 : d;
    }

    inline int clampInt(int min, int max, int v) { return v < min ? min : (v > max ? max : v); }

    // sRGB <-> linear, both on a 0-255 input scale, matching upstream.
    inline double linearized(uint8_t component)
    {
        const double normalized = (double)component / 255.0;
        if (normalized <= 0.040449936)
            return normalized / 12.92 * 100.0;
        return std::pow((normalized + 0.055) / 1.055, 2.4) * 100.0;
    }

    inline uint8_t delinearized(double component)
    {
        const double normalized = component / 100.0;
        double out;
        if (normalized <= 0.0031308)
            out = normalized * 12.92;
        else
            out = 1.055 * std::pow(normalized, 1.0 / 2.4) - 0.055;
        return (uint8_t)clampInt(0, 255, (int)std::lround(out * 255.0));
    }

    inline uint32_t argbFromRgb(int r, int g, int b)
    {
        return 0xFF000000u | ((uint32_t)clampInt(0, 255, r) << 16) | ((uint32_t)clampInt(0, 255, g) << 8) |
               (uint32_t)clampInt(0, 255, b);
    }

    inline uint8_t redFromArgb(uint32_t argb)   { return (uint8_t)((argb >> 16) & 0xFF); }
    inline uint8_t greenFromArgb(uint32_t argb) { return (uint8_t)((argb >> 8) & 0xFF); }
    inline uint8_t blueFromArgb(uint32_t argb)  { return (uint8_t)(argb & 0xFF); }
    inline uint8_t alphaFromArgb(uint32_t argb) { return (uint8_t)((argb >> 24) & 0xFF); }

    inline uint32_t argbFromLinrgb(const double linrgb[3])
    {
        return argbFromRgb(delinearized(linrgb[0]), delinearized(linrgb[1]), delinearized(linrgb[2]));
    }

    // L* helpers. Tone in HCT is CIE L*, not HSL lightness or Y.
    inline double labF(double t)
    {
        const double e = 216.0 / 24389.0;
        const double kappa = 24389.0 / 27.0;
        if (t > e)
            return std::pow(t, 1.0 / 3.0);
        return (kappa * t + 16.0) / 116.0;
    }

    inline double labInvf(double ft)
    {
        const double e = 216.0 / 24389.0;
        const double kappa = 24389.0 / 27.0;
        const double ft3 = ft * ft * ft;
        if (ft3 > e)
            return ft3;
        return (116.0 * ft - 16.0) / kappa;
    }

    inline double yFromLstar(double lstar) { return 100.0 * labInvf((lstar + 16.0) / 116.0); }
    inline double lstarFromY(double y)     { return labF(y / 100.0) * 116.0 - 16.0; }

    inline uint32_t argbFromLstar(double lstar)
    {
        const uint8_t component = delinearized(yFromLstar(lstar));
        return argbFromRgb(component, component, component);
    }

    inline double lstarFromArgb(uint32_t argb)
    {
        // Only the Y row of sRGB -> XYZ is needed.
        const double r = linearized(redFromArgb(argb));
        const double g = linearized(greenFromArgb(argb));
        const double b = linearized(blueFromArgb(argb));
        return lstarFromY(0.2126 * r + 0.7152 * g + 0.0722 * b);
    }

    // CAM16 default viewing conditions for sRGB / D65 / average surround.
    struct ViewingConditions
    {
        double n, aw, nbb, ncb, c, nc, rgbD[3], fl, fLRoot, z;
    };

    inline ViewingConditions MakeDefaultViewingConditions()
    {
        static const double kPi = 3.14159265358979323846;
        const double whitePoint[3] = {95.047, 100.0, 108.883};
        const double adaptingLuminance = (200.0 / kPi) * yFromLstar(50.0) / 100.0;
        const double backgroundLstar = 50.0;
        const double surround = 2.0;

        const double rW = whitePoint[0] * 0.401288 + whitePoint[1] * 0.650173 + whitePoint[2] * -0.051461;
        const double gW = whitePoint[0] * -0.250268 + whitePoint[1] * 1.204414 + whitePoint[2] * 0.045854;
        const double bW = whitePoint[0] * -0.002079 + whitePoint[1] * 0.048952 + whitePoint[2] * 0.953127;

        const double f = 0.8 + surround / 10.0;
        const double c = (f >= 0.9) ? 0.59 + (0.69 - 0.59) * ((f - 0.9) * 10.0)
                                    : 0.525 + (0.59 - 0.525) * ((f - 0.8) * 10.0);
        double d = f * (1.0 - (1.0 / 3.6) * std::exp((-adaptingLuminance - 42.0) / 92.0));
        d = d > 1.0 ? 1.0 : (d < 0.0 ? 0.0 : d);

        ViewingConditions vc;
        vc.c = c;
        vc.nc = f;
        vc.n = yFromLstar(backgroundLstar) / whitePoint[1];
        vc.rgbD[0] = d * (100.0 / rW) + 1.0 - d;
        vc.rgbD[1] = d * (100.0 / gW) + 1.0 - d;
        vc.rgbD[2] = d * (100.0 / bW) + 1.0 - d;

        const double k = 1.0 / (5.0 * adaptingLuminance + 1.0);
        const double k4 = k * k * k * k;
        const double k4F = 1.0 - k4;
        vc.fl = k4 * adaptingLuminance + 0.1 * k4F * k4F * std::cbrt(5.0 * adaptingLuminance);
        vc.z = 1.48 + std::sqrt(vc.n);
        vc.nbb = 0.725 / std::pow(vc.n, 0.2);
        vc.ncb = vc.nbb;

        const double rgbAFactors[3] = {
            std::pow((vc.fl * vc.rgbD[0] * rW) / 100.0, 0.42),
            std::pow((vc.fl * vc.rgbD[1] * gW) / 100.0, 0.42),
            std::pow((vc.fl * vc.rgbD[2] * bW) / 100.0, 0.42)};
        const double rgbA[3] = {
            (400.0 * rgbAFactors[0]) / (rgbAFactors[0] + 27.13),
            (400.0 * rgbAFactors[1]) / (rgbAFactors[1] + 27.13),
            (400.0 * rgbAFactors[2]) / (rgbAFactors[2] + 27.13)};

        vc.aw = (2.0 * rgbA[0] + rgbA[1] + 0.05 * rgbA[2]) * vc.nbb;
        vc.fLRoot = std::pow(vc.fl, 0.25);
        return vc;
    }

    inline const ViewingConditions& DefaultViewingConditions()
    {
        static const ViewingConditions vc = MakeDefaultViewingConditions();
        return vc;
    }

    struct Cam16
    {
        double hue = 0.0, chroma = 0.0, j = 0.0, q = 0.0, m = 0.0, s = 0.0, jstar = 0.0, astar = 0.0, bstar = 0.0;

        static Cam16 FromInt(uint32_t argb)
        {
            static const double kPi = 3.14159265358979323846;
            const ViewingConditions& vc = DefaultViewingConditions();
            const double redL   = linearized(redFromArgb(argb));
            const double greenL = linearized(greenFromArgb(argb));
            const double blueL  = linearized(blueFromArgb(argb));
            const double x = 0.41233895 * redL + 0.35762064 * greenL + 0.18051042 * blueL;
            const double y = 0.2126 * redL + 0.7152 * greenL + 0.0722 * blueL;
            const double z = 0.01932141 * redL + 0.11916382 * greenL + 0.95034478 * blueL;

            const double rC = 0.401288 * x + 0.650173 * y - 0.051461 * z;
            const double gC = -0.250268 * x + 1.204414 * y + 0.045854 * z;
            const double bC = -0.002079 * x + 0.048952 * y + 0.953127 * z;

            const double rD = vc.rgbD[0] * rC;
            const double gD = vc.rgbD[1] * gC;
            const double bD = vc.rgbD[2] * bC;

            const double rAF = std::pow((vc.fl * std::fabs(rD)) / 100.0, 0.42);
            const double gAF = std::pow((vc.fl * std::fabs(gD)) / 100.0, 0.42);
            const double bAF = std::pow((vc.fl * std::fabs(bD)) / 100.0, 0.42);

            const double rA = (signum(rD) * 400.0 * rAF) / (rAF + 27.13);
            const double gA = (signum(gD) * 400.0 * gAF) / (gAF + 27.13);
            const double bA = (signum(bD) * 400.0 * bAF) / (bAF + 27.13);

            const double a = (11.0 * rA + -12.0 * gA + bA) / 11.0;
            const double b = (rA + gA - 2.0 * bA) / 9.0;
            const double u = (20.0 * rA + 20.0 * gA + 21.0 * bA) / 20.0;
            const double p2 = (40.0 * rA + 20.0 * gA + bA) / 20.0;
            const double hue = sanitizeDegrees((std::atan2(b, a) * 180.0) / kPi);

            Cam16 out;
            out.hue = hue;
            const double ac = p2 * vc.nbb;
            out.j = 100.0 * std::pow(ac / vc.aw, vc.c * vc.z);
            out.q = (4.0 / vc.c) * std::sqrt(out.j / 100.0) * (vc.aw + 4.0) * vc.fLRoot;
            const double huePrime = (hue < 20.14) ? hue + 360.0 : hue;
            const double eHue = 0.25 * (std::cos((huePrime * kPi) / 180.0 + 2.0) + 3.8);
            const double p1 = (50000.0 / 13.0) * eHue * vc.nc * vc.ncb;
            const double t = (p1 * std::sqrt(a * a + b * b)) / (u + 0.305);
            const double alpha = std::pow(t, 0.9) * std::pow(1.64 - std::pow(0.29, vc.n), 0.73);
            out.chroma = alpha * std::sqrt(out.j / 100.0);
            out.m = out.chroma * vc.fLRoot;
            out.s = 50.0 * std::sqrt((alpha * vc.c) / (vc.aw + 4.0));
            const double hueRadians = (hue * kPi) / 180.0;
            out.jstar = ((1.0 + 100.0 * 0.007) * out.j) / (1.0 + 0.007 * out.j);
            const double mstar = (1.0 / 0.0228) * std::log(1.0 + 0.0228 * out.m);
            out.astar = mstar * std::cos(hueRadians);
            out.bstar = mstar * std::sin(hueRadians);
            return out;
        }
    };

    inline bool IsYellowHue(double hue) { return hue >= 105.0 && hue < 125.0; }
    inline bool IsBlueHue(double hue)   { return hue >= 250.0 && hue < 270.0; }
    inline bool IsCyanHue(double hue)   { return hue >= 170.0 && hue < 207.0; }

    inline double inverseChromaticAdaptation(double adapted)
    {
        const double adaptedAbs = std::fabs(adapted);
        const double base = std::max(0.0, 27.13 * adaptedAbs / (400.0 - adaptedAbs));
        return signum(adapted) * std::pow(base, 1.0 / 0.42);
    }

    // Newton solve for an in-gamut sRGB colour; 0 when chroma falls outside it.
    inline uint32_t FindResultByJ(double hueRadians, double chroma, double y)
    {
        const ViewingConditions& vc = DefaultViewingConditions();
        double j = std::sqrt(y) * 11.0;
        const double tInnerCoeff = 1.0 / std::pow(1.64 - std::pow(0.29, vc.n), 0.73);
        const double eHue = 0.25 * (std::cos(hueRadians + 2.0) + 3.8);
        const double p1 = eHue * (50000.0 / 13.0) * vc.nc * vc.ncb;
        const double hSin = std::sin(hueRadians);
        const double hCos = std::cos(hueRadians);

        for (int round = 0; round < 5; round++)
        {
            const double jNormalized = j / 100.0;
            const double alpha = (chroma == 0.0 || j == 0.0) ? 0.0 : chroma / std::sqrt(jNormalized);
            const double t = std::pow(alpha * tInnerCoeff, 1.0 / 0.9);
            const double ac = vc.aw * std::pow(jNormalized, 1.0 / vc.c / vc.z);
            const double p2 = ac / vc.nbb;
            const double gamma = 23.0 * (p2 + 0.305) * t / (23.0 * p1 + 11.0 * t * hCos + 108.0 * t * hSin);
            const double a = gamma * hCos;
            const double b = gamma * hSin;
            const double rA = (460.0 * p2 + 451.0 * a + 288.0 * b) / 1403.0;
            const double gA = (460.0 * p2 - 891.0 * a - 261.0 * b) / 1403.0;
            const double bA = (460.0 * p2 - 220.0 * a - 6300.0 * b) / 1403.0;

            const double linrgb[3] = {
                1373.2198709594231 * inverseChromaticAdaptation(rA) -
                    1100.4251190754821 * inverseChromaticAdaptation(gA) - 7.278681089101213 * inverseChromaticAdaptation(bA),
                -271.815969077903 * inverseChromaticAdaptation(rA) + 559.6580465940733 * inverseChromaticAdaptation(gA) -
                    32.46047482791194 * inverseChromaticAdaptation(bA),
                1.9622899599665666 * inverseChromaticAdaptation(rA) - 57.173814538844006 * inverseChromaticAdaptation(gA) +
                    308.7233197812385 * inverseChromaticAdaptation(bA)};

            if (linrgb[0] < 0 || linrgb[1] < 0 || linrgb[2] < 0)
                return 0;
            const double fnj = 0.2126 * linrgb[0] + 0.7152 * linrgb[1] + 0.0722 * linrgb[2];
            if (fnj <= 0)
                return 0;
            if (round == 4 || std::fabs(fnj - y) < 0.002)
            {
                if (linrgb[0] > 100.01 || linrgb[1] > 100.01 || linrgb[2] > 100.01)
                    return 0;
                return argbFromLinrgb(linrgb);
            }
            // Newton step, with 2 * fn(j) / j as the derivative approximation.
            j = j - (fnj - y) * j / (2.0 * fnj);
        }
        return 0;
    }

    // Solves hue / chroma / L*. upstream bisects a 255-entry critical-plane table
    // when chroma is out of gamut; a binary search on chroma reaches the same answer
    // without the table.
    inline uint32_t SolveToInt(double hue, double chroma, double lstar)
    {
        if (chroma < 0.0001 || lstar < 0.0001 || lstar > 99.9999)
            return argbFromLstar(lstar);

        hue = sanitizeDegrees(hue);
        const double hueRadians = (hue / 180.0) * 3.14159265358979323846;
        const double y = yFromLstar(lstar);

        const uint32_t exact = FindResultByJ(hueRadians, chroma, y);
        if (exact != 0)
            return exact;

        double low = 0.0, high = chroma;
        uint32_t best = 0;
        for (int i = 0; i < 20 && high - low > 0.005; i++)
        {
            const double mid = (low + high) * 0.5;
            const uint32_t attempt = FindResultByJ(hueRadians, mid, y);
            if (attempt != 0)
            {
                low = mid;
                best = attempt;
            }
            else
            {
                high = mid;
            }
        }
        return best != 0 ? best : argbFromLstar(lstar);
    }

    inline double ChromaOf(uint32_t argb) { return Cam16::FromInt(argb).chroma; }
    inline double HueOf(uint32_t argb)    { return Cam16::FromInt(argb).hue; }
    inline double ToneOf(uint32_t argb)   { return lstarFromArgb(argb); }

    // Clamps a requested chroma to what this hue can actually reach.
    inline double ResolvePaletteChroma(double hue, double requestedChroma)
    {
        const double maxChromaValue = 200.0;
        const double epsilon = 0.01;
        const int pivotTone = 50;

        double maxChromaCache[101] = {0.0};
        bool maxChromaCached[101] = {false};
        auto maxChroma = [&](int tone) -> double
        {
            if (!maxChromaCached[tone])
            {
                maxChromaCache[tone] = ChromaOf(SolveToInt(hue, maxChromaValue, (double)tone));
                maxChromaCached[tone] = true;
            }
            return maxChromaCache[tone];
        };

        int lowerTone = 0;
        int upperTone = 100;
        while (lowerTone < upperTone)
        {
            const int midTone = (lowerTone + upperTone) / 2;
            const bool isAscending = maxChroma(midTone) < maxChroma(midTone + 1);
            const bool sufficientChroma = maxChroma(midTone) >= requestedChroma - epsilon;
            if (sufficientChroma)
            {
                if (std::abs(lowerTone - pivotTone) < std::abs(upperTone - pivotTone))
                    upperTone = midTone;
                else
                {
                    if (lowerTone == midTone)
                        return ChromaOf(SolveToInt(hue, requestedChroma, (double)lowerTone));
                    lowerTone = midTone;
                }
            }
            else
            {
                if (isAscending)
                    lowerTone = midTone + 1;
                else
                    upperTone = midTone;
            }
        }
        return ChromaOf(SolveToInt(hue, requestedChroma, (double)lowerTone));
    }

    // One hue, 101 tones. the constructor solves every tone so `tone()` is a lookup.
    struct TonalPalette
    {
        double   hue = 0.0;
        double   chroma = 0.0;
        uint32_t tones[101] = {0};

        TonalPalette() { for (int i = 0; i <= 100; i++) tones[i] = argbFromLstar((double)i); }

        TonalPalette(double hue_, double chroma_)
        {
            // keeps the *requested* chroma, not the key color's: upstream does the
            // same, and clamping here would shift every hex in the palette.
            hue = sanitizeDegrees(hue_);
            chroma = chroma_;
            for (int i = 0; i <= 100; i++)
                tones[i] = SolveToInt(hue, chroma, (double)i);
        }

        uint32_t tone(int t) const
        {
            t = clampInt(0, 100, t);
            // Yellow clips at tone 99, so average 98 and 100 there.
            if (t == 99 && IsYellowHue(hue))
            {
                return argbFromRgb((redFromArgb(tones[98]) + redFromArgb(tones[100])) / 2,
                                   (greenFromArgb(tones[98]) + greenFromArgb(tones[100])) / 2,
                                   (blueFromArgb(tones[98]) + blueFromArgb(tones[100])) / 2);
            }
            return tones[t];
        }
    };

    // The five palettes a scheme is built from, plus error.
    struct CorePalette
    {
        TonalPalette a1, a2, a3, n1, n2, error;

        static CorePalette FromSource(uint32_t argb)
        {
            const Cam16 cam = Cam16::FromInt(argb);
            CorePalette p;
            p.a1 = TonalPalette(cam.hue, cam.chroma > 48.0 ? cam.chroma : 48.0);
            p.a2 = TonalPalette(cam.hue, 16.0);
            p.a3 = TonalPalette(cam.hue + 60.0, 24.0);
            p.n1 = TonalPalette(cam.hue, 4.0);
            p.n2 = TonalPalette(cam.hue, 8.0);
            p.error = TonalPalette(25.0, 84.0);
            return p;
        }
    };

} // namespace ImGuiM3Palette

#endif // IMGUI_M3_COLOR_H_INCLUDED