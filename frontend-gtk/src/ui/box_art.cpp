// SPDX-License-Identifier: GPL-3.0-or-later
#include "box_art.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace ds::frontend {

namespace {

struct Vec2 { double x, y; };

struct Rgba { float r = 0, g = 0, b = 0, a = 0; };

// A picture as floats, for sampling.
struct Image {
    int w = 0, h = 0;
    std::vector<Rgba> px;

    Rgba at(int x, int y) const {
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return px[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) + static_cast<std::size_t>(x)];
    }
    // Bilinear, `u`, `v` in 0..1 across the picture.
    Rgba sample(double u, double v) const {
        const double fx = u * w - 0.5, fy = v * h - 0.5;
        const int x0 = static_cast<int>(std::floor(fx)), y0 = static_cast<int>(std::floor(fy));
        const float tx = static_cast<float>(fx - x0), ty = static_cast<float>(fy - y0);
        const Rgba a = at(x0, y0), b = at(x0 + 1, y0), c = at(x0, y0 + 1), d = at(x0 + 1, y0 + 1);
        const auto mix = [](float p, float q, float t) { return p + (q - p) * t; };
        return {mix(mix(a.r, b.r, tx), mix(c.r, d.r, tx), ty), mix(mix(a.g, b.g, tx), mix(c.g, d.g, tx), ty),
                mix(mix(a.b, b.b, tx), mix(c.b, d.b, tx), ty), mix(mix(a.a, b.a, tx), mix(c.a, d.a, tx), ty)};
    }
};

Image from_pixbuf(GdkPixbuf* p) {
    Image img;
    img.w = gdk_pixbuf_get_width(p);
    img.h = gdk_pixbuf_get_height(p);
    const int n = gdk_pixbuf_get_n_channels(p), stride = gdk_pixbuf_get_rowstride(p);
    const guchar* src = gdk_pixbuf_read_pixels(p);
    img.px.resize(static_cast<std::size_t>(img.w) * static_cast<std::size_t>(img.h));
    for (int y = 0; y < img.h; ++y)
        for (int x = 0; x < img.w; ++x) {
            const guchar* s = src + y * stride + x * n;
            img.px[static_cast<std::size_t>(y * img.w + x)] =
                {s[0] / 255.f, s[1] / 255.f, s[2] / 255.f, n == 4 ? s[3] / 255.f : 1.f};
        }
    return img;
}

// Maps the unit square onto a quadrilateral (corners in the order (0,0),
// (1,0), (1,1), (0,1)), and back: a plane seen in perspective.
struct Homography {
    double m[9];   // row major, maps (s, t, 1) to (x, y, w)
    double inv[9];

    explicit Homography(const std::array<Vec2, 4>& q) {
        const double dx1 = q[1].x - q[2].x, dx2 = q[3].x - q[2].x, dx3 = q[0].x - q[1].x + q[2].x - q[3].x;
        const double dy1 = q[1].y - q[2].y, dy2 = q[3].y - q[2].y, dy3 = q[0].y - q[1].y + q[2].y - q[3].y;
        double g = 0, h = 0;
        const double det = dx1 * dy2 - dx2 * dy1;
        if (std::abs(det) > 1e-12) {
            g = (dx3 * dy2 - dx2 * dy3) / det;
            h = (dx1 * dy3 - dx3 * dy1) / det;
        }
        const double a = q[1].x - q[0].x + g * q[1].x, b = q[3].x - q[0].x + h * q[3].x, c = q[0].x;
        const double d = q[1].y - q[0].y + g * q[1].y, e = q[3].y - q[0].y + h * q[3].y, f = q[0].y;
        const double mm[9] = {a, b, c, d, e, f, g, h, 1.0};
        std::copy(std::begin(mm), std::end(mm), m);
        // Inverse by the adjugate.
        const double A = e - f * h, B = -(d - f * g), C = d * h - e * g;
        const double D = -(b - c * h), E = a - c * g, F = -(a * h - b * g);
        const double G = b * f - c * e, H = -(a * f - c * d), I = a * e - b * d;
        const double dt = a * A + b * B + c * C;
        const double s = std::abs(dt) > 1e-12 ? 1.0 / dt : 0.0;
        const double ii[9] = {A * s, D * s, G * s, B * s, E * s, H * s, C * s, F * s, I * s};
        std::copy(std::begin(ii), std::end(ii), inv);
    }
    // Screen point to (s, t); false when behind the plane.
    bool back(double x, double y, double& s, double& t) const {
        const double w = inv[6] * x + inv[7] * y + inv[8];
        if (std::abs(w) < 1e-12)
            return false;
        s = (inv[0] * x + inv[1] * y + inv[2]) / w;
        t = (inv[3] * x + inv[4] * y + inv[5]) / w;
        return true;
    }
};

// Paints a quadrilateral face; `shade(s, t)` gives the colour at a point of
// the face. Coverage is exact per pixel only through the supersampling done
// by the caller.
template <typename Shade>
void fill_face(Image& out, const std::array<Vec2, 4>& quad, Shade shade) {
    const Homography hom(quad);
    double x0 = quad[0].x, x1 = x0, y0 = quad[0].y, y1 = y0;
    for (const auto& p : quad) {
        x0 = std::min(x0, p.x); x1 = std::max(x1, p.x);
        y0 = std::min(y0, p.y); y1 = std::max(y1, p.y);
    }
    const int ix0 = std::max(0, static_cast<int>(std::floor(x0))), ix1 = std::min(out.w - 1, static_cast<int>(std::ceil(x1)));
    const int iy0 = std::max(0, static_cast<int>(std::floor(y0))), iy1 = std::min(out.h - 1, static_cast<int>(std::ceil(y1)));
    for (int y = iy0; y <= iy1; ++y)
        for (int x = ix0; x <= ix1; ++x) {
            double s = 0, t = 0;
            if (!hom.back(x + 0.5, y + 0.5, s, t) || s < 0 || s > 1 || t < 0 || t > 1)
                continue;
            const Rgba c = shade(s, t);
            Rgba& d = out.px[static_cast<std::size_t>(y * out.w + x)];
            const float a = c.a, keep = 1.f - a;
            d.r = c.r * a + d.r * keep;
            d.g = c.g * a + d.g * keep;
            d.b = c.b * a + d.b * keep;
            d.a = a + d.a * keep;
        }
}

// Blurs the alpha channel in place (three box passes ~ a gaussian).
void blur_alpha(std::vector<float>& a, int w, int h, int radius) {
    if (radius < 1)
        return;
    std::vector<float> tmp(a.size());
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {          // horizontal
            float sum = 0;
            for (int x = -radius; x <= radius; ++x)
                sum += a[static_cast<std::size_t>(y * w + std::clamp(x, 0, w - 1))];
            for (int x = 0; x < w; ++x) {
                tmp[static_cast<std::size_t>(y * w + x)] = sum / static_cast<float>(2 * radius + 1);
                sum += a[static_cast<std::size_t>(y * w + std::min(x + radius + 1, w - 1))] -
                       a[static_cast<std::size_t>(y * w + std::max(x - radius, 0))];
            }
        }
        for (int x = 0; x < w; ++x) {          // vertical
            float sum = 0;
            for (int y = -radius; y <= radius; ++y)
                sum += tmp[static_cast<std::size_t>(std::clamp(y, 0, h - 1) * w + x)];
            for (int y = 0; y < h; ++y) {
                a[static_cast<std::size_t>(y * w + x)] = sum / static_cast<float>(2 * radius + 1);
                sum += tmp[static_cast<std::size_t>(std::min(y + radius + 1, h - 1) * w + x)] -
                       tmp[static_cast<std::size_t>(std::max(y - radius, 0) * w + x)];
            }
        }
    }
}

} // namespace

GdkPixbuf* render_box_art(GdkPixbuf* cover_pixbuf, int edge) {
    if (!cover_pixbuf || edge < 16)
        return nullptr;
    const Image cover = from_pixbuf(cover_pixbuf);
    if (cover.w < 2 || cover.h < 2)
        return nullptr;

    // The case in its own units: the front is 1 wide and `aspect` tall, the
    // spine `depth` deep behind its left edge (a disc case is about 7%).
    const double aspect = static_cast<double>(cover.h) / cover.w;
    const double depth  = 0.11;
    const double turn   = 34.0 * M_PI / 180.0;   // spine turned towards the viewer
    const double tilt   = 4.0 * M_PI / 180.0;    // seen slightly from above
    const double camera = 2.8;                   // distance; smaller = stronger perspective

    const auto project = [&](double x, double y, double z) -> Vec2 {
        // Centre the case on the origin, turn it, tilt it, then perspective.
        x -= 0.5; y -= aspect / 2; z -= depth / 2;
        const double rx = x * std::cos(turn) - z * std::sin(turn);
        const double rz = x * std::sin(turn) + z * std::cos(turn);
        const double ry = y * std::cos(tilt) - rz * std::sin(tilt);
        const double rz2 = y * std::sin(tilt) + rz * std::cos(tilt);
        const double k = camera / (camera + rz2);
        return {rx * k, ry * k};
    };
    const std::array<Vec2, 4> front3 = {project(0, 0, 0), project(1, 0, 0), project(1, aspect, 0), project(0, aspect, 0)};
    // Spine: from the back edge (z = depth) to the front's left edge.
    const std::array<Vec2, 4> spine3 = {project(0, 0, depth), project(0, 0, 0), project(0, aspect, 0), project(0, aspect, depth)};
    const std::array<Vec2, 4> top3 = {project(0, 0, depth), project(1, 0, depth), project(1, 0, 0), project(0, 0, 0)};

    // Fit everything, plus room for the shadow, into the square output;
    // drawn at twice the size and reduced at the end for smooth edges.
    const int ss = 2, size = edge * ss;
    double minx = 1e9, maxx = -1e9, miny = 1e9, maxy = -1e9;
    for (const auto* q : {&front3, &spine3, &top3})
        for (const auto& p : *q) {
            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
        }
    const double margin = 0.06;
    const double scale = (1.0 - 2 * margin) * size / std::max(maxx - minx, maxy - miny);
    const double offx = (size - (maxx - minx) * scale) / 2 - minx * scale;
    const double offy = (size - (maxy - miny) * scale) / 2 - miny * scale - size * 0.015;
    const auto place = [&](const std::array<Vec2, 4>& q) {
        std::array<Vec2, 4> out;
        for (std::size_t i = 0; i < 4; ++i)
            out[i] = {q[i].x * scale + offx, q[i].y * scale + offy};
        return out;
    };
    const auto front = place(front3), spine = place(spine3), top = place(top3);

    Image canvas;
    canvas.w = canvas.h = size;
    canvas.px.assign(static_cast<std::size_t>(size) * static_cast<std::size_t>(size), {});

    // Shadow: the case's outline, moved down and right, blurred. Soft
    // anyway, so it is worked out at a quarter of the size.
    {
        const int q = 4, small = std::max(1, size / q);
        Image mask;
        mask.w = mask.h = small;
        mask.px.assign(static_cast<std::size_t>(small) * static_cast<std::size_t>(small), {});
        const auto shifted = [&](std::array<Vec2, 4> quad) {
            for (auto& p : quad) {
                p.x = (p.x + size * 0.025) / q;
                p.y = (p.y + size * 0.03) / q;
            }
            return quad;
        };
        const auto solid = [](double, double) { return Rgba{0, 0, 0, 1}; };
        fill_face(mask, shifted(front), solid);
        fill_face(mask, shifted(spine), solid);
        std::vector<float> alpha(mask.px.size());
        for (std::size_t i = 0; i < alpha.size(); ++i)
            alpha[i] = mask.px[i].a;
        blur_alpha(alpha, small, small, std::max(1, small / 60));
        for (std::size_t i = 0; i < alpha.size(); ++i)
            mask.px[i].a = alpha[i];
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                canvas.px[static_cast<std::size_t>(y * size + x)] =
                    {0.f, 0.f, 0.f, mask.sample((x + 0.5) / size, (y + 0.5) / size).a * 0.55f};
    }

    // Spine: the leftmost strip of the cover, as if the sleeve wrapped round,
    // in the shade of a face turned from the light.
    const double strip = std::min(0.12, depth * 1.4);
    fill_face(canvas, spine, [&](double s, double t) {
        Rgba c = cover.sample(s * strip, t);
        const float light = 0.48f + 0.12f * static_cast<float>(s);
        c.r *= light; c.g *= light; c.b *= light;
        // The case's hinge: a lighter line where the spine meets the front.
        if (s > 0.92) { c.r += 0.10f; c.g += 0.10f; c.b += 0.10f; }
        c.a = 1.f;
        return c;
    });
    // Top edge of the case (barely visible): dark plastic.
    fill_face(canvas, top, [](double, double) { return Rgba{0.08f, 0.08f, 0.09f, 1.f}; });

    // Front: the cover, lit from the left, with a sheen across the plastic
    // and a thin bright rim at its edges.
    fill_face(canvas, front, [&](double s, double t) {
        Rgba c = cover.sample(s, t);
        const float light = 1.02f - 0.10f * static_cast<float>(s);
        c.r *= light; c.g *= light; c.b *= light;
        const double band = std::abs((s * 0.8 + t * 0.6 / aspect) - 0.55);
        const float sheen = static_cast<float>(std::max(0.0, 0.10 - band * 0.25));
        const double rim = std::min({s, 1 - s, t / aspect * 1.0, (1 - t) / aspect * 1.0});
        const float edge_light = rim < 0.006 ? 0.22f : rim < 0.012 ? 0.08f : 0.f;
        const float spine_shade = s < 0.02 ? 0.85f + static_cast<float>(s) * 7.5f : 1.f;
        c.r = std::min(1.f, c.r * spine_shade + sheen + edge_light);
        c.g = std::min(1.f, c.g * spine_shade + sheen + edge_light);
        c.b = std::min(1.f, c.b * spine_shade + sheen + edge_light);
        c.a = 1.f;
        return c;
    });

    // Down to the requested size (box filter over the supersamples).
    GdkPixbuf* out = gdk_pixbuf_new(GDK_COLORSPACE_RGB, TRUE, 8, edge, edge);
    if (!out)
        return nullptr;
    const int stride = gdk_pixbuf_get_rowstride(out);
    guchar* dst = gdk_pixbuf_get_pixels(out);
    for (int y = 0; y < edge; ++y)
        for (int x = 0; x < edge; ++x) {
            float r = 0, g = 0, b = 0, a = 0;
            for (int sy = 0; sy < ss; ++sy)
                for (int sx = 0; sx < ss; ++sx) {
                    const Rgba& p = canvas.px[static_cast<std::size_t>((y * ss + sy) * size + x * ss + sx)];
                    r += p.r * p.a; g += p.g * p.a; b += p.b * p.a; a += p.a;
                }
            guchar* d = dst + y * stride + x * 4;
            const float n = static_cast<float>(ss * ss);
            const float alpha = a / n;
            const auto to8 = [](float v) { return static_cast<guchar>(std::lround(std::clamp(v, 0.f, 1.f) * 255.f)); };
            // Straight (not premultiplied) colour, as pixbufs store it.
            d[0] = to8(alpha > 0 ? r / a : 0);
            d[1] = to8(alpha > 0 ? g / a : 0);
            d[2] = to8(alpha > 0 ? b / a : 0);
            d[3] = to8(alpha);
        }
    return out;
}

} // namespace ds::frontend
