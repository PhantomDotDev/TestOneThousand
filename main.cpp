#ifdef __INTELLISENSE__
#undef DAXA_CMAKE_EXPORT
#define DAXA_CMAKE_EXPORT
#endif
#define SDL_MAIN_HANDLED

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <daxa/daxa.hpp>
#include <daxa/utils/pipeline_manager.hpp>
#include <GLFW/glfw3.h>
#include "utility/window/Window.hpp"
#include <muli/muli.h>

namespace
{
    using u8 = std::uint8_t;
    using u16 = std::uint16_t;
    using u32 = std::uint32_t;
    using u64 = std::uint64_t;

    constexpr int GRID_W = 640;
    constexpr int GRID_H = 360;
    constexpr int CHUNK = 32;
    constexpr int CHUNKS_X = (GRID_W + CHUNK - 1) / CHUNK;
    constexpr int CHUNKS_Y = (GRID_H + CHUNK - 1) / CHUNK;
    constexpr int NUM_CHUNKS = CHUNKS_X * CHUNKS_Y;
    constexpr double SIM_DT = 1.0 / 60.0;

    // =========================================================================
    // Colours: bytes are R,G,B,A in memory (matches R8G8B8A8_UNORM)
    // =========================================================================
    constexpr u32 rgba(u32 r, u32 g, u32 b, u32 a = 255) { return r | (g << 8) | (b << 16) | (a << 24); }
    constexpr u32 with_alpha(u32 c, u32 a) { return (c & 0x00FFFFFFu) | (a << 24); }

    inline u32 blend(u32 dst, u32 src)
    {
        u32 a = src >> 24;
        a += a >> 7; // 0..256
        u32 ia = 256 - a;
        u32 r = ((src & 255) * a + (dst & 255) * ia) >> 8;
        u32 g = (((src >> 8) & 255) * a + ((dst >> 8) & 255) * ia) >> 8;
        u32 b = (((src >> 16) & 255) * a + ((dst >> 16) & 255) * ia) >> 8;
        return r | (g << 8) | (b << 16) | 0xFF000000u;
    }

    // =========================================================================
    // Shared shape API. Derived must provide:
    //   int width()/height(), bool inside(x,y), V get(x,y) const,
    //   void plot(x,y,V), void span(x0,x1,y,V)   (span is inclusive, may clip)
    // =========================================================================
    struct Pt
    {
        int x, y;
    };

    template <class D, class V>
    class Shapes
    {
        D& self() { return static_cast<D&>(*this); }

    public:
        void px(int x, int y, V v) { self().plot(x, y, v); }

        void hline(int x0, int x1, int y, V v)
        {
            if (x0 > x1) std::swap(x0, x1);
            self().span(x0, x1, y, v);
        }
        void vline(int x, int y0, int y1, V v)
        {
            if (y0 > y1) std::swap(y0, y1);
            for (int y = y0; y <= y1; ++y) self().plot(x, y, v);
        }

        // Bresenham. radius > 0 stamps a filled circle at every step (thick line / brush stroke).
        void line(int x0, int y0, int x1, int y1, V v, int radius = 0)
        {
            int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
            int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
            int err = dx + dy;
            for (;;)
            {
                if (radius > 0) fcircle(x0, y0, radius, v);
                else self().plot(x0, y0, v);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        }

        // Rectangles by corners (inclusive) or by x,y,w,h
        void rect(int x0, int y0, int x1, int y1, V v)
        {
            if (x0 > x1) std::swap(x0, x1);
            if (y0 > y1) std::swap(y0, y1);
            hline(x0, x1, y0, v);
            if (y1 != y0) hline(x0, x1, y1, v);
            for (int y = y0 + 1; y < y1; ++y) { self().plot(x0, y, v); if (x1 != x0) self().plot(x1, y, v); }
        }
        void frect(int x0, int y0, int x1, int y1, V v)
        {
            if (x0 > x1) std::swap(x0, x1);
            if (y0 > y1) std::swap(y0, y1);
            for (int y = y0; y <= y1; ++y) self().span(x0, x1, y, v);
        }
        void box(int x, int y, int w, int h, V v) { rect(x, y, x + w - 1, y + h - 1, v); }
        void fbox(int x, int y, int w, int h, V v) { frect(x, y, x + w - 1, y + h - 1, v); }

        void fellipse(int cx, int cy, int rx, int ry, V v)
        {
            if (rx < 0 || ry < 0) return;
            if (ry == 0) { hline(cx - rx, cx + rx, cy, v); return; }
            for (int dy = -ry; dy <= ry; ++dy)
            {
                double t = 1.0 - double(dy * dy) / double(ry * ry);
                int dx = int(std::floor(rx * std::sqrt(std::max(0.0, t)) + 0.5));
                hline(cx - dx, cx + dx, cy + dy, v);
            }
        }
        void ellipse(int cx, int cy, int rx, int ry, V v)
        {
            if (rx < 0 || ry < 0) return;
            if (rx == 0) { vline(cx, cy - ry, cy + ry, v); return; }
            if (ry == 0) { hline(cx - rx, cx + rx, cy, v); return; }
            for (int dy = -ry; dy <= ry; ++dy)
            {
                double t = 1.0 - double(dy * dy) / double(ry * ry);
                int dx = int(std::floor(rx * std::sqrt(std::max(0.0, t)) + 0.5));
                self().plot(cx - dx, cy + dy, v);
                self().plot(cx + dx, cy + dy, v);
            }
            for (int dx = -rx; dx <= rx; ++dx)
            {
                double t = 1.0 - double(dx * dx) / double(rx * rx);
                int dy = int(std::floor(ry * std::sqrt(std::max(0.0, t)) + 0.5));
                self().plot(cx + dx, cy - dy, v);
                self().plot(cx + dx, cy + dy, v);
            }
        }
        void circle(int cx, int cy, int r, V v) { ellipse(cx, cy, r, r, v); }
        void fcircle(int cx, int cy, int r, V v) { fellipse(cx, cy, r, r, v); }

        void poly(const std::vector<Pt>& p, V v, bool closed = true)
        {
            if (p.empty()) return;
            if (p.size() == 1) { self().plot(p[0].x, p[0].y, v); return; }
            for (size_t i = 0; i + 1 < p.size(); ++i) line(p[i].x, p[i].y, p[i + 1].x, p[i + 1].y, v);
            if (closed) line(p.back().x, p.back().y, p[0].x, p[0].y, v);
        }
        // Even-odd scanline fill (+ outline so thin shapes never vanish).
        void fpoly(const std::vector<Pt>& p, V v)
        {
            if (p.size() < 3) { poly(p, v); return; }
            int miny = INT_MAX, maxy = INT_MIN;
            for (auto const& q : p) { miny = std::min(miny, q.y); maxy = std::max(maxy, q.y); }
            std::vector<double> xs;
            for (int y = miny; y <= maxy; ++y)
            {
                xs.clear();
                for (size_t i = 0; i < p.size(); ++i)
                {
                    Pt a = p[i], b = p[(i + 1) % p.size()];
                    if (a.y == b.y) continue;
                    if (y < std::min(a.y, b.y) || y >= std::max(a.y, b.y)) continue;
                    xs.push_back(a.x + double(y - a.y) * double(b.x - a.x) / double(b.y - a.y));
                }
                std::sort(xs.begin(), xs.end());
                for (size_t i = 0; i + 1 < xs.size(); i += 2)
                {
                    int xa = int(std::ceil(xs[i])), xb = int(std::ceil(xs[i + 1])) - 1;
                    if (xa <= xb) self().span(xa, xb, y, v);
                }
            }
            poly(p, v);
        }
        void tri(int x0, int y0, int x1, int y1, int x2, int y2, V v) { poly({ { x0, y0 },{ x1, y1 },{ x2, y2 } }, v); }
        void ftri(int x0, int y0, int x1, int y1, int x2, int y2, V v) { fpoly({ { x0, y0 },{ x1, y1 },{ x2, y2 } }, v); }

        // 4-connected scanline flood fill. Replaces the region matching get(x,y).
        void flood(int sx, int sy, V v)
        {
            D& d = self();
            if (!d.inside(sx, sy)) return;
            const V target = d.get(sx, sy);
            if (target == v) return;
            const int w = d.width(), h = d.height();
            std::vector<u8> seen(size_t(w) * size_t(h), 0);
            std::vector<Pt> stack;
            stack.push_back({ sx, sy });
            auto match = [&](int x, int y)
                { return d.inside(x, y) && !seen[size_t(y) * w + x] && d.get(x, y) == target; };
            while (!stack.empty())
            {
                Pt p = stack.back();
                stack.pop_back();
                if (!match(p.x, p.y)) continue;
                int l = p.x, r = p.x;
                while (match(l - 1, p.y)) --l;
                while (match(r + 1, p.y)) ++r;
                for (int x = l; x <= r; ++x) { seen[size_t(p.y) * w + x] = 1; d.plot(x, p.y, v); }
                for (int dy = -1; dy <= 1; dy += 2)
                {
                    bool run = false;
                    for (int x = l; x <= r; ++x)
                    {
                        bool m = match(x, p.y + dy);
                        if (m && !run) stack.push_back({ x, p.y + dy });
                        run = m;
                    }
                }
            }
        }
    };

    // =========================================================================
    // 3x5 bitmap font (ASCII 32..95, lower case is mapped to upper case)
    // =========================================================================
    constexpr u16 G(int a, int b, int c, int d, int e) { return u16((a << 12) | (b << 9) | (c << 6) | (d << 3) | e); }
    constexpr std::array<u16, 64> FONT = {
        G(0,0,0,0,0), G(2,2,2,0,2), G(5,5,0,0,0), G(5,7,5,7,5), G(3,6,2,3,6), G(5,1,2,4,5), G(2,5,2,5,3), G(2,2,0,0,0), // sp ! " # $ % & '
        G(2,4,4,4,2), G(2,1,1,1,2), G(0,5,2,5,0), G(0,2,7,2,0), G(0,0,0,2,4), G(0,0,7,0,0), G(0,0,0,0,2), G(1,1,2,4,4), // ( ) * + , - . /
        G(7,5,5,5,7), G(2,6,2,2,7), G(7,1,7,4,7), G(7,1,7,1,7), G(5,5,7,1,1), G(7,4,7,1,7), G(7,4,7,5,7), G(7,1,2,2,2), // 0-7
        G(7,5,7,5,7), G(7,5,7,1,7), G(0,2,0,2,0), G(0,2,0,2,4), G(1,2,4,2,1), G(0,7,0,7,0), G(4,2,1,2,4), G(7,1,3,0,2), // 8 9 : ; < = > ?
        G(2,5,7,4,3), G(2,5,7,5,5), G(6,5,6,5,6), G(3,4,4,4,3), G(6,5,5,5,6), G(7,4,6,4,7), G(7,4,6,4,4), G(3,4,5,5,3), // @ A-G
        G(5,5,7,5,5), G(7,2,2,2,7), G(1,1,1,5,2), G(5,5,6,5,5), G(4,4,4,4,7), G(5,7,7,5,5), G(6,5,5,5,5), G(2,5,5,5,2), // H-O
        G(6,5,6,4,4), G(2,5,5,7,3), G(6,5,6,5,5), G(3,4,2,1,6), G(7,2,2,2,2), G(5,5,5,5,7), G(5,5,5,5,2), G(5,5,7,7,5), // P-W
        G(5,5,2,5,5), G(5,5,2,2,2), G(7,1,2,4,7), G(6,4,4,4,6), G(4,4,2,1,1), G(3,1,1,1,3), G(2,5,0,0,0), G(0,0,0,0,7), // X Y Z [ \ ] ^ _
    };

    // =========================================================================
    // Canvas: non-owning RGBA pixel surface with the shared shape API + text
    // =========================================================================
    class Canvas : public Shapes<Canvas, u32>
    {
    public:
        Canvas(u32* data, int w, int h) : data_(data), w_(w), h_(h) {}
        int width() const { return w_; }
        int height() const { return h_; }
        bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < w_ && y < h_; }
        u32 get(int x, int y) const { return data_[size_t(y) * w_ + x]; }
        u32* row(int y) { return data_ + size_t(y) * w_; }
        u32* data() { return data_; }

        void plot(int x, int y, u32 c)
        {
            if (!inside(x, y)) return;
            u32& d = data_[size_t(y) * w_ + x];
            d = (c >> 24) == 255 ? c : blend(d, c);
        }
        void span(int x0, int x1, int y, u32 c)
        {
            if (y < 0 || y >= h_) return;
            x0 = std::max(x0, 0);
            x1 = std::min(x1, w_ - 1);
            if (x0 > x1) return;
            u32* p = data_ + size_t(y) * w_;
            if ((c >> 24) == 255) std::fill(p + x0, p + x1 + 1, c);
            else for (int x = x0; x <= x1; ++x) p[x] = blend(p[x], c);
        }
        void clear(u32 c) { std::fill(data_, data_ + size_t(w_) * h_, c); }

        static int text_width(const std::string& s, int scale = 1) { return int(s.size()) * 4 * scale; }
        void text(int x, int y, const std::string& s, u32 col, int scale = 1)
        {
            int cx = x;
            for (char ch : s)
            {
                if (ch == '\n') { cx = x; y += 6 * scale; continue; }
                int u = std::toupper(static_cast<unsigned char>(ch));
                if (u < 32 || u > 95) u = '?';
                u16 g = FONT[size_t(u - 32)];
                for (int r = 0; r < 5; ++r)
                    for (int c = 0; c < 3; ++c)
                        if ((g >> (3 * (4 - r) + (2 - c))) & 1)
                        {
                            if (scale == 1) plot(cx + c, y + r, col);
                            else frect(cx + c * scale, y + r * scale, cx + c * scale + scale - 1, y + r * scale + scale - 1, col);
                        }
                cx += 4 * scale;
            }
        }

    private:
        u32* data_;
        int w_, h_;
    };

    // =========================================================================
    // Thread pool (parallel_for with caller participation)
    // =========================================================================
    class ThreadPool
    {
    public:
        explicit ThreadPool(unsigned extra_workers)
        {
            for (unsigned i = 0; i < extra_workers; ++i)
                workers_.emplace_back([this, i] { loop(i + 1); });
        }
        ~ThreadPool()
        {
            {
                std::lock_guard<std::mutex> lk(m_);
                stop_ = true;
            }
            cv_.notify_all();
            for (auto& t : workers_) t.join();
        }
        unsigned size() const { return unsigned(workers_.size()) + 1; }

        // f(int index, unsigned thread_id)
        template <class F>
        void parallel_for(int n, F&& f)
        {
            if (n <= 0) return;
            if (workers_.empty() || n == 1)
            {
                for (int i = 0; i < n; ++i) f(i, 0u);
                return;
            }
            std::function<void(int, unsigned)> fn = f;
            {
                std::lock_guard<std::mutex> lk(m_);
                job_ = &fn;
                count_ = n;
                next_.store(0);
                pending_ = int(workers_.size());
                ++generation_;
            }
            cv_.notify_all();
            drain(0);
            std::unique_lock<std::mutex> lk(m_);
            cv_done_.wait(lk, [&] { return pending_ == 0; });
            job_ = nullptr;
        }

    private:
        void drain(unsigned tid)
        {
            for (;;)
            {
                int i = next_.fetch_add(1);
                if (i >= count_) break;
                (*job_)(i, tid);
            }
        }
        void loop(unsigned tid)
        {
            u64 seen = 0;
            for (;;)
            {
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [&] { return stop_ || generation_ != seen; });
                    if (stop_) return;
                    seen = generation_;
                }
                drain(tid);
                {
                    std::lock_guard<std::mutex> lk(m_);
                    if (--pending_ == 0) cv_done_.notify_one();
                }
            }
        }

        std::vector<std::thread> workers_;
        std::mutex m_;
        std::condition_variable cv_, cv_done_;
        std::function<void(int, unsigned)>* job_ = nullptr;
        std::atomic<int> next_{ 0 };
        int count_ = 0;
        int pending_ = 0;
        u64 generation_ = 0;
        bool stop_ = false;
    };

    // =========================================================================
    // World (cellular falling-sand) — UNTOUCHED core simulation
    // =========================================================================
    enum class Mat : u8
    {
        Empty = 0, Sand, Water, Stone, Gas, Lava, Fire, COUNT
    };
    constexpr int MAT_COUNT = int(Mat::COUNT);
    constexpr const char* MAT_NAMES[MAT_COUNT] = { "EMPTY", "SAND", "WATER", "STONE", "GAS", "LAVA", "FIRE" };
    constexpr u32 MAT_BASE[MAT_COUNT] = {
        rgba(0, 0, 0), rgba(232, 176, 71), rgba(46, 110, 214), rgba(110, 112, 122),
        rgba(175, 175, 190), rgba(255, 110, 20), rgba(255, 160, 40)
    };

    struct Cell
    {
        u8 type = 0; // Mat
        u8 vel = 0;  // fall speed (sand/water) or remaining life (gas)
        u8 var = 0;  // colour variation 0..15
        u8 stamp = 0;// last frame this cell moved (prevents double-moves)
    };
    static_assert(sizeof(Cell) == 4);

    struct Rng
    {
        u32 s;
        u32 next() { s ^= s << 13; s ^= s >> 17; s ^= s << 5; return s; }
        bool coin() { return next() & 1u; }
        u32 below(u32 n) { return next() % n; }
    };

    class World : public Shapes<World, Mat>
    {
    public:
        static constexpr int MARGIN = 6;   // wake neighbouring chunks if a change is this close to the border
        static constexpr int MAX_FALL = 8; // sand fall speed cap
        static constexpr int WATER_FALL = 6;
        static constexpr int DISP = 6;     // water max lateral reach per step
        static constexpr u8 DIR_BIT = 0x40; // water flow direction stored in Cell::var

        bool overwrite = true; // false: plotting only fills empty cells (used by freehand brush)
        u32 density = 255;     // 0..255 chance a plotted non-empty cell is actually placed (spray)

        // External solid occupancy (rigid-body pixels). 0 = free, non-zero = blocked.
        // Set each frame before stepping so sand/water cannot phase through boxes.
        const u16* solid_id_ = nullptr;
        void set_solid_mask(const u16* ids) { solid_id_ = ids; }
        bool is_solid(int x, int y) const
        {
            return solid_id_ && inside(x, y) && solid_id_[size_t(y) * GRID_W + x] != 0;
        }

        World()
            : cells_(size_t(GRID_W)* GRID_H),
            wake_next_(new std::atomic<u8>[NUM_CHUNKS]),
            dirty_(new std::atomic<u8>[NUM_CHUNKS]),
            awake_cur_(NUM_CHUNKS, 1), chunk_moves_(NUM_CHUNKS, 0), chunk_tid_(NUM_CHUNKS, 0)
        {
            build_palette();
            reset();
        }

        // ---- Shapes interface ------------------------------------------------
        int width() const { return GRID_W; }
        int height() const { return GRID_H; }
        bool inside(int x, int y) const { return x >= 0 && y >= 0 && x < GRID_W && y < GRID_H; }
        Mat get(int x, int y) const { return Mat(cells_[idx(x, y)].type); }
        void plot(int x, int y, Mat m)
        {
            if (!inside(x, y)) return;
            // Cannot place materials into rigid-body solid cells (erase is still allowed)
            if (m != Mat::Empty && is_solid(x, y)) return;
            Cell& c = cells_[idx(x, y)];
            if (m == Mat::Empty)
            {
                if (c.type == u8(Mat::Empty)) return;
                c = Cell{};
            }
            else
            {
                if (c.type == u8(m)) return;
                if (!overwrite && c.type != u8(Mat::Empty)) return;
                if (density < 255 && prng_.below(256) >= density) return;
                c = make_cell(m);
            }
            touch(x, y);
        }
        void span(int x0, int x1, int y, Mat m)
        {
            if (y < 0 || y >= GRID_H) return;
            x0 = std::max(x0, 0);
            x1 = std::min(x1, GRID_W - 1);
            for (int x = x0; x <= x1; ++x) plot(x, y, m);
        }

        // ---- State -------------------------------------------------------------
        void reset()
        {
            std::fill(cells_.begin(), cells_.end(), Cell{});
            for (int i = 0; i < NUM_CHUNKS; ++i)
            {
                wake_next_[i].store(1, std::memory_order_relaxed);
                dirty_[i].store(1, std::memory_order_relaxed);
                awake_cur_[i] = 1;
                chunk_moves_[i] = 0;
                chunk_tid_[i] = 0;
            }
            frame_ = 0;
            bool old = overwrite;
            overwrite = true;
            // Starter pile, drawn with the same shape API the user tools use.
            ftri(GRID_W / 2 - 102, GRID_H - 2, GRID_W / 2 + 102, GRID_H - 2, GRID_W / 2, GRID_H - 32, Mat::Sand);
            overwrite = old;
        }

        // ---- Simulation --------------------------------------------------------
        void step(ThreadPool& pool, bool multithread, bool sleeping)
        {
            ++frame_;
            stamp_ = u8(frame_ % 255 + 1);
            active_chunks = 0;
            total_moved = 0;
            for (int ci = 0; ci < NUM_CHUNKS; ++ci)
            {
                u8 w = wake_next_[ci].exchange(0, std::memory_order_relaxed);
                awake_cur_[ci] = sleeping ? w : 1;
                chunk_moves_[ci] = 0;
                chunk_tid_[ci] = 0;
            }
            // Chunk rows run strictly bottom -> top, so a chunk only moves after the chunk
            // beneath it has already moved this frame (no stalls / gaps at chunk borders).
            // Inside a row, chunks are split by cx parity (>= 1 chunk apart, cells reach at most
            // MAX_FALL / DISP < CHUNK cells sideways) so they can run in parallel. The parity order
            // alternates each frame to avoid a left/right bias.
            const int parity0 = int(frame_ & 1u);
            for (int cy = CHUNKS_Y - 1; cy >= 0; --cy)
            {
                for (int pass = 0; pass < 2; ++pass)
                {
                    list_.clear();
                    for (int cx = pass ^ parity0; cx < CHUNKS_X; cx += 2)
                    {
                        int ci = cy * CHUNKS_X + cx;
                        if (awake_cur_[ci]) list_.push_back(ci);
                    }
                    if (list_.empty()) continue;
                    active_chunks += int(list_.size());
                    auto work = [&](int i, unsigned tid)
                        {
                            int ci = list_[size_t(i)];
                            Rng rng{ (((u32(ci) + 1u) * 2654435761u) ^ (frame_ * 0x9E3779B9u)) | 1u };
                            rng.next();
                            chunk_moves_[ci] = step_chunk(ci % CHUNKS_X, ci / CHUNKS_X, rng);
                            chunk_tid_[ci] = u8(tid + 1);
                        };
                    if (multithread && list_.size() >= 6) pool.parallel_for(int(list_.size()), work);
                    else for (int i = 0; i < int(list_.size()); ++i) work(i, 0u);
                }
            }
            for (int ci = 0; ci < NUM_CHUNKS; ++ci) total_moved += chunk_moves_[ci];
        }

        // ---- Rendering (only dirty chunks unless all==true) --------------------
        u32 render(Canvas& fb, ThreadPool& pool, bool multithread, bool all = false)
        {
            list_.clear();
            for (int ci = 0; ci < NUM_CHUNKS; ++ci)
                if (all || dirty_[ci].load(std::memory_order_relaxed)) list_.push_back(ci);
            auto work = [&](int i, unsigned)
                {
                    int ci = list_[size_t(i)];
                    dirty_[ci].store(0, std::memory_order_relaxed);
                    draw_chunk(ci, fb);
                };
            if (multithread && list_.size() >= 8) pool.parallel_for(int(list_.size()), work);
            else for (int i = 0; i < int(list_.size()); ++i) work(i, 0u);
            return u32(list_.size());
        }

        void count_types(std::array<u32, MAT_COUNT>& out) const
        {
            out.fill(0);
            for (auto const& c : cells_) ++out[c.type < MAT_COUNT ? c.type : 0];
        }

        // ---- Debug accessors ---------------------------------------------------
        Cell const& cell(int x, int y) const { return cells_[idx(x, y)]; }
        bool chunk_awake(int ci) const { return awake_cur_[ci] != 0; }
        u32 chunk_moves(int ci) const { return chunk_moves_[ci]; }
        u8 chunk_thread(int ci) const { return chunk_tid_[ci]; }
        int active_chunks = 0;
        u32 total_moved = 0;

        // Public helpers used by rigid-body interaction (do not change core step)
        Cell& cell_mut(int x, int y) { return cells_[idx(x, y)]; }
        void force_touch(int x, int y) { touch(x, y); }
        Cell make_cell_public(Mat m) { return make_cell(m); }

    private:
        static size_t idx(int x, int y) { return size_t(y) * GRID_W + x; }

        static Cell make_cell(Mat m)
        {
            static Rng r{ 0x1234567u };
            Cell c;
            c.type = u8(m);
            c.var = u8((r.next() & 15u) | ((r.next() & 1u) << 6));
            switch (m)
            {
            case Mat::Sand:
            case Mat::Water: c.vel = 1; break;
            case Mat::Gas: c.vel = u8(120 + r.below(100)); break;
            case Mat::Fire: c.vel = u8(40 + r.below(50)); break; // lifetime
            default: c.vel = 0; break;
            }
            return c;
        }

        void build_palette()
        {
            for (int t = 0; t < MAT_COUNT; ++t)
                for (int v = 0; v < 16; ++v)
                {
                    int d = (v - 8) * 3;
                    u32 base = MAT_BASE[t];
                    auto ch = [&](int shift) { return u32(std::clamp(int((base >> shift) & 255) + d, 0, 255)); };
                    lut_[t][v] = rgba(ch(0), ch(8), ch(16));
                }
            for (int y = 0; y < GRID_H; ++y)
            {
                float g = 0.015f + 0.025f * float(y) / float(GRID_H);
                bg_[y] = rgba(u32(g * 0.55f * 255.f), u32(g * 0.8f * 255.f), u32(g * 1.25f * 255.f));
            }
        }

        static void mark(std::atomic<u8>& a)
        {
            if (!a.load(std::memory_order_relaxed)) a.store(1, std::memory_order_relaxed);
        }
        void wake(int cx, int cy)
        {
            if (cx < 0 || cy < 0 || cx >= CHUNKS_X || cy >= CHUNKS_Y) return;
            mark(wake_next_[cy * CHUNKS_X + cx]);
        }
        // A cell changed at (x,y): redraw its chunk, wake it and any neighbour within MARGIN.
        void touch(int x, int y)
        {
            int cx = x / CHUNK, cy = y / CHUNK;
            int lx = x - cx * CHUNK, ly = y - cy * CHUNK;
            mark(dirty_[cy * CHUNKS_X + cx]);
            int mx = lx < MARGIN ? -1 : (lx >= CHUNK - MARGIN ? 1 : 0);
            int my = ly < MARGIN ? -1 : (ly >= CHUNK - MARGIN ? 1 : 0);
            wake(cx, cy);
            if (mx) wake(cx + mx, cy);
            if (my) wake(cx, cy + my);
            if (mx && my) wake(cx + mx, cy + my);
        }

        static void set_dir(Cell& c, int d) { c.var = u8((c.var & ~DIR_BIT) | (d > 0 ? DIR_BIT : 0)); }

        bool empty_at(int x, int y) const
        {
            if (!inside(x, y)) return false;
            if (cells_[idx(x, y)].type != u8(Mat::Empty)) return false;
            // Rigid-body solid pixels block sand/water/lava/gas the same way stone does
            if (solid_id_ && solid_id_[size_t(y) * GRID_W + x] != 0) return false;
            return true;
        }

        // Swap src and dst (so displaced fluids go back to src), stamp both, wake.
        void move(int x, int y, int nx, int ny)
        {
            std::swap(cells_[idx(x, y)], cells_[idx(nx, ny)]);
            cells_[idx(x, y)].stamp = stamp_;
            cells_[idx(nx, ny)].stamp = stamp_;
            touch(x, y);
            touch(nx, ny);
        }

        u32 step_chunk(int cx, int cy, Rng& rng)
        {
            const int x0 = cx * CHUNK, y0 = cy * CHUNK;
            const int x1 = std::min(x0 + CHUNK, GRID_W), y1 = std::min(y0 + CHUNK, GRID_H);
            const bool ltr = (frame_ & 1u) != 0;
            u32 moved = 0;
            for (int y = y1 - 1; y >= y0; --y)
            {
                for (int i = 0; i < x1 - x0; ++i)
                {
                    const int x = ltr ? x0 + i : x1 - 1 - i;
                    Cell& c = cells_[idx(x, y)];
                    if (c.type == u8(Mat::Empty) || c.type == u8(Mat::Stone) || c.stamp == stamp_) continue;
                    switch (Mat(c.type))
                    {
                    case Mat::Sand:
                    {
                        const int fall = std::min<int>(c.vel, MAX_FALL);
                        int dist = 0;
                        while (dist < fall && empty_at(x, y + dist + 1)) ++dist;
                        if (dist > 0)
                        {
                            // Per-grain random acceleration + sideways scatter while airborne:
                            // blobs break up into a spray instead of falling as a rigid block.
                            u8 nv = c.vel;
                            if (rng.below(4) != 0) nv = u8(std::min<int>(nv + 1, MAX_FALL));
                            int nx = x;
                            if (c.vel >= 2 && rng.below(5) == 0)
                            {
                                const int d = rng.coin() ? -1 : 1;
                                if (empty_at(x + d, y + 1) && empty_at(x + d, y + dist)) nx = x + d;
                            }
                            move(x, y, nx, y + dist);
                            cells_[idx(nx, y + dist)].vel = nv;
                            ++moved;
                            break;
                        }
                        if (inside(x, y + 1) && cells_[idx(x, y + 1)].type == u8(Mat::Water))
                        {
                            move(x, y, x, y + 1); // sand sinks through water
                            cells_[idx(x, y + 1)].vel = 1;
                            ++moved;
                            break;
                        }
                        const int d = rng.coin() ? -1 : 1;
                        if (empty_at(x + d, y + 1)) { move(x, y, x + d, y + 1); cells_[idx(x + d, y + 1)].vel = 1; ++moved; }
                        else if (empty_at(x - d, y + 1)) { move(x, y, x - d, y + 1); cells_[idx(x - d, y + 1)].vel = 1; ++moved; }
                        else
                        {
                            // Resting grain: occasional lateral settle smooths chunk seams / visible grid
                            if (rng.below(12) == 0)
                            {
                                if (empty_at(x + d, y)) { move(x, y, x + d, y); cells_[idx(x + d, y)].vel = 1; ++moved; }
                                else if (empty_at(x - d, y)) { move(x, y, x - d, y); cells_[idx(x - d, y)].vel = 1; ++moved; }
                            }
                            if (y < y1 - 1) c.vel = 1; // (not at chunk bottom: lower chunk may simply not have run yet)
                        }
                        break;
                    }
                    case Mat::Water:
                    {
                        const int fall = std::min<int>(c.vel, WATER_FALL);
                        int dist = 0;
                        while (dist < fall && empty_at(x, y + dist + 1)) ++dist;
                        if (dist > 0)
                        {
                            u8 nv = c.vel;
                            if (rng.below(4) != 0) nv = u8(std::min<int>(nv + 1, WATER_FALL));
                            int nx = x;
                            if (c.vel >= 2 && rng.below(3) == 0)
                            {
                                const int d = rng.coin() ? -1 : 1;
                                if (empty_at(x + d, y + 1) && empty_at(x + d, y + dist)) nx = x + d;
                            }
                            move(x, y, nx, y + dist);
                            cells_[idx(nx, y + dist)].vel = nv;
                            ++moved;
                            break;
                        }
                        if (y < y1 - 1) c.vel = 1;
                        const int d = rng.coin() ? -1 : 1;
                        if (empty_at(x + d, y + 1)) { set_dir(c, d); move(x, y, x + d, y + 1); ++moved; break; }
                        if (empty_at(x - d, y + 1)) { set_dir(c, -d); move(x, y, x - d, y + 1); ++moved; break; }
                        // Flow sideways with momentum: keep going the same way until blocked, then turn.
                        const int reach = 2 + int(rng.below(DISP - 1));
                        int dir = (c.var & DIR_BIT) ? 1 : -1, nx = x;
                        for (int attempt = 0; attempt < 2 && nx == x; ++attempt, dir = -dir)
                            for (int s = 1; s <= reach; ++s)
                            {
                                if (empty_at(x + dir * s, y)) nx = x + dir * s;
                                else break;
                            }
                        if (nx != x) { set_dir(c, nx > x ? 1 : -1); move(x, y, nx, y); ++moved; }
                        break;
                    }
                    case Mat::Lava:
                    {
                        static constexpr int nd[4][2] = { { 0, 1 },{ 0, -1 },{ 1, 0 },{ -1, 0 } };
                        bool cooled = false;
                        for (auto const& n : nd)
                        {
                            int wx = x + n[0], wy = y + n[1];
                            if (inside(wx, wy) && cells_[idx(wx, wy)].type == u8(Mat::Water))
                            {
                                cells_[idx(wx, wy)] = make_cell(Mat::Gas); // water flashes to steam
                                touch(wx, wy);
                                cooled = true;
                                break;
                            }
                        }
                        if (cooled) { c = make_cell(Mat::Stone); touch(x, y); ++moved; break; }
                        touch(x, y); // slow & probabilistic, so stay awake
                        if (rng.below(4) != 0) break;
                        const int d = rng.coin() ? -1 : 1;
                        if (empty_at(x, y + 1)) { move(x, y, x, y + 1); ++moved; }
                        else if (empty_at(x + d, y + 1)) { move(x, y, x + d, y + 1); ++moved; }
                        else if (empty_at(x - d, y + 1)) { move(x, y, x - d, y + 1); ++moved; }
                        else
                        {
                            int nx = x;
                            for (int s = 1; s <= 2; ++s) { if (empty_at(x + d * s, y)) nx = x + d * s; else break; }
                            if (nx != x) { move(x, y, nx, y); ++moved; }
                        }
                        break;
                    }
                    case Mat::Gas:
                    {
                        if (c.vel == 0)
                        {
                            c = Cell{};
                            touch(x, y);
                            break;
                        }
                        --c.vel;
                        touch(x, y); // colour fades with life, so keep awake/dirty
                        const int d = rng.coin() ? -1 : 1;
                        if (rng.below(8) < 6 && empty_at(x, y - 1)) { move(x, y, x, y - 1); ++moved; }
                        else if (empty_at(x + d, y - 1)) { move(x, y, x + d, y - 1); ++moved; }
                        else if (empty_at(x + d, y)) { move(x, y, x + d, y); ++moved; }
                        break;
                    }
                    case Mat::Fire:
                    {
                        // Lifetime countdown; flicker colour via var
                        if (c.vel == 0) { c = Cell{}; touch(x, y); break; }
                        if (rng.below(3) == 0) --c.vel;
                        c.var = u8((c.var + rng.below(3)) & 15);
                        touch(x, y);

                        // Extinguish in water / turn water to steam occasionally
                        static constexpr int nd[4][2] = { { 0,1 },{ 0,-1 },{ 1,0 },{ -1,0 } };
                        for (auto const& n : nd)
                        {
                            int nx = x + n[0], ny = y + n[1];
                            if (!inside(nx, ny)) continue;
                            Cell& nb = cells_[idx(nx, ny)];
                            if (nb.type == u8(Mat::Water))
                            {
                                // steam + kill fire
                                nb = make_cell(Mat::Gas);
                                touch(nx, ny);
                                c = Cell{};
                                touch(x, y);
                                ++moved;
                                break;
                            }
                            // ignite gas / lava stays hot
                            if (nb.type == u8(Mat::Gas) && rng.below(8) == 0)
                            {
                                nb = make_cell(Mat::Fire);
                                touch(nx, ny);
                            }
                        }
                        if (c.type != u8(Mat::Fire)) break;

                        // Rise and flicker like real fire (prefer up + slight sideways sway)
                        const int d = rng.coin() ? -1 : 1;
                        if (empty_at(x, y - 1))
                        {
                            move(x, y, x, y - 1);
                            // occasionally spawn a sibling flame for a taller plume
                            if (rng.below(14) == 0 && empty_at(x + d, y) && c.vel > 10)
                            {
                                cells_[idx(x + d, y)] = make_cell(Mat::Fire);
                                cells_[idx(x + d, y)].vel = u8(std::max(8, int(c.vel) / 2));
                                touch(x + d, y);
                            }
                            ++moved;
                        }
                        else if (empty_at(x + d, y - 1)) { move(x, y, x + d, y - 1); ++moved; }
                        else if (empty_at(x + d, y)) { move(x, y, x + d, y); ++moved; }
                        else if (rng.below(6) == 0) // trapped: die faster
                            c.vel = u8(std::max(0, int(c.vel) - 3));
                        break;
                    }
                    default: break;
                    }
                }
            }
            return moved;
        }

        void draw_chunk(int ci, Canvas& fb) const
        {
            const int cx = ci % CHUNKS_X, cy = ci / CHUNKS_X;
            const int x0 = cx * CHUNK, y0 = cy * CHUNK;
            const int x1 = std::min(x0 + CHUNK, GRID_W), y1 = std::min(y0 + CHUNK, GRID_H);
            for (int y = y0; y < y1; ++y)
            {
                u32* dst = fb.row(y);
                const Cell* src = &cells_[idx(0, y)];
                const u32 bg = bg_[y];
                for (int x = x0; x < x1; ++x)
                {
                    const Cell& c = src[x];
                    u32 rgb, mask = 0; // alpha channel carries a material mask for the post-process shader
                    switch (Mat(c.type))
                    {
                    case Mat::Empty: rgb = bg; break;
                    case Mat::Gas: rgb = blend(bg, with_alpha(lut_[c.type][c.var & 15], u32(c.vel) * 3 / 4)); break;
                    case Mat::Water: rgb = lut_[c.type][c.var & 15]; mask = 200; break;
                    case Mat::Lava: rgb = lut_[c.type][c.var & 15]; mask = 120; break;
                    case Mat::Fire:
                    {
                        // Hot core → cooler tip based on remaining life + var flicker
                        float t = std::clamp(float(c.vel) / 90.0f, 0.0f, 1.0f);
                        float flick = 0.7f + 0.3f * float((c.var & 15)) / 15.0f;
                        u32 r = u32(std::clamp(180.0f + 75.0f * t * flick, 0.0f, 255.0f));
                        u32 g = u32(std::clamp(30.0f + 140.0f * t * t * flick, 0.0f, 220.0f));
                        u32 b = u32(std::clamp(10.0f + 20.0f * (1.0f - t), 0.0f, 80.0f));
                        rgb = rgba(r, g, b);
                        mask = 300; // bloom-ish
                        break;
                    }
                    default: rgb = lut_[c.type][c.var & 15]; break;
                    }
                    dst[x] = (rgb & 0x00FFFFFFu) | (mask << 24); // never 255: 255 marks CPU overlay pixels
                }
            }
        }

        std::vector<Cell> cells_;
        std::unique_ptr<std::atomic<u8>[]> wake_next_, dirty_;
        std::vector<u8> awake_cur_;
        std::vector<u32> chunk_moves_;
        std::vector<u8> chunk_tid_;
        std::vector<int> list_;
        u32 lut_[MAT_COUNT][16] = {};
        u32 bg_[GRID_H] = {};
        u32 frame_ = 0;
        u8 stamp_ = 1;
        Rng prng_{ 0xC0FFEEu };
    };

    // =========================================================================
    // Launched particles — short-lived ballistic sim when rigid bodies hit sand/water
    // (does not touch the core cellular step_chunk / sleeping / multithreading)
    // =========================================================================
    struct LaunchedParticle
    {
        float x, y;     // sub-pixel position (pixel space, y-down)
        float vx, vy;   // velocity in pixels per frame
        Mat type;
        u8 var;
        int life;       // remaining frames before forced re-insertion
    };

    // Pixel-owned destructible rigid body. local_pixels are in metres, body-local (Y-up).
    struct DestructibleBody
    {
        muli::Body* body = nullptr;
        std::vector<muli::Vec2> local_pixels; // solid samples relative to body origin
        bool dirty = false;                  // needs rebuild after a cut
        u32 color = 0;
    };

    // Rasterize a box or circle into local pixel samples (metres, Y-up, centred on origin)
    inline void sample_box_local(std::vector<muli::Vec2>& out, float half_w, float half_h, float spacing)
    {
        out.clear();
        for (float y = -half_h; y <= half_h + 1e-4f; y += spacing)
            for (float x = -half_w; x <= half_w + 1e-4f; x += spacing)
                out.push_back({ x, y });
    }
    inline void sample_circle_local(std::vector<muli::Vec2>& out, float radius, float spacing)
    {
        out.clear();
        const float r2 = radius * radius;
        for (float y = -radius; y <= radius + 1e-4f; y += spacing)
            for (float x = -radius; x <= radius + 1e-4f; x += spacing)
                if (x * x + y * y <= r2)
                    out.push_back({ x, y });
    }

    // Rebuild a Muli body from remaining local pixels (AABB approximation for speed)
    inline muli::Body* rebuild_body_from_pixels(muli::World& world, const std::vector<muli::Vec2>& local,
        const muli::Transform& old_tf, u32 /*color*/)
    {
        if (local.size() < 4) return nullptr;

        float minx = local[0].x, maxx = local[0].x, miny = local[0].y, maxy = local[0].y;
        for (auto const& p : local)
        {
            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
            miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
        }
        float w = maxx - minx;
        float h = maxy - miny;
        if (w < 0.08f || h < 0.08f) return nullptr;

        // Centre offset of the remaining AABB in local space
        muli::Vec2 local_center{ (minx + maxx) * 0.5f, (miny + maxy) * 0.5f };
        muli::Vec2 world_center = muli::Mul(old_tf, local_center);

        muli::Body* b = world.CreateBox(w, h,
            muli::Transform{ world_center, old_tf.rotation },
            muli::Body::dynamic_body);
        if (b)
        {
            b->SetFriction(0.55f);
            b->SetRestitution(0.12f);
        }
        return b;
    }

    // =========================================================================
    // Tools: one function draws any tool into ANY target (Canvas preview or World)
    // =========================================================================
    enum class Tool { Brush, Line, Rect, Circle, Ellipse, Triangle, Fill, COUNT };
    constexpr const char* TOOL_NAMES[] = { "BRUSH", "LINE", "RECT", "CIRCLE", "ELLIPSE", "TRIANGLE", "FILL" };

    template <class T, class V>
    void apply_tool(T& t, Tool tool, Pt a, Pt b, V v, bool filled, int radius)
    {
        switch (tool)
        {
        case Tool::Brush: t.line(a.x, a.y, b.x, b.y, v, radius); break;
        case Tool::Line: t.line(a.x, a.y, b.x, b.y, v, radius / 2); break;
        case Tool::Rect: filled ? t.frect(a.x, a.y, b.x, b.y, v) : t.rect(a.x, a.y, b.x, b.y, v); break;
        case Tool::Circle:
        {
            int r = int(std::lround(std::hypot(double(b.x - a.x), double(b.y - a.y))));
            filled ? t.fcircle(a.x, a.y, r, v) : t.circle(a.x, a.y, r, v);
            break;
        }
        case Tool::Ellipse:
        {
            int cx = (a.x + b.x) / 2, cy = (a.y + b.y) / 2;
            int rx = std::abs(b.x - a.x) / 2, ry = std::abs(b.y - a.y) / 2;
            filled ? t.fellipse(cx, cy, rx, ry, v) : t.ellipse(cx, cy, rx, ry, v);
            break;
        }
        case Tool::Triangle:
        {
            int mx = (a.x + b.x) / 2;
            filled ? t.ftri(a.x, b.y, b.x, b.y, mx, a.y, v) : t.tri(a.x, b.y, b.x, b.y, mx, a.y, v);
            break;
        }
        case Tool::Fill: t.flood(b.x, b.y, v); break;
        default: break;
        }
    }

    // =========================================================================
    // Debug overlays
    // =========================================================================
    enum DebugFlags : u32
    {
        DBG_HUD = 1, DBG_CHUNKS = 2, DBG_VELOCITY = 4, DBG_THREADS = 8,
        DBG_GRAPH = 16, DBG_INSPECT = 32, DBG_HELP = 64
    };

    struct Stats
    {
        double frame_ms = 0, sim_ms = 0, edit_ms = 0, render_ms = 0, overlay_ms = 0, upload_ms = 0;
        int fps = 0;
        u32 rendered_chunks = 0;
        std::array<u32, MAT_COUNT> counts{};
        std::array<float, 120> history{};
        int hist_pos = 0;
        static void ema(double& v, double x) { v = v * 0.9 + x * 0.1; }
    };

    inline void chunk_rect(int ci, int& x0, int& y0, int& x1, int& y1)
    {
        x0 = (ci % CHUNKS_X) * CHUNK;
        y0 = (ci / CHUNKS_X) * CHUNK;
        x1 = std::min(x0 + CHUNK, GRID_W) - 1;
        y1 = std::min(y0 + CHUNK, GRID_H) - 1;
    }

    void overlay_chunks(Canvas& c, World const& w)
    {
        for (int ci = 0; ci < NUM_CHUNKS; ++ci)
        {
            int x0, y0, x1, y1;
            chunk_rect(ci, x0, y0, x1, y1);
            bool awake = w.chunk_awake(ci);
            if (awake)
            {
                u32 a = 35 + std::min<u32>(w.chunk_moves(ci), 120);
                c.frect(x0, y0, x1, y1, rgba(30, 220, 90, a));
            }
            c.rect(x0, y0, x1, y1, awake ? rgba(70, 255, 170, 210) : rgba(60, 100, 130, 110));
        }
    }

    void overlay_threads(Canvas& c, World const& w)
    {
        static const u32 pal[8] = {
            rgba(255, 90, 90), rgba(255, 180, 60), rgba(240, 240, 80), rgba(90, 230, 90),
            rgba(70, 220, 230), rgba(90, 130, 255), rgba(190, 100, 255), rgba(255, 110, 200) };
        for (int ci = 0; ci < NUM_CHUNKS; ++ci)
        {
            u8 t = w.chunk_thread(ci);
            if (!t) continue;
            int x0, y0, x1, y1;
            chunk_rect(ci, x0, y0, x1, y1);
            u32 col = pal[(t - 1) & 7];
            c.frect(x0, y0, x1, y1, with_alpha(col, 110));
            c.rect(x0, y0, x1, y1, with_alpha(col, 230));
        }
    }

    void overlay_velocity(Canvas& c, World const& w)
    {
        for (int y = 0; y < GRID_H; ++y)
        {
            u32* row = c.row(y);
            for (int x = 0; x < GRID_W; ++x)
            {
                Cell const& cell = w.cell(x, y);
                if ((cell.type == u8(Mat::Sand) || cell.type == u8(Mat::Water)) && cell.vel > 1)
                {
                    float t = float(cell.vel - 1) / 7.0f;
                    row[x] = rgba(u32(60 + 195 * t), u32(200 - 140 * t), u32(255 - 215 * t));
                }
            }
        }
    }

    void overlay_graph(Canvas& c, Stats const& s)
    {
        const int gw = int(s.history.size()), gh = 40;
        const int x0 = GRID_W - gw - 4, y0 = GRID_H - gh - 4;
        c.fbox(x0 - 2, y0 - 8, gw + 4, gh + 10, rgba(0, 0, 0, 170));
        c.text(x0, y0 - 6, "FRAME MS", rgba(220, 220, 220));
        const float max_ms = 33.4f;
        for (int i = 0; i < gw; ++i)
        {
            float ms = s.history[size_t((s.hist_pos + i) % gw)];
            int h = int(std::min(ms, max_ms) / max_ms * float(gh));
            u32 col = ms <= 17.5f ? rgba(80, 230, 110) : (ms <= 33.4f ? rgba(240, 210, 70) : rgba(240, 80, 80));
            if (h > 0) c.vline(x0 + i, y0 + gh - h, y0 + gh - 1, col);
        }
        c.hline(x0, x0 + gw - 1, y0 + gh - int(16.7f / max_ms * float(gh)), rgba(255, 255, 255, 90)); // 60 fps
    }

    void overlay_inspector(Canvas& c, World const& w, Pt cur)
    {
        if (!w.inside(cur.x, cur.y)) return;
        const u32 line = rgba(255, 255, 255, 80);
        c.hline(0, GRID_W - 1, cur.y, line);
        c.vline(cur.x, 0, GRID_H - 1, line);
        Cell const& cell = w.cell(cur.x, cur.y);
        int ci = (cur.y / CHUNK) * CHUNKS_X + (cur.x / CHUNK);
        char buf[96];
        std::snprintf(buf, sizeof buf, "%d,%d %s V%d VAR%d\nCHUNK %d,%d %s MOVES %u",
            cur.x, cur.y, MAT_NAMES[cell.type < MAT_COUNT ? cell.type : 0], int(cell.vel), int(cell.var),
            cur.x / CHUNK, cur.y / CHUNK, w.chunk_awake(ci) ? "AWAKE" : "ASLEEP", w.chunk_moves(ci));
        std::string s = buf;
        int tw = 0, cur_w = 0, lines = 1;
        for (char ch : s) { if (ch == '\n') { tw = std::max(tw, cur_w); cur_w = 0; ++lines; } else cur_w += 4; }
        tw = std::max(tw, cur_w);
        int tx = std::min(cur.x + 8, GRID_W - tw - 6), ty = std::min(cur.y + 8, GRID_H - lines * 6 - 6);
        c.fbox(tx - 2, ty - 2, tw + 4, lines * 6 + 3, rgba(0, 0, 0, 200));
        c.text(tx, ty, s, rgba(255, 240, 160));
    }

    void overlay_hud(Canvas& c, Stats const& s, World const& w, unsigned threads, bool mt, bool sleeping,
        Tool tool, Mat mat, int radius, bool paused, bool fx, int launched_count)
    {
        std::vector<std::string> lines;
        char b[128];
        auto add = [&](const char* fmt, auto... args) { std::snprintf(b, sizeof b, fmt, args...); lines.emplace_back(b); };
        (void)add;
        add("FPS %d  FRAME %.2f MS%s", s.fps, s.frame_ms, paused ? "  PAUSED" : "");
        add("SIM %.2f  EDIT %.2f  RENDER %.2f", s.sim_ms, s.edit_ms, s.render_ms);
        add("OVERLAY %.2f  UPLOAD %.2f MS", s.overlay_ms, s.upload_ms);
        add("CHUNKS AWAKE %d/%d  REDRAWN %u", w.active_chunks, NUM_CHUNKS, s.rendered_chunks);
        add("MOVED %u CELLS/STEP", w.total_moved);
        add("SAND %u WATER %u STONE %u", s.counts[1], s.counts[2], s.counts[3]);
        add("GAS %u LAVA %u  FX %s", s.counts[4], s.counts[5], fx ? "ON" : "OFF");
        add("THREADS %u  MT %s  SLEEP %s", threads, mt ? "ON" : "OFF", sleeping ? "ON" : "OFF");
        add("TOOL %s  MAT %s  SIZE %d", TOOL_NAMES[int(tool)], MAT_NAMES[int(mat)], radius);
        add("LAUNCHED %d  (Muli rigid bodies)", launched_count);
        lines.emplace_back("F1 HELP");
        int tw = 0;
        for (auto& l : lines) tw = std::max(tw, Canvas::text_width(l));
        c.fbox(2, 2, tw + 6, int(lines.size()) * 7 + 3, rgba(0, 0, 0, 175));
        for (size_t i = 0; i < lines.size(); ++i)
            c.text(5, 5 + int(i) * 7, lines[i], i == lines.size() - 1 ? rgba(150, 200, 255) : rgba(235, 235, 235));
    }

    void overlay_help(Canvas& c)
    {
        static const char* L[] = {
            "CONTROLS",
            "LMB DRAW  RMB ERASE  MMB PICKUP/THROW",
            "1-6 SAND WATER STONE GAS LAVA FIRE",
            "B BRUSH  L LINE  K RECT  C CIRCLE",
            "E ELLIPSE  T TRIANGLE  F FILL  TAB CYCLE",
            "SHIFT = FILLED SHAPES   [ ] SIZE",
            "SPACE PAUSE  . STEP  R RESET  ESC QUIT",
            "F1 HELP  F3 HUD  F4 CHUNKS  F5 VELOCITY",
            "F6 THREADS  F7 GRAPH  F8 MULTITHREAD",
            "F9 CHUNK SLEEP  F10 INSPECTOR  F11 FX",
            "M SPAWN BOX   N SPAWN CIRCLE",
            "RMB CUTS RIGID BODIES  STONE->BODY ON RELEASE",
        };
        int tw = 0;
        for (auto l : L) tw = std::max(tw, Canvas::text_width(l));
        int n = int(sizeof(L) / sizeof(L[0]));
        int w = tw + 12, h = n * 8 + 8;
        int x = (GRID_W - w) / 2, y = (GRID_H - h) / 2;
        c.fbox(x, y, w, h, rgba(10, 14, 24, 225));
        c.box(x, y, w, h, rgba(90, 160, 255, 220));
        for (int i = 0; i < n; ++i) c.text(x + 6, y + 6 + i * 8, L[i], i == 0 ? rgba(255, 230, 120) : rgba(230, 235, 245));
    }
} // namespace

namespace
{
    template <class P>
    inline auto* unwrap_host_ptr(P&& p)
    {
        if constexpr (requires { p.value(); }) return p.value();
        else return p;
    }

    daxa::ImageId make_image(daxa::Device& device, daxa::Format format, daxa::u32 w, daxa::u32 h, char const* name)
    {
        return device.create_image({
            .format = format,
            .size = { w, h, 1 },
            .usage = daxa::ImageUsageFlagBits::SHADER_STORAGE |
                     daxa::ImageUsageFlagBits::TRANSFER_SRC |
                     daxa::ImageUsageFlagBits::TRANSFER_DST,
            .name = name,
            });
    }

    using Clock = std::chrono::steady_clock;
    inline double ms_since(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

    // Polled keyboard with our own edge detection (does not depend on AppWindow's key state).
    class Keys
    {
    public:
        explicit Keys(GLFWwindow* w) : w_(w) { glfwSetInputMode(w_, GLFW_STICKY_KEYS, GLFW_TRUE); }
        bool pressed(int key)
        {
            bool d = glfwGetKey(w_, key) == GLFW_PRESS;
            bool r = d && !prev_[size_t(key)];
            prev_[size_t(key)] = d;
            return r;
        }
        bool held(int key) const { return glfwGetKey(w_, key) == GLFW_PRESS; }

    private:
        GLFWwindow* w_;
        std::array<bool, GLFW_KEY_LAST + 1> prev_{};
    };

    // ---- post-process tuning ---------------------------------------------------
    constexpr float FX_THRESHOLD = 0.62f;  // bloom brightness threshold
    constexpr float FX_BLOOM = 2.0f;       // bloom strength
    constexpr float FX_DISTORTION = 1.0f;  // liquid wobble strength

    struct PostPush
    {
        daxa::ImageViewId img0, img1, img2, img3;
        daxa::u32 width, height;
        float time, threshold, intensity, distortion, pad0, pad1;
    };

    // The scene image is the CPU framebuffer. Its alpha channel is a material mask:
    //   0 = nothing special, ~200 = water, ~120 = lava, 255 = CPU overlay/HUD pixel (left untouched).
    const std::string POST_PREFIX = R"glsl(
#version 450
#include <daxa/daxa.inl>
struct PostPush
{
    daxa_ImageViewId img0;
    daxa_ImageViewId img1;
    daxa_ImageViewId img2;
    daxa_ImageViewId img3;
    uint width;
    uint height;
    float time;
    float threshold;
    float intensity;
    float distortion;
    float pad0;
    float pad1;
};
DAXA_DECL_PUSH_CONSTANT(PostPush, push)
DAXA_DECL_IMAGE_ACCESSOR_WITH_FORMAT(image2D, rgba8, , RGBA8Image)
DAXA_DECL_IMAGE_ACCESSOR_WITH_FORMAT(image2D, rgba16f, , RGBA16fImage)
layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;
)glsl";

    const std::string POST_DISTORT = POST_PREFIX + R"glsl(
vec4 sceneAt(ivec2 p)
{
    p = clamp(p, ivec2(0), ivec2(int(push.width) - 1, int(push.height) - 1));
    return imageLoad(daxa_access(RGBA8Image, push.img0), p);
}
bool isWaterA(float a) { return a > 0.7 && a < 0.99; }
bool isLavaA(float a) { return a > 0.4 && a <= 0.7; }
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(push.width) || p.y >= int(push.height)) return;
    vec4 s = sceneAt(p);
    if (s.a > 0.99)
    {
        imageStore(daxa_access(RGBA16fImage, push.img1), p, vec4(s.rgb, 1.0));
        imageStore(daxa_access(RGBA16fImage, push.img2), p, vec4(0.0));
        return;
    }

    // local water density (5x5) and heat haze from lava below (vertical taps)
    float water = 0.0;
    for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
            if (isWaterA(sceneAt(p + ivec2(dx, dy)).a)) water += 1.0;
    water /= 25.0;
    float haze = 0.0;
    for (int i = 1; i <= 6; ++i)
        if (isLavaA(sceneAt(p + ivec2(0, i * 2)).a)) haze += 1.0 - float(i) / 7.0;
    haze = min(haze / 3.0, 1.0);

    float t = push.time;
    vec2 off = vec2(0.0);
    off.x += sin(float(p.y) * 0.45 + t * 2.6 + float(p.x) * 0.12) * 1.6 * water;
    off.y += cos(float(p.x) * 0.35 + t * 2.1) * 0.9 * water;
    off.x += sin(float(p.y) * 0.8 - t * 5.0 + float(p.x) * 0.2) * 1.8 * haze;
    off *= push.distortion;

    vec4 d = sceneAt(p + ivec2(round(off)));
    if (d.a > 0.99) d = s;
    vec3 col = d.rgb;

    // moving highlights on and around water
    float wave = sin(float(p.x) * 0.25 + float(p.y) * 0.15 + t * 3.0) * sin(float(p.y) * 0.5 - t * 1.7);
    col += vec3(0.05, 0.09, 0.14) * max(wave, 0.0) * water * push.distortion;
    imageStore(daxa_access(RGBA16fImage, push.img1), p, vec4(col, 1.0));

    // bright pass (+ lava is emissive)
    float lum = dot(col, vec3(0.2126, 0.7152, 0.0722));
    vec3 glow = col * smoothstep(push.threshold, push.threshold + 0.25, lum);
    if (isLavaA(s.a)) glow += col * 1.2;
    imageStore(daxa_access(RGBA16fImage, push.img2), p, vec4(glow, 1.0));
}
)glsl";

    std::string post_blur(bool horizontal)
    {
        return POST_PREFIX + std::string("const ivec2 DIR = ") + (horizontal ? "ivec2(1, 0);" : "ivec2(0, 1);") + R"glsl(
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(push.width) || p.y >= int(push.height)) return;
    const int R = 14;
    const float sigma = 6.0;
    vec3 acc = vec3(0.0);
    float ws = 0.0;
    for (int i = -R; i <= R; ++i)
    {
        float w = exp(-float(i * i) / (2.0 * sigma * sigma));
        ivec2 q = clamp(p + DIR * i, ivec2(0), ivec2(int(push.width) - 1, int(push.height) - 1));
        acc += imageLoad(daxa_access(RGBA16fImage, push.img0), q).rgb * w;
        ws += w;
    }
    imageStore(daxa_access(RGBA16fImage, push.img1), p, vec4(acc / ws, 1.0));
}
)glsl";
    }

    const std::string POST_COMPOSITE = POST_PREFIX + R"glsl(
void main()
{
    ivec2 p = ivec2(gl_GlobalInvocationID.xy);
    if (p.x >= int(push.width) || p.y >= int(push.height)) return;
    vec4 s = imageLoad(daxa_access(RGBA8Image, push.img0), p);
    if (s.a > 0.99)
    {
        imageStore(daxa_access(RGBA8Image, push.img3), p, vec4(s.rgb, 1.0));
        return;
    }
    vec3 base = imageLoad(daxa_access(RGBA16fImage, push.img1), p).rgb;
    vec3 bloom = imageLoad(daxa_access(RGBA16fImage, push.img2), p).rgb;
    vec3 col = min(base + bloom * push.intensity, vec3(1.0));
    imageStore(daxa_access(RGBA8Image, push.img3), p, vec4(col, 1.0));
}
)glsl";

    // =========================================================================
    // Muli coordinate helpers (pixel Y-down  <->  Muli Y-up)
    // =========================================================================
    constexpr float PIXELS_PER_METER = 32.0f;
    constexpr float METERS_PER_PIXEL = 1.0f / PIXELS_PER_METER;

    inline muli::Vec2 to_muli(float px, float py)
    {
        return muli::Vec2{ px * METERS_PER_PIXEL, (float(GRID_H) - 1.0f - py) * METERS_PER_PIXEL };
    }
    inline Pt to_pixel(const muli::Vec2& p)
    {
        return {
            int(std::lround(p.x * PIXELS_PER_METER)),
            GRID_H - 1 - int(std::lround(p.y * PIXELS_PER_METER))
        };
    }
} // namespace



int main()
{
    try
    {
        AppWindow window("Voita + Muli", 1280, 720);
        if (window.get_glfw_window() == nullptr)
        {
            std::cerr << "Failed to create GLFW window.\n";
            return 1;
        }
        GLFWwindow* glfw = window.get_glfw_window();
        Keys keys(glfw);

        daxa::Instance instance = daxa::create_instance({});
        auto device_info = instance.choose_device({}, {});
        daxa::Device device = instance.create_device_2(device_info);

        daxa::Swapchain swapchain = device.create_swapchain({
            .native_window_info = window.get_native_window_info(),
            .surface_format = device.choose_swapchain_surface_format({
                .native_window_info = window.get_native_window_info(),
            }),
            .present_mode = daxa::PresentMode::FIFO,
            .image_usage = daxa::ImageUsageFlagBits::TRANSFER_DST,
            .name = "Voita swapchain",
            });

        // ---- post-process pipelines ---------------------------------------------
        daxa::PipelineManager pipeline_manager({
            .device = device,
            .root_paths = { DAXA_SHADER_INCLUDE_DIR },
            .default_language = daxa::ShaderLanguage::GLSL,
            .default_enable_debug_info = true,
            .name = "Voita pipeline manager",
            });
        const std::string blur_h_src = post_blur(true), blur_v_src = post_blur(false);
        auto make_post = [&](std::string const& src, char const* name)
            {
                auto r = pipeline_manager.add_compute_pipeline2({
                    .source = daxa::ShaderCode{ .string = src },
                    .push_constant_size = sizeof(PostPush),
                    .name = name,
                    });
                if (r.is_err()) throw std::runtime_error(std::string("Failed to compile ") + name + ":\n" + r.message());
                return r.value();
            };
        auto distort_pipeline = make_post(POST_DISTORT, "post distort+bright");
        auto blur_h_pipeline = make_post(blur_h_src, "post blur h");
        auto blur_v_pipeline = make_post(blur_v_src, "post blur v");
        auto composite_pipeline = make_post(POST_COMPOSITE, "post composite");

        // scene: CPU framebuffer (uploaded) -> distort/bright -> blur -> composite -> output -> blit
        daxa::ImageId scene = make_image(device, daxa::Format::R8G8B8A8_UNORM, GRID_W, GRID_H, "scene (cpu framebuffer)");
        daxa::ImageId post = make_image(device, daxa::Format::R16G16B16A16_SFLOAT, GRID_W, GRID_H, "post color");
        daxa::ImageId bright = make_image(device, daxa::Format::R16G16B16A16_SFLOAT, GRID_W, GRID_H, "post bright");
        daxa::ImageId blur_a = make_image(device, daxa::Format::R16G16B16A16_SFLOAT, GRID_W, GRID_H, "post blur A");
        daxa::ImageId blur_b = make_image(device, daxa::Format::R16G16B16A16_SFLOAT, GRID_W, GRID_H, "post blur B");
        daxa::ImageId output = make_image(device, daxa::Format::R8G8B8A8_UNORM, GRID_W, GRID_H, "post output");

        constexpr int STAGING_RING = 3;
        std::array<daxa::BufferId, STAGING_RING> staging{};
        std::array<u32*, STAGING_RING> staging_ptr{};
        for (int i = 0; i < STAGING_RING; ++i)
        {
            staging[size_t(i)] = device.create_buffer({
                .size = sizeof(u32) * GRID_W * GRID_H,
                .memory_flags = daxa::MemoryFlagBits::HOST_ACCESS_SEQUENTIAL_WRITE,
                .name = "framebuffer staging",
                });
            staging_ptr[size_t(i)] = reinterpret_cast<u32*>(unwrap_host_ptr(device.buffer_host_address(staging[size_t(i)])));
        }
        {
            daxa::CommandRecorder rec = device.create_command_recorder({ .name = "init images" });
            for (auto img : { scene, post, bright, blur_a, blur_b, output })
                rec.pipeline_image_barrier({
                    .dst_access = daxa::AccessConsts::COMPUTE_SHADER_READ_WRITE,
                    .image = img,
                    .layout_operation = daxa::ImageLayoutOperation::TO_GENERAL,
                    });
            device.submit_commands({ .command_lists = std::array{ rec.complete_current_commands() } });
        }
        auto rw = [](daxa::CommandRecorder& rec, daxa::ImageId img)
            {
                rec.pipeline_image_barrier({
                    .src_access = daxa::AccessConsts::COMPUTE_SHADER_READ_WRITE,
                    .dst_access = daxa::AccessConsts::COMPUTE_SHADER_READ_WRITE,
                    .image = img,
                    });
            };
        auto dispatch_grid = [](daxa::CommandRecorder& rec)
            {
                rec.dispatch({ .x = (GRID_W + 7) / 8, .y = (GRID_H + 7) / 8, .z = 1 });
            };

        // ---- CPU side ----------------------------------------------------------
        unsigned hw = std::max(1u, std::min(8u, std::thread::hardware_concurrency()));
        ThreadPool pool(hw - 1);
        World world;
        std::vector<u32> world_pixels(size_t(GRID_W) * GRID_H), frame_pixels(size_t(GRID_W) * GRID_H);
        Canvas world_fb(world_pixels.data(), GRID_W, GRID_H);
        Canvas frame_fb(frame_pixels.data(), GRID_W, GRID_H);
        world.render(world_fb, pool, true, true);

        // ---- Muli rigid-body world (runs in parallel with cellular sim) --------
        muli::WorldSettings muli_settings;
        muli_settings.gravity = muli::Vec2{ 0.0f, -12.0f };
        muli_settings.apply_gravity = true;
        muli_settings.sleeping = true;
        muli::World muli_world(muli_settings);

        // Static floor + side walls (roughly matching the pixel grid)
        {
            const float W = GRID_W * METERS_PER_PIXEL;
            const float H = GRID_H * METERS_PER_PIXEL;
            muli_world.CreateBox(W + 2.0f, 0.5f,
                muli::Transform{ muli::Vec2{ W * 0.5f, -0.25f } },
                muli::Body::static_body);
            // left / right walls
            muli_world.CreateBox(0.5f, H + 1.0f,
                muli::Transform{ muli::Vec2{ -0.25f, H * 0.5f } },
                muli::Body::static_body);
            muli_world.CreateBox(0.5f, H + 1.0f,
                muli::Transform{ muli::Vec2{ W + 0.25f, H * 0.5f } },
                muli::Body::static_body);
        }

        // Destructible rigid bodies + solid occupancy mask (u16 body index+1, 0 = free)
        std::vector<DestructibleBody> d_bodies;
        d_bodies.reserve(64);
        std::vector<u16> solid_id(size_t(GRID_W) * GRID_H, 0);
        constexpr float PIXEL_SPACING = METERS_PER_PIXEL; // one sample per pixel

        auto register_box = [&](float cx, float cy, float half_w, float half_h, u32 col)
            {
                DestructibleBody db;
                sample_box_local(db.local_pixels, half_w, half_h, PIXEL_SPACING);
                db.body = muli_world.CreateBox(half_w * 2.0f, half_h * 2.0f,
                    muli::Transform{ muli::Vec2{ cx, cy } }, muli::Body::dynamic_body);
                if (db.body)
                {
                    db.body->SetFriction(0.55f);
                    db.body->SetRestitution(0.12f);
                }
                db.color = col;
                d_bodies.push_back(std::move(db));
            };
        auto register_circle = [&](float cx, float cy, float radius, u32 col)
            {
                DestructibleBody db;
                sample_circle_local(db.local_pixels, radius, PIXEL_SPACING);
                db.body = muli_world.CreateCircle(radius,
                    muli::Transform{ muli::Vec2{ cx, cy } }, muli::Body::dynamic_body);
                if (db.body)
                {
                    db.body->SetFriction(0.5f);
                    db.body->SetRestitution(0.35f);
                }
                db.color = col;
                d_bodies.push_back(std::move(db));
            };

        // Starter bodies
        for (int i = 0; i < 4; ++i)
            register_box(4.0f + i * 1.8f, 9.0f + i * 0.4f, 0.35f + 0.08f * (i % 2), 0.35f,
                rgba(240, 190 + i * 10, 70, 255));
        register_circle(12.0f, 11.0f, 0.45f, rgba(100, 200, 255, 255));

        // Launched particles (ballistic) created when rigid bodies hit sand/water
        std::vector<LaunchedParticle> launched;
        launched.reserve(4096);
        Rng launch_rng{ 0xDEADBEEFu };

        bool paused = false, multithread = true, sleeping = true, fx = true;
        u32 dbg = DBG_HUD;
        Tool tool = Tool::Brush;
        Mat material = Mat::Sand;
        int brush_radius = 5;

        bool was_down = false, dragging = false, drag_erase = false;
        bool painted_stone = false; // convert connected stone to rigid body on release
        Pt drag_start{ 0, 0 }, last_pos{ 0, 0 };

        // Middle-mouse grab state
        muli::Body* grabbed_body = nullptr;
        size_t grabbed_bi = size_t(-1);
        std::vector<LaunchedParticle> grabbed_clump;
        bool mmb_was_down = false;

        Stats stats;
        double sim_accum = 0.0;
        int title_frames = 0;
        double title_accum = 0.0;
        u32 frame_counter = 0;
        const auto t0 = Clock::now();
        auto previous_time = Clock::now();

        while (!window.should_close())
        {
            auto frame_start = Clock::now();
            glfwPollEvents();
            window.update();

            // ---- keys (polled directly) ------------------------------------------
            if (keys.pressed(GLFW_KEY_ESCAPE)) window.request_close();
            if (keys.pressed(GLFW_KEY_SPACE)) paused = !paused;
            bool step_once = keys.pressed(GLFW_KEY_PERIOD);
            if (keys.pressed(GLFW_KEY_F1)) dbg ^= DBG_HELP;
            if (keys.pressed(GLFW_KEY_F3)) dbg ^= DBG_HUD;
            if (keys.pressed(GLFW_KEY_F4)) dbg ^= DBG_CHUNKS;
            if (keys.pressed(GLFW_KEY_F5)) dbg ^= DBG_VELOCITY;
            if (keys.pressed(GLFW_KEY_F6)) dbg ^= DBG_THREADS;
            if (keys.pressed(GLFW_KEY_F7)) dbg ^= DBG_GRAPH;
            if (keys.pressed(GLFW_KEY_F8)) multithread = !multithread;
            if (keys.pressed(GLFW_KEY_F9)) sleeping = !sleeping;
            if (keys.pressed(GLFW_KEY_F10)) dbg ^= DBG_INSPECT;
            if (keys.pressed(GLFW_KEY_F11)) fx = !fx;
            if (keys.pressed(GLFW_KEY_LEFT_BRACKET)) brush_radius = std::max(1, brush_radius - 1);
            if (keys.pressed(GLFW_KEY_RIGHT_BRACKET)) brush_radius = std::min(64, brush_radius + 1);
            if (keys.pressed(GLFW_KEY_1) || keys.pressed(GLFW_KEY_KP_1)) material = Mat::Sand;
            if (keys.pressed(GLFW_KEY_2) || keys.pressed(GLFW_KEY_KP_2)) material = Mat::Water;
            if (keys.pressed(GLFW_KEY_3) || keys.pressed(GLFW_KEY_KP_3)) material = Mat::Stone;
            if (keys.pressed(GLFW_KEY_4) || keys.pressed(GLFW_KEY_KP_4)) material = Mat::Gas;
            if (keys.pressed(GLFW_KEY_5) || keys.pressed(GLFW_KEY_KP_5)) material = Mat::Lava;
            if (keys.pressed(GLFW_KEY_6) || keys.pressed(GLFW_KEY_KP_6)) material = Mat::Fire;
            if (keys.pressed(GLFW_KEY_B)) tool = Tool::Brush;
            if (keys.pressed(GLFW_KEY_L)) tool = Tool::Line;
            if (keys.pressed(GLFW_KEY_K)) tool = Tool::Rect;
            if (keys.pressed(GLFW_KEY_C)) tool = Tool::Circle;
            if (keys.pressed(GLFW_KEY_E)) tool = Tool::Ellipse;
            if (keys.pressed(GLFW_KEY_T)) tool = Tool::Triangle;
            if (keys.pressed(GLFW_KEY_F)) tool = Tool::Fill;
            if (keys.pressed(GLFW_KEY_TAB)) tool = Tool((int(tool) + 1) % int(Tool::COUNT));
            if (keys.pressed(GLFW_KEY_R))
            {
                world.reset();
                launched.clear();
                // Destroy all dynamic destructible bodies
                for (auto& db : d_bodies)
                    if (db.body) muli_world.Destroy(db.body);
                d_bodies.clear();
                std::fill(solid_id.begin(), solid_id.end(), u16(0));
                dragging = false;
            }
            // Spawn extra Muli bodies at cursor (M = box, N = circle)
            if (keys.pressed(GLFW_KEY_M) || keys.pressed(GLFW_KEY_N))
            {
                double mx = 0, my = 0;
                glfwGetCursorPos(glfw, &mx, &my);
                int ww = 1, wh = 1;
                glfwGetWindowSize(glfw, &ww, &wh);
                Pt cur{
                    std::clamp(int(mx * GRID_W / std::max(ww, 1)), 0, GRID_W - 1),
                    std::clamp(int(my * GRID_H / std::max(wh, 1)), 0, GRID_H - 1) };
                muli::Vec2 pos = to_muli(float(cur.x), float(cur.y));
                if (keys.pressed(GLFW_KEY_M))
                {
                    float hw = 0.3f + 0.15f * (launch_rng.below(100) / 100.f);
                    register_box(pos.x, pos.y, hw, hw, rgba(240, 200, 80, 255));
                }
                else
                {
                    float r = 0.28f + 0.15f * (launch_rng.below(100) / 100.f);
                    register_circle(pos.x, pos.y, r, rgba(100, 200, 255, 255));
                }
            }
            const bool shift = keys.held(GLFW_KEY_LEFT_SHIFT) || keys.held(GLFW_KEY_RIGHT_SHIFT);

            auto now = Clock::now();
            double dt = std::min(0.1, std::chrono::duration<double>(now - previous_time).count());
            previous_time = now;

            if (window.minimized || window.width == 0 || window.height == 0)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(16));
                continue;
            }

            // ---- mouse -> grid -------------------------------------------------
            double mouse_x = 0.0, mouse_y = 0.0;
            glfwGetCursorPos(glfw, &mouse_x, &mouse_y);
            int win_w = 1, win_h = 1;
            glfwGetWindowSize(glfw, &win_w, &win_h);
            Pt cur{
                std::clamp(int(mouse_x * GRID_W / std::max(win_w, 1)), 0, GRID_W - 1),
                std::clamp(int(mouse_y * GRID_H / std::max(win_h, 1)), 0, GRID_H - 1) };

            // ---- editing (same API as the preview below) -------------------------
            auto t_edit = Clock::now();
            const bool lmb = window.mouse_left, rmb = window.mouse_right;
            const bool mmb = glfwGetMouseButton(glfw, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;
            const bool down = lmb || rmb;
            if (down && !was_down)
            {
                dragging = true;
                drag_erase = rmb && !lmb;
                painted_stone = false;
                drag_start = cur;
                last_pos = cur;
                if (tool == Tool::Fill)
                {
                    world.overwrite = true;
                    world.flood(cur.x, cur.y, drag_erase ? Mat::Empty : material);
                    if (!drag_erase && material == Mat::Stone) painted_stone = true;
                    dragging = false;
                }
            }
            if (dragging && tool == Tool::Brush)
            {
                // Fluids/gas/lava are sprayed (random fraction of the brush) so they pour instead of
                // dropping a solid disc. Fire is also sprayed for a natural plume.
                world.overwrite = drag_erase || material == Mat::Stone;
                world.density = (drag_erase || material == Mat::Stone) ? 255u
                    : (material == Mat::Gas ? 70u : (material == Mat::Fire ? 90u : 120u));
                apply_tool(world, Tool::Brush, last_pos, cur, drag_erase ? Mat::Empty : material, true, brush_radius);
                if (!drag_erase && material == Mat::Stone) painted_stone = true;

                // RMB brush also cuts rigid-body pixels (destructible)
                if (drag_erase)
                {
                    const int r = brush_radius;
                    const int r2 = r * r;
                    for (int dy = -r; dy <= r; ++dy)
                        for (int dx = -r; dx <= r; ++dx)
                        {
                            if (dx * dx + dy * dy > r2) continue;
                            int px = cur.x + dx, py = cur.y + dy;
                            if (!world.inside(px, py)) continue;
                            u16 id = solid_id[size_t(py) * GRID_W + px];
                            if (id == 0) continue;
                            size_t bi = size_t(id - 1);
                            if (bi >= d_bodies.size() || !d_bodies[bi].body) continue;

                            // Remove local pixels that currently map near this screen pixel
                            auto& db = d_bodies[bi];
                            const muli::Transform& tf = db.body->GetTransform();
                            muli::Vec2 world_pt = to_muli(float(px) + 0.5f, float(py) + 0.5f);
                            // Inverse transform roughly: bring world point into body local
                            muli::Vec2 dlt = world_pt - tf.position;
                            float ca = std::cos(-tf.rotation.GetAngle()), sa = std::sin(-tf.rotation.GetAngle());
                            muli::Vec2 local{ dlt.x * ca - dlt.y * sa, dlt.x * sa + dlt.y * ca };
                            const float kill_r = PIXEL_SPACING * 1.1f;
                            const float kill_r2 = kill_r * kill_r;
                            auto& lp = db.local_pixels;
                            for (size_t k = 0; k < lp.size(); )
                            {
                                float ddx = lp[k].x - local.x, ddy = lp[k].y - local.y;
                                if (ddx * ddx + ddy * ddy <= kill_r2)
                                {
                                    lp[k] = lp.back();
                                    lp.pop_back();
                                    db.dirty = true;
                                }
                                else ++k;
                            }
                        }
                }
                last_pos = cur;
            }
            if (!down && was_down && dragging)
            {
                if (tool != Tool::Brush && tool != Tool::Fill)
                {
                    world.overwrite = true;
                    world.density = 255;
                    apply_tool(world, tool, drag_start, cur, drag_erase ? Mat::Empty : material, shift, brush_radius);
                    if (!drag_erase && material == Mat::Stone) painted_stone = true;
                }
                dragging = false;
            }
            // On release after painting stone: turn the connected stone blob into a dynamic rigid body
            if (!down && was_down && painted_stone)
            {
                painted_stone = false;
                // Flood-collect connected stone from the release point (or drag start)
                Pt seed = world.inside(cur.x, cur.y) && world.get(cur.x, cur.y) == Mat::Stone ? cur : drag_start;
                if (world.inside(seed.x, seed.y) && world.get(seed.x, seed.y) == Mat::Stone)
                {
                    std::vector<Pt> blob;
                    std::vector<u8> seen(size_t(GRID_W) * GRID_H, 0);
                    std::vector<Pt> stack;
                    stack.push_back(seed);
                    while (!stack.empty())
                    {
                        Pt p = stack.back(); stack.pop_back();
                        if (!world.inside(p.x, p.y)) continue;
                        size_t ii = size_t(p.y) * GRID_W + p.x;
                        if (seen[ii]) continue;
                        if (world.get(p.x, p.y) != Mat::Stone) continue;
                        seen[ii] = 1;
                        blob.push_back(p);
                        stack.push_back({ p.x + 1, p.y });
                        stack.push_back({ p.x - 1, p.y });
                        stack.push_back({ p.x, p.y + 1 });
                        stack.push_back({ p.x, p.y - 1 });
                    }
                    if (blob.size() >= 8)
                    {
                        // Centroid in pixel space → Muli, build local pixels
                        double cx = 0, cy = 0;
                        for (auto const& p : blob) { cx += p.x; cy += p.y; }
                        cx /= double(blob.size()); cy /= double(blob.size());
                        muli::Vec2 world_center = to_muli(float(cx), float(cy));

                        DestructibleBody db;
                        db.local_pixels.reserve(blob.size());
                        for (auto const& p : blob)
                        {
                            muli::Vec2 wp = to_muli(float(p.x) + 0.5f, float(p.y) + 0.5f);
                            db.local_pixels.push_back({ wp.x - world_center.x, wp.y - world_center.y });
                            // remove stone from cellular grid
                            world.cell_mut(p.x, p.y) = Cell{};
                            world.force_touch(p.x, p.y);
                        }
                        float minx = db.local_pixels[0].x, maxx = minx, miny = db.local_pixels[0].y, maxy = miny;
                        for (auto const& p : db.local_pixels)
                        {
                            minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                            miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
                        }
                        float w = std::max(0.1f, maxx - minx), h = std::max(0.1f, maxy - miny);
                        db.body = muli_world.CreateBox(w, h,
                            muli::Transform{ world_center }, muli::Body::dynamic_body);
                        if (db.body)
                        {
                            db.body->SetFriction(0.7f);
                            db.body->SetRestitution(0.05f);
                        }
                        db.color = rgba(130, 130, 140, 255);
                        d_bodies.push_back(std::move(db));
                    }
                }
            }
            was_down = down;
            world.overwrite = true;
            world.density = 255;
            stats.ema(stats.edit_ms, ms_since(t_edit));

            // ---- Middle mouse: pick up rigid body OR clump of sand/water ----
            if (mmb && !mmb_was_down)
            {
                grabbed_body = nullptr;
                grabbed_bi = size_t(-1);
                grabbed_clump.clear();
                // Prefer rigid body under cursor
                u16 id = solid_id[size_t(cur.y) * GRID_W + cur.x];
                if (id != 0 && size_t(id - 1) < d_bodies.size() && d_bodies[size_t(id - 1)].body)
                {
                    grabbed_bi = size_t(id - 1);
                    grabbed_body = d_bodies[grabbed_bi].body;
                    grabbed_body->SetType(muli::Body::kinematic_body);
                    grabbed_body->SetLinearVelocity(0, 0);
                    grabbed_body->SetAngularVelocity(0);
                }
                else
                {
                    // Grab a clump of sand/water around cursor
                    const int rad = std::max(4, brush_radius);
                    const int r2 = rad * rad;
                    for (int dy = -rad; dy <= rad; ++dy)
                        for (int dx = -rad; dx <= rad; ++dx)
                        {
                            if (dx * dx + dy * dy > r2) continue;
                            int px = cur.x + dx, py = cur.y + dy;
                            if (!world.inside(px, py)) continue;
                            Mat m = world.get(px, py);
                            if (m != Mat::Sand && m != Mat::Water) continue;
                            Cell& c = world.cell_mut(px, py);
                            u8 var = c.var;
                            c = Cell{};
                            world.force_touch(px, py);
                            grabbed_clump.push_back({
                                float(px) + 0.5f, float(py) + 0.5f,
                                0.f, 0.f, m, var, 9999
                                });
                        }
                }
            }
            if (mmb)
            {
                if (grabbed_body)
                {
                    muli::Vec2 target = to_muli(float(cur.x), float(cur.y));
                    muli::Vec2 pos = grabbed_body->GetPosition();
                    muli::Vec2 vel = (target - pos) * 12.0f; // spring toward cursor
                    grabbed_body->SetLinearVelocity(vel);
                    grabbed_body->SetAngularVelocity(grabbed_body->GetAngularVelocity() * 0.9f);
                }
                for (auto& p : grabbed_clump)
                {
                    // Follow cursor with slight scatter
                    p.x = float(cur.x) + (p.x - float(cur.x)) * 0.4f + float(int(launch_rng.below(5)) - 2) * 0.15f;
                    p.y = float(cur.y) + (p.y - float(cur.y)) * 0.4f + float(int(launch_rng.below(5)) - 2) * 0.15f;
                }
            }
            if (!mmb && mmb_was_down)
            {
                // Release: restore dynamic + fling with mouse velocity estimate
                if (grabbed_body)
                {
                    grabbed_body->SetType(muli::Body::dynamic_body);
                    // small upward/outward flick
                    muli::Vec2 v = grabbed_body->GetLinearVelocity();
                    grabbed_body->SetLinearVelocity(v * 1.4f);
                    grabbed_body = nullptr;
                    grabbed_bi = size_t(-1);
                }
                if (!grabbed_clump.empty())
                {
                    for (auto& p : grabbed_clump)
                    {
                        p.vx = float(int(launch_rng.below(7)) - 3) * 0.8f;
                        p.vy = -2.5f - float(launch_rng.below(20)) * 0.1f;
                        p.life = 80 + int(launch_rng.below(40));
                        launched.push_back(p);
                    }
                    grabbed_clump.clear();
                }
            }
            mmb_was_down = mmb;

            // ---- simulation (fixed 60 Hz) ------------------------------------------
            auto t_sim = Clock::now();

            // Stamp rigid-body solid occupancy so the cellular sim treats them as solid
            {
                std::fill(solid_id.begin(), solid_id.end(), u16(0));
                for (size_t bi = 0; bi < d_bodies.size(); ++bi)
                {
                    auto& db = d_bodies[bi];
                    if (!db.body || db.local_pixels.empty()) continue;
                    const muli::Transform& tf = db.body->GetTransform();
                    float ca = std::cos(tf.rotation.GetAngle()), sa = std::sin(tf.rotation.GetAngle());
                    for (auto const& lp : db.local_pixels)
                    {
                        float wx = lp.x * ca - lp.y * sa + tf.position.x;
                        float wy = lp.x * sa + lp.y * ca + tf.position.y;
                        Pt pix = to_pixel(muli::Vec2{ wx, wy });
                        if (world.inside(pix.x, pix.y))
                            solid_id[size_t(pix.y) * GRID_W + pix.x] = u16(bi + 1);
                    }
                }
                world.set_solid_mask(solid_id.data());
            }

            // Rebuild any bodies that were cut this frame (before stepping physics)
            for (size_t bi = 0; bi < d_bodies.size(); ++bi)
            {
                auto& db = d_bodies[bi];
                if (!db.dirty || !db.body) continue;
                db.dirty = false;
                muli::Transform old_tf = db.body->GetTransform();
                muli::Vec2 old_vel = db.body->GetLinearVelocity();
                float old_angvel = db.body->GetAngularVelocity();
                muli_world.Destroy(db.body);
                db.body = nullptr;

                if (db.local_pixels.size() < 4)
                {
                    db.local_pixels.clear();
                    continue;
                }
                // Re-centre local pixels around new AABB centre so mass is correct
                float minx = db.local_pixels[0].x, maxx = minx, miny = db.local_pixels[0].y, maxy = miny;
                for (auto const& p : db.local_pixels)
                {
                    minx = std::min(minx, p.x); maxx = std::max(maxx, p.x);
                    miny = std::min(miny, p.y); maxy = std::max(maxy, p.y);
                }
                muli::Vec2 mid{ (minx + maxx) * 0.5f, (miny + maxy) * 0.5f };
                for (auto& p : db.local_pixels) { p.x -= mid.x; p.y -= mid.y; }

                db.body = rebuild_body_from_pixels(muli_world, db.local_pixels, old_tf, db.color);
                if (db.body)
                {
                    // Preserve momentum as much as possible
                    db.body->SetLinearVelocity(old_vel);
                    db.body->SetAngularVelocity(old_angvel);
                }
                else
                {
                    db.local_pixels.clear();
                }
            }

            int steps = 0;
            if (paused)
            {
                sim_accum = 0.0;
                if (step_once)
                {
                    world.step(pool, multithread, sleeping);
                    muli_world.Step(float(SIM_DT));
                    steps = 1;
                }
            }
            else
            {
                sim_accum += dt;
                while (sim_accum >= SIM_DT && steps < 4)
                {
                    // 1) Cellular sand/water/lava/gas (solid_mask blocks phasing through boxes)
                    world.step(pool, multithread, sleeping);

                    // 2) Muli rigid-body step
                    muli_world.Step(float(SIM_DT));

                    // 3) Interaction: only splash when a rigid body is *falling*
                    //    (significant downward velocity). Splash amount scales with impact speed.
                    for (muli::Body* b = muli_world.GetBodyList(); b; b = b->GetNext())
                    {
                        if (b->GetType() != muli::Body::dynamic_body || !b->IsEnabled()) continue;

                        muli::Vec2 lin = b->GetLinearVelocity();
                        // Muli Y-up: falling means lin.y is negative. Require a real impact.
                        constexpr float FALL_THRESHOLD = -1.8f; // m/s
                        if (lin.y > FALL_THRESHOLD) continue;   // not falling hard enough → no splash

                        // Impact strength 0..1 (clamped). Stronger falls = bigger splash.
                        float speed = std::sqrt(lin.x * lin.x + lin.y * lin.y);
                        float impact = std::clamp((-lin.y - 1.0f) / 10.0f, 0.0f, 1.0f); // based on downward speed
                        impact = std::max(impact, std::clamp((speed - 2.0f) / 12.0f, 0.0f, 1.0f));

                        const muli::Transform& tf = b->GetTransform();
                        Pt center = to_pixel(tf.position);

                        // Radius grows a little with impact (harder hits disturb a wider area)
                        const int rad = 12 + int(impact * 14.0f);
                        int x0 = std::max(0, center.x - rad);
                        int x1 = std::min(GRID_W - 1, center.x + rad);
                        int y0 = std::max(0, center.y - rad);
                        int y1 = std::min(GRID_H - 1, center.y + rad);

                        // Pixel-space velocity contribution for launch direction
                        float pvx = lin.x * PIXELS_PER_METER * float(SIM_DT) * 5.0f;
                        float pvy = -lin.y * PIXELS_PER_METER * float(SIM_DT) * 5.0f; // flip Y

                        // Probability a given overlapping cell gets launched (higher impact → denser splash)
                        const u32 launch_chance = 40u + u32(impact * 180.0f); // 40..220 out of 256

                        for (int y = y0; y <= y1; ++y)
                        {
                            for (int x = x0; x <= x1; ++x)
                            {
                                int dx = x - center.x, dy = y - center.y;
                                if (dx * dx + dy * dy > rad * rad) continue;

                                Cell& c = world.cell_mut(x, y);
                                if (c.type != u8(Mat::Sand) && c.type != u8(Mat::Water)) continue;

                                // Stochastic density so light taps only fling a few grains
                                if (launch_rng.below(256) >= launch_chance) continue;

                                Mat mt = Mat(c.type);
                                u8 var = c.var;
                                c = Cell{};
                                world.force_touch(x, y);

                                // Launch strength scales with impact
                                float up = (2.5f + impact * 7.0f) + float(launch_rng.below(40)) * 0.08f;
                                float side = (float(launch_rng.below(200)) - 100.0f) * (0.03f + impact * 0.06f);
                                float life_scale = 60.0f + impact * 50.0f;

                                launched.push_back({
                                    float(x) + 0.5f,
                                    float(y) + 0.5f,
                                    pvx * 0.7f + side,
                                    pvy * 0.55f - up,             // negative vy = up the screen
                                    mt,
                                    var,
                                    int(life_scale) + int(launch_rng.below(30))
                                    });
                            }
                        }
                    }

                    // 4) Integrate launched particles (simple ballistic + gravity)
                    {
                        constexpr float GRAV = 0.35f; // pixels / frame^2
                        for (size_t i = 0; i < launched.size(); )
                        {
                            LaunchedParticle& p = launched[i];
                            p.vy += GRAV;
                            p.x += p.vx;
                            p.y += p.vy;
                            --p.life;

                            int ix = int(p.x), iy = int(p.y);
                            bool out = !world.inside(ix, iy);
                            bool land = false;
                            if (!out)
                            {
                                Mat below = world.get(ix, iy);
                                // Land on solid or denser material
                                if (below == Mat::Stone || below == Mat::Sand ||
                                    (p.type == Mat::Water && below == Mat::Water) ||
                                    (p.vy > 0.0f && below != Mat::Empty && below != Mat::Gas))
                                {
                                    land = true;
                                }
                            }

                            if (out || land || p.life <= 0)
                            {
                                // Re-insert into the cellular grid if possible
                                int rx = std::clamp(ix, 0, GRID_W - 1);
                                int ry = std::clamp(iy, 0, GRID_H - 1);
                                // Search a little upward for an empty cell
                                for (int try_y = ry; try_y >= 0 && try_y > ry - 8; --try_y)
                                {
                                    if (world.get(rx, try_y) == Mat::Empty)
                                    {
                                        Cell nc = world.make_cell_public(p.type);
                                        nc.var = p.var;
                                        nc.vel = 1;
                                        world.cell_mut(rx, try_y) = nc;
                                        world.force_touch(rx, try_y);
                                        break;
                                    }
                                }
                                // swap-remove
                                launched[i] = launched.back();
                                launched.pop_back();
                            }
                            else
                            {
                                ++i;
                            }
                        }
                    }

                    sim_accum -= SIM_DT;
                    ++steps;
                }
                if (steps == 4) sim_accum = 0.0;
            }
            stats.ema(stats.sim_ms, ms_since(t_sim));

            // ---- render world (dirty chunks only) ---------------------------------
            auto t_render = Clock::now();
            stats.rendered_chunks = world.render(world_fb, pool, multithread);
            stats.ema(stats.render_ms, ms_since(t_render));

            // ---- overlays on a copy (opaque overlay pixels get alpha 255 = "skip post") ----
            auto t_overlay = Clock::now();
            std::memcpy(frame_pixels.data(), world_pixels.data(), frame_pixels.size() * sizeof(u32));
            if ((frame_counter % 10) == 0) world.count_types(stats.counts);
            if (dbg & DBG_VELOCITY) overlay_velocity(frame_fb, world);
            if (dbg & DBG_THREADS) overlay_threads(frame_fb, world);
            if (dbg & DBG_CHUNKS) overlay_chunks(frame_fb, world);

            // Draw launched + grabbed clump particles
            auto draw_particle = [&](LaunchedParticle const& p, u32 alpha)
                {
                    int ix = int(p.x), iy = int(p.y);
                    if (!frame_fb.inside(ix, iy)) return;
                    u32 col = with_alpha(MAT_BASE[int(p.type) < MAT_COUNT ? int(p.type) : 0], alpha);
                    frame_fb.plot(ix, iy, col);
                    if (frame_fb.inside(ix, iy - 1)) frame_fb.plot(ix, iy - 1, with_alpha(col, alpha / 2));
                };
            for (auto const& p : launched) draw_particle(p, 220);
            for (auto const& p : grabbed_clump) draw_particle(p, 255);

            // Draw destructible rigid bodies from their solid pixels (cuts are visible)
            for (auto const& db : d_bodies)
            {
                if (!db.body || db.local_pixels.empty()) continue;
                const muli::Transform& tf = db.body->GetTransform();
                float ca = std::cos(tf.rotation.GetAngle()), sa = std::sin(tf.rotation.GetAngle());
                u32 col = with_alpha(db.color ? db.color : rgba(240, 200, 80), 230);
                for (auto const& lp : db.local_pixels)
                {
                    float wx = lp.x * ca - lp.y * sa + tf.position.x;
                    float wy = lp.x * sa + lp.y * ca + tf.position.y;
                    Pt pix = to_pixel(muli::Vec2{ wx, wy });
                    if (frame_fb.inside(pix.x, pix.y))
                        frame_fb.plot(pix.x, pix.y, col);
                }
            }

            {
                // Tool preview: the very same apply_tool() call, but targeting the Canvas.
                u32 col = drag_erase ? rgba(255, 90, 90, 190) : with_alpha(MAT_BASE[int(material)], 190);
                if (dragging && tool != Tool::Brush && tool != Tool::Fill)
                    apply_tool(frame_fb, tool, drag_start, cur, col, shift, brush_radius);
                if (tool == Tool::Brush)
                    frame_fb.circle(cur.x, cur.y, brush_radius, rgba(255, 255, 255, 130));
                else
                {
                    frame_fb.hline(cur.x - 3, cur.x + 3, cur.y, rgba(255, 255, 255, 170));
                    frame_fb.vline(cur.x, cur.y - 3, cur.y + 3, rgba(255, 255, 255, 170));
                }
            }
            if (dbg & DBG_INSPECT) overlay_inspector(frame_fb, world, cur);
            if (dbg & DBG_GRAPH) overlay_graph(frame_fb, stats);
            if (dbg & DBG_HUD) overlay_hud(frame_fb, stats, world, pool.size(), multithread, sleeping, tool, material, brush_radius, paused, fx, int(launched.size()));
            if (dbg & DBG_HELP) overlay_help(frame_fb);
            stats.ema(stats.overlay_ms, ms_since(t_overlay));

            // ---- present ----------------------------------------------------------
            daxa::ImageId swap_image = swapchain.acquire_next_image();
            if (swap_image.is_empty()) continue;

            auto t_upload = Clock::now();
            const size_t slot = frame_counter % STAGING_RING;
            std::memcpy(staging_ptr[slot], frame_pixels.data(), frame_pixels.size() * sizeof(u32));
            stats.ema(stats.upload_ms, ms_since(t_upload));

            const float time_s = std::chrono::duration<float>(Clock::now() - t0).count();
            const float fx_dist = fx ? FX_DISTORTION : 0.0f;
            const float fx_bloom = fx ? FX_BLOOM : 0.0f;
            auto push = [&](daxa::ImageId a, daxa::ImageId b, daxa::ImageId c, daxa::ImageId d)
                {
                    return PostPush{
                        a.default_view(), b.default_view(), c.default_view(), d.default_view(),
                        GRID_W, GRID_H, time_s, FX_THRESHOLD, fx_bloom, fx_dist, 0.0f, 0.0f };
                };

            daxa::CommandRecorder recorder = device.create_command_recorder({ .name = "cpu sand frame" });

            // 1) upload CPU framebuffer into `scene`
            recorder.pipeline_image_barrier({
                .src_access = daxa::AccessConsts::COMPUTE_SHADER_READ_WRITE,
                .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
                .image = scene,
                });
            recorder.copy_buffer_to_image({
                .src_buffer = staging[slot],
                .dst_image = scene,
                .image_slice = { .mip_level = 0, .base_array_layer = 0, .layer_count = 1 },
                .image_offset = { 0, 0, 0 },
                .image_extent = { GRID_W, GRID_H, 1 },
                });
            recorder.pipeline_image_barrier({
                .src_access = daxa::AccessConsts::TRANSFER_WRITE,
                .dst_access = daxa::AccessConsts::COMPUTE_SHADER_READ_WRITE,
                .image = scene,
                });

            // 2) liquid distortion + bright pass: scene -> post, bright
            rw(recorder, post);
            rw(recorder, bright);
            recorder.set_pipeline(*distort_pipeline);
            recorder.push_constant(push(scene, post, bright, scene));
            dispatch_grid(recorder);

            // 3) separable gaussian bloom: bright -> blur_a -> blur_b
            rw(recorder, bright);
            rw(recorder, blur_a);
            recorder.set_pipeline(*blur_h_pipeline);
            recorder.push_constant(push(bright, blur_a, scene, scene));
            dispatch_grid(recorder);
            rw(recorder, blur_a);
            rw(recorder, blur_b);
            recorder.set_pipeline(*blur_v_pipeline);
            recorder.push_constant(push(blur_a, blur_b, scene, scene));
            dispatch_grid(recorder);

            // 4) composite: scene (overlay mask) + post + bloom -> output
            rw(recorder, post);
            rw(recorder, blur_b);
            recorder.pipeline_image_barrier({
                .src_access = daxa::AccessConsts::TRANSFER_READ,
                .dst_access = daxa::AccessConsts::COMPUTE_SHADER_READ_WRITE,
                .image = output,
                });
            recorder.set_pipeline(*composite_pipeline);
            recorder.push_constant(push(scene, post, blur_b, output));
            dispatch_grid(recorder);

            // 5) blit output -> swapchain
            recorder.pipeline_image_barrier({
                .src_access = daxa::AccessConsts::COMPUTE_SHADER_READ_WRITE,
                .dst_access = daxa::AccessConsts::TRANSFER_READ,
                .image = output,
                });
            recorder.pipeline_image_barrier({
                .dst_access = daxa::AccessConsts::TRANSFER_WRITE,
                .image = swap_image,
                .layout_operation = daxa::ImageLayoutOperation::TO_GENERAL,
                });
            recorder.blit_image_to_image({
                .src_image = output,
                .dst_image = swap_image,
                .src_slice = { .mip_level = 0 },
                .src_offsets = { { { 0, 0, 0 },{ GRID_W, GRID_H, 1 } } },
                .dst_slice = { .mip_level = 0 },
                .dst_offsets = { { { 0, 0, 0 },{ static_cast<daxa::i32>(window.width), static_cast<daxa::i32>(window.height), 1 } } },
                .filter = daxa::Filter::NEAREST,
                });
            recorder.pipeline_image_barrier({
                .src_access = daxa::AccessConsts::TRANSFER_WRITE,
                .image = swap_image,
                .layout_operation = daxa::ImageLayoutOperation::TO_PRESENT_SRC,
                });
            device.submit_commands({
                .command_lists = std::array{ recorder.complete_current_commands() },
                .wait_binary_semaphores = std::array{ swapchain.current_acquire_semaphore() },
                .signal_binary_semaphores = std::array{ swapchain.current_present_semaphore() },
                .signal_timeline_semaphores = std::array{ swapchain.current_timeline_pair() },
                });
            device.present_frame({
                .wait_binary_semaphores = std::array{ swapchain.current_present_semaphore() },
                .swapchain = swapchain,
                });
            device.collect_garbage();

            // ---- stats ------------------------------------------------------------
            double total_ms = ms_since(frame_start);
            stats.ema(stats.frame_ms, total_ms);
            stats.history[size_t(stats.hist_pos)] = float(total_ms);
            stats.hist_pos = (stats.hist_pos + 1) % int(stats.history.size());
            ++frame_counter;
            title_accum += dt;
            ++title_frames;
            if (title_accum >= 0.5)
            {
                stats.fps = int(double(title_frames) / title_accum);
                title_accum = 0.0;
                title_frames = 0;
                std::string title = "Voita + Muli | " + std::to_string(stats.fps) + " FPS | " +
                    TOOL_NAMES[int(tool)] + " " + MAT_NAMES[int(material)] + " r" + std::to_string(brush_radius) +
                    (paused ? " | PAUSED" : "") + " | F1: help";
                glfwSetWindowTitle(glfw, title.c_str());
            }
        }

        device.wait_idle();
        for (auto b : staging) device.destroy_buffer(b);
        for (auto img : { scene, post, bright, blur_a, blur_b, output }) device.destroy_image(img);
        return 0;
    }
    catch (std::exception const& e)
    {
        std::cerr << "Fatal error: " << e.what() << '\n';
        return 1;
    }
}
