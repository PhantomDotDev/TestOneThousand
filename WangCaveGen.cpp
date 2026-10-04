#include "WangCaveGen.h"
#include "WorldCache.h"
#include "Constants.h"

#include <../stb/stb_herringbone_wang_tile.h>

#ifndef STBI_INCLUDE_STB_IMAGE_H
#include <../stb_image.h>
#endif

#include "Material/Pixel.h"

#include <algorithm>
#include <utility>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace {

	constexpr int kNeighborCount = 8;
	constexpr int kCardinalCount = 4;

	struct CavePoint {
		int x = 0;
		int y = 0;
	};

	struct CaveRect {
		int x0 = 0;
		int y0 = 0;
		int x1 = 0;
		int y1 = 0;
	};

	struct CaveBiomeWeights {
		float regular = 0.0f;
		float lush = 0.0f;
		float mines = 0.0f;
		float ice = 0.0f;
		float temple = 0.0f;
	};

	struct CaveCellInfo {
		bool inBounds = false;
		bool open = false;
		bool solid = false;
		bool floor = false;
		bool ceiling = false;
		bool leftWall = false;
		bool rightWall = false;
		bool corner = false;
		bool exposed = false;
		int openNeighbors = 0;
		int solidNeighbors = 0;
		int floorSupport = 0;
		int ceilingSupport = 0;
	};

	struct BiomeStamp {
		CaveBiome biome = CaveBiome::REGULAR;
		int x = 0;
		int y = 0;
		int radius = 0;
	};

	struct FeatureCandidate {
		CavePoint p{};
		int length = 0;
		int score = 0;
		bool horizontal = false;
		bool downward = false;
	};

	stbhw_tileset gTileset{};
	bool gTilesetLoaded = false;

	static CaveGenerationSettings gLastSettings{};
	static bool gHaveLastSettings = false;

	// -----------------------------------------------------------------------------
	// Basic integer/hash utilities
	// -----------------------------------------------------------------------------

	static std::uint32_t mix32(std::uint32_t x) {
		x ^= x >> 16;
		x *= 0x7feb352du;
		x ^= x >> 15;
		x *= 0x846ca68bu;
		x ^= x >> 16;
		return x;
	}

	// -----------------------------------------------------------------------------
	// Streaming world origin
	//
	// The sim grid (gMap) is only a window onto a much larger procedural world.
	// gOriginX/Y is the absolute world cell that grid cell (0,0) represents. All
	// generation hashes/noise are keyed on ABSOLUTE coordinates, so regenerating
	// the window at a different origin yields a consistent, seamless world.
	// The world wraps (torus) every gMasterDim * wangScale cells.
	// -----------------------------------------------------------------------------
	static long long gOriginX = 0;
	static long long gOriginY = 0;
	static bool gHashUsesOrigin = true;

	struct NoOriginScope {
		bool prev;
		NoOriginScope() : prev(gHashUsesOrigin) { gHashUsesOrigin = false; }
		~NoOriginScope() { gHashUsesOrigin = prev; }
	};

	// Master coarse Wang mask for the whole (wrapping) world: 1 byte per Wang px.
	static int gMasterDim = 0; // one Wang image's size (largest stbhw accepts)
	// The world is a kMacroGrid x kMacroGrid mosaic of Wang images. Each macro cell
	// picks one of kVariants independently generated images (optionally mirrored),
	// and neighbours are cross-faded near their borders. The world therefore wraps
	// only every kMacroGrid * gMasterDim * wangScale cells instead of every
	// gMasterDim * wangScale.
	static constexpr int kMacroGrid = 16;
	static constexpr int kVariants = 4;
	static std::vector<unsigned char> gMaster;
	static int gMasterScale = 0;
	static std::uint32_t gMasterSeed = 0;

	static int worldCellsW() { return gMasterScale > 0 ? gMasterDim * kMacroGrid * gMasterScale : GRID_W; }
	static int worldCellsH() { return gMasterScale > 0 ? gMasterDim * kMacroGrid * gMasterScale : GRID_H; }

	static int wrapMod(long long v, int m) {
		long long r = v % m;
		if (r < 0)
			r += m;
		return static_cast<int>(r);
	}

	static std::uint32_t hash2D(int x, int y, std::uint32_t seed) {
		std::uint32_t ux = static_cast<std::uint32_t>(
			x + (gHashUsesOrigin ? static_cast<int>(gOriginX) : 0));
		std::uint32_t uy = static_cast<std::uint32_t>(
			y + (gHashUsesOrigin ? static_cast<int>(gOriginY) : 0));

		std::uint32_t h = seed;
		h ^= mix32(ux + 0x9e3779b9u);
		h ^= mix32(uy + 0x85ebca6bu + (h << 6) + (h >> 2));
		return mix32(h);
	}

	static std::uint32_t hash3D(int x, int y, int z, std::uint32_t seed) {
		std::uint32_t h = hash2D(x, y, seed);
		h ^= mix32(static_cast<std::uint32_t>(z) + 0x27d4eb2du);
		return mix32(h);
	}

	static float hash01(int x, int y, std::uint32_t seed) {
		return static_cast<float>(hash2D(x, y, seed) & 0x00ffffffu) / static_cast<float>(0x01000000u);
	}

	static float hashSigned(int x, int y, std::uint32_t seed) {
		return hash01(x, y, seed) * 2.0f - 1.0f;
	}

	static int randomRange(
		int minValue,
		int maxValue,
		int x,
		int y,
		std::uint32_t seed) {
		if (maxValue <= minValue)
			return minValue;

		std::uint32_t h = hash2D(x, y, seed);
		std::uint32_t span = static_cast<std::uint32_t>(
			maxValue - minValue + 1);

		return minValue + static_cast<int>(h % span);
	}

	static bool randomChance(
		int percent,
		int x,
		int y,
		std::uint32_t seed) {
		if (percent <= 0)
			return false;

		if (percent >= 100)
			return true;

		return static_cast<int>(hash2D(x, y, seed) % 100u) < percent;
	}

	static bool randomChanceSalted(
		int percent,
		int x,
		int y,
		std::uint32_t seed,
		std::uint32_t salt) {
		return randomChance(
			percent,
			x,
			y,
			seed ^ mix32(salt));
	}

	static int clampInt(int v, int lo, int hi) {
		return std::fmax(lo, std::fmin(v, hi));
	}

	static float clampFloat(float v, float lo, float hi) {
		return std::fmax(lo, std::fmin(v, hi));
	}

	static float smoothStep(float a, float b, float t) {
		t = clampFloat(t, 0.0f, 1.0f);
		t = t * t * (3.0f - 2.0f * t);
		return a + (b - a) * t;
	}

	static float lerpFloat(float a, float b, float t) {
		return a + (b - a) * t;
	}

	// -----------------------------------------------------------------------------
	// Low-frequency deterministic noise.
	//
	// This is deliberately hash-based instead of depending on another noise
	// library. Biome layout therefore remains deterministic and cheap.
	// -----------------------------------------------------------------------------

	static float valueNoise(
		float x,
		float y,
		std::uint32_t seed) {
		int x0 = static_cast<int>(std::floor(x));
		int y0 = static_cast<int>(std::floor(y));

		int x1 = x0 + 1;
		int y1 = y0 + 1;

		float tx = x - static_cast<float>(x0);
		float ty = y - static_cast<float>(y0);

		float a = hash01(x0, y0, seed);
		float b = hash01(x1, y0, seed);
		float c = hash01(x0, y1, seed);
		float d = hash01(x1, y1, seed);

		float sx = tx * tx * (3.0f - 2.0f * tx);
		float sy = ty * ty * (3.0f - 2.0f * ty);

		float top = lerpFloat(a, b, sx);
		float bottom = lerpFloat(c, d, sx);

		return lerpFloat(top, bottom, sy);
	}

	static float fractalNoise(
		float x,
		float y,
		std::uint32_t seed,
		int octaves,
		float lacunarity,
		float gain) {
		float amplitude = 1.0f;
		float frequency = 1.0f;
		float sum = 0.0f;
		float normalization = 0.0f;

		for (int i = 0; i < octaves; ++i) {
			sum += valueNoise(
					   x * frequency,
					   y * frequency,
					   seed + static_cast<std::uint32_t>(i) * 1013u) *
				   amplitude;

			normalization += amplitude;
			amplitude *= gain;
			frequency *= lacunarity;
		}

		if (normalization <= 0.0f)
			return 0.0f;

		return sum / normalization;
	}

	static float biomeWarpNoise(
		int x,
		int y,
		const CaveGenerationSettings& settings) {
		float scale = 1.0f /
					  static_cast<float>(std::fmax(16, settings.biomeCellSize));

		float n = fractalNoise(
			static_cast<float>(x) * scale,
			static_cast<float>(y) * scale,
			settings.seed ^ 0xB10A4E1u,
			3,
			2.0f,
			0.5f);

		return (n - 0.5f) * 2.0f * settings.biomeWarp;
	}

	// -----------------------------------------------------------------------------
	// Material helpers
	// -----------------------------------------------------------------------------

	static void markCellChanged(int x, int y) {
		if (x < 0 || y < 0 || x >= GRID_W || y >= GRID_H)
			return;

		gMap[y][x].moved = true;
	}

	static void setCellMaterial(
		int x,
		int y,
		Material material,
		bool force = false) {
		if (x < 0 || y < 0 || x >= GRID_W || y >= GRID_H)
			return;

		if (!force && gLastSettings.preserveExistingNonAir &&
			gMap[y][x].type != AIR)
			return;

		gMap[y][x].type = material;

		if (material == AIR) {
			gMap[y][x].color = gColors[AIR];
			gMap[y][x].mass = 0.0f;
		}
		else {
			applyMaterialTexture(gMap[y][x], material);
			gMap[y][x].mass = 0.0f;
		}

		gMap[y][x].moved = true;
	}

	static Material cellMaterial(int x, int y) {
		if (x < 0 || y < 0 || x >= GRID_W || y >= GRID_H)
			return STONE;

		return static_cast<Material>(gMap[y][x].type);
	}

	static bool isOpenCell(int x, int y) {
		if (!inBounds(x, y))
			return false;

		return gMap[y][x].type == AIR;
	}

	static bool isSolidCell(int x, int y) {
		if (!inBounds(x, y))
			return false;

		return gMap[y][x].type != AIR;
	}

	static bool isWaterCell(int x, int y) {
		return inBounds(x, y) && gMap[y][x].type == WATER;
	}

	static bool isWoodCell(int x, int y) {
		return inBounds(x, y) && gMap[y][x].type == WOOD;
	}

	static bool isVegetationCell(int x, int y) {
		if (!inBounds(x, y))
			return false;

		Material m = static_cast<Material>(gMap[y][x].type);

		return m == GRASS || m == MOSS;
	}

	static bool isStoneLikeInternal(Material m) {
		switch (m) {
		case STONE:
		case ICE:
		case OBSIDIAN:
		case BRICK:
		case SANDSTONE:
		case TEMPLEBRICK:
		case CYBERBRICK:
		case STEEL:
		case RUSTED_STEEL:
			return true;
		default:
			return false;
		}
	}

	static bool isVegetationMaterialInternal(Material m) {
		return m == GRASS || m == MOSS;
	}

	static void setMaterialForced(int x, int y, Material m) {
		setCellMaterial(x, y, m, true);
	}

	static void setMaterialIfOpen(int x, int y, Material m) {
		if (isOpenCell(x, y))
			setCellMaterial(x, y, m, true);
	}

	static void setMaterialIfSolid(
		int x,
		int y,
		Material m) {
		if (isSolidCell(x, y))
			setCellMaterial(x, y, m, true);
	}

	// -----------------------------------------------------------------------------
	// Wang loading
	// -----------------------------------------------------------------------------

	static bool loadTilesetInternal(const char* path) {
		if (gTilesetLoaded)
			freeCaveTileset();

		if (!path || !path[0])
			return false;

		int w = 0;
		int h = 0;
		int channels = 0;

		unsigned char* data = stbi_load(
			path,
			&w,
			&h,
			&channels,
			3);

		if (!data) {
			std::fprintf(
				stderr,
				"[WangCaveGen] Failed to load '%s': %s\n",
				path,
				stbi_failure_reason());

			return false;
		}

		int stride = w * 3;

		int ok = stbhw_build_tileset_from_image(
			&gTileset,
			data,
			stride,
			w,
			h);

		stbi_image_free(data);

		if (!ok) {
			const char* err = stbhw_get_last_error();

			std::fprintf(
				stderr,
				"[WangCaveGen] stbhw_build_tileset_from_image failed: %s\n",
				err ? err : "(unknown error)");

			gTilesetLoaded = false;
			return false;
		}

		gTilesetLoaded = true;

		std::fprintf(
			stdout,
			"[WangCaveGen] loaded Wang tileset '%s' (%dx%d)\n",
			path,
			w,
			h);

		return true;
	}

	static bool generateWangBuffer(
		int width,
		int height,
		std::vector<unsigned char>& buffer) {
		if (!gTilesetLoaded)
			return false;

		if (width <= 0 || height <= 0)
			return false;

		buffer.resize(
			static_cast<std::size_t>(width) *
			static_cast<std::size_t>(height) *
			3u);

		int stride = width * 3;

		int ok = stbhw_generate_image(
			&gTileset,
			nullptr,
			buffer.data(),
			stride,
			width,
			height);

		if (!ok) {
			const char* err = stbhw_get_last_error();

			std::fprintf(
				stderr,
				"[WangCaveGen] stbhw_generate_image failed: %s\n",
				err ? err : "(unknown error)");

			return false;
		}

		return true;
	}

	// -----------------------------------------------------------------------------
	// Wang color classification
	// -----------------------------------------------------------------------------

	static bool classifyColorInternal(
		unsigned char r,
		unsigned char g,
		unsigned char b) {
		int luminance =
			(static_cast<int>(r) * 3 +
				static_cast<int>(g) * 6 +
				static_cast<int>(b)) /
			10;

		// Tileset: white (255) = rock, teal (167,204,204 -> lum ~192) = cave.
		// The old threshold (40) classified BOTH as solid, so no air was ever
		// generated. Flip the comparison if the caves come out inverted.
		return luminance > 224;
	}

	static void writeWangBuffer(
		int startX,
		int startY,
		int width,
		int height,
		int scale,
		const std::vector<unsigned char>& buffer,
		Material stoneMaterial,
		Material airMaterial) {
		scale = std::fmax(1, scale);

		int generatedWidth = width;
		int generatedHeight = height;

		for (int cy = 0; cy < generatedHeight; ++cy) {
			for (int cx = 0; cx < generatedWidth; ++cx) {
				std::size_t index =
					(static_cast<std::size_t>(cy) *
							static_cast<std::size_t>(generatedWidth) +
						static_cast<std::size_t>(cx)) *
					3u;

				bool solid = classifyColorInternal(
					buffer[index + 0],
					buffer[index + 1],
					buffer[index + 2]);

				int gx0 = startX + cx * scale;
				int gy0 = startY + cy * scale;

				for (int oy = 0; oy < scale; ++oy) {
					for (int ox = 0; ox < scale; ++ox) {
						int gx = gx0 + ox;
						int gy = gy0 + oy;

						if (!inBounds(gx, gy))
							continue;

						if (gx >= startX + width ||
							gy >= startY + height)
							continue;

						Material m = solid
										 ? stoneMaterial
										 : airMaterial;

						setMaterialForced(gx, gy, m);
					}
				}
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Biome classification
	// -----------------------------------------------------------------------------
	//
	// The important design choice here is that biome selection happens at a much
	// lower frequency than individual decorations. That creates "big parts of the
	// world" instead of a checkerboard of one-cell biomes.
	//
	// The field uses both broad vertical bands and a warped low-frequency noise
	// value. The weights then choose the biome inside that broad field.
	// -----------------------------------------------------------------------------

	static CaveBiome chooseWeightedBiome(
		const CaveBiomeWeights& weights,
		int x,
		int y,
		std::uint32_t seed) {
		float total =
			weights.regular +
			weights.lush +
			weights.mines +
			weights.ice +
			weights.temple;

		if (total <= 0.0001f)
			return CaveBiome::REGULAR;

		float roll = hash01(
			x,
			y,
			seed ^ 0x51B10AEu);

		float cursor = 0.0f;

		cursor += weights.regular / total;
		if (roll < cursor)
			return CaveBiome::REGULAR;

		cursor += weights.lush / total;
		if (roll < cursor)
			return CaveBiome::LUSH_CAVES;

		cursor += weights.mines / total;
		if (roll < cursor)
			return CaveBiome::MINES;

		cursor += weights.ice / total;
		if (roll < cursor)
			return CaveBiome::ICE;

		return CaveBiome::TEMPLE;
	}

	static CaveBiomeWeights weightsForRegion(
		float macroX,
		float macroY,
		float warp,
		const CaveGenerationSettings& settings) {
		CaveBiomeWeights w{};

		// Start from the user-configured global weights.
		w.regular = settings.regularWeight;
		w.lush = settings.lushWeight;
		w.mines = settings.minesWeight;
		w.ice = settings.iceWeight;
		w.temple = settings.templeWeight;

		// Broad "biome continents".
		//
		// Left/top is mostly ordinary caves.
		// A broad central-lower area becomes lush.
		// The right side becomes ice.
		// Mine regions cluster around the middle.
		// Temple regions cluster deeper and toward one side.
		//
		// The weights are still blended so there are transition areas.
		float regularBias = 0.0f;
		float lushBias = 0.0f;
		float minesBias = 0.0f;
		float iceBias = 0.0f;
		float templeBias = 0.0f;

		float lushCenterX = 0.38f + warp * 0.18f;
		float lushCenterY = 0.48f + warp * 0.10f;

		float iceCenterX = 0.83f + warp * 0.08f;
		float iceCenterY = 0.47f + warp * 0.12f;

		float templeCenterX = 0.66f + warp * 0.10f;
		float templeCenterY = 0.82f + warp * 0.10f;

		float minesCenterX = 0.52f + warp * 0.12f;
		float minesCenterY = 0.30f + warp * 0.12f;

		auto gaussian = [](float x, float y, float cx, float cy, float radius) {
			float dx = x - cx;
			float dy = y - cy;
			float d2 = dx * dx + dy * dy;
			float r2 = radius * radius;

			if (r2 <= 0.00001f)
				return 0.0f;

			return std::exp(-d2 / r2);
		};

		lushBias =
			gaussian(macroX, macroY, lushCenterX, lushCenterY, 0.32f);

		iceBias =
			gaussian(macroX, macroY, iceCenterX, iceCenterY, 0.28f);

		templeBias =
			gaussian(macroX, macroY, templeCenterX, templeCenterY, 0.25f);

		minesBias =
			gaussian(macroX, macroY, minesCenterX, minesCenterY, 0.34f);

		regularBias =
			0.85f +
			(1.0f - macroY) * 0.55f;

		w.regular *= regularBias;
		w.lush *= 1.0f + lushBias * 6.0f;
		w.mines *= 1.0f + minesBias * 4.5f;
		w.ice *= 1.0f + iceBias * 7.0f;
		w.temple *= 1.0f + templeBias * 7.0f;

		// Make the lower world less surface-like.
		if (macroY > 0.72f) {
			w.regular *= 0.72f;
			w.temple *= 1.55f;
			w.lush *= 1.12f;
		}

		// Very top region remains predominantly regular caves.
		if (macroY < 0.20f) {
			w.regular *= 1.85f;
			w.temple *= 0.20f;
			w.ice *= 0.60f;
		}

		return w;
	}

	static CaveBiome caveBiomeCompute(
		int lx,
		int ly,
		const CaveGenerationSettings& settings) {
		// Biomes are large, irregular Voronoi-style regions. We deliberately do
		// NOT snap every pixel to a square biomeCellSize block; each coarse cell
		// owns one jittered seed point, and the nearest seed wins. This gives
		// large organic regions instead of visible checkerboard/pixel squares.
		NoOriginScope noOrigin;

		const int worldW = std::max(1, worldCellsW());
		const int worldH = std::max(1, worldCellsH());
		const int x = wrapMod(gOriginX + lx, worldW);
		const int y = wrapMod(gOriginY + ly, worldH);
		const int cellSize = std::max(256, settings.biomeCellSize);

		const int bx0 = x / cellSize;
		const int by0 = y / cellSize;

		float bestDist2 = std::numeric_limits<float>::max();
		int bestBX = bx0;
		int bestBY = by0;
		float bestCX = static_cast<float>(x);
		float bestCY = static_cast<float>(y);

		// Search the surrounding Voronoi cells. The center jitter is deliberately
		// kept inside each coarse cell so adjacent regions remain large and stable.
		for (int by = by0 - 1; by <= by0 + 1; ++by) {
			for (int bx = bx0 - 1; bx <= bx0 + 1; ++bx) {
				float jx = 0.22f + hash01(bx, by, settings.seed ^ 0xB10A11u) * 0.56f;
				float jy = 0.22f + hash01(bx, by, settings.seed ^ 0xB10A22u) * 0.56f;

				float cx = static_cast<float>(bx * cellSize) + jx * cellSize;
				float cy = static_cast<float>(by * cellSize) + jy * cellSize;

				// Periodic wrapping keeps the biome field seamless at world edges.
				float dx = cx - static_cast<float>(x);
				float dy = cy - static_cast<float>(y);
				if (dx > worldW * 0.5f)
					dx -= static_cast<float>(worldW);
				if (dx < -worldW * 0.5f)
					dx += static_cast<float>(worldW);
				if (dy > worldH * 0.5f)
					dy -= static_cast<float>(worldH);
				if (dy < -worldH * 0.5f)
					dy += static_cast<float>(worldH);

				float d2 = dx * dx + dy * dy;
				if (d2 < bestDist2) {
					bestDist2 = d2;
					bestBX = bx;
					bestBY = by;
					bestCX = static_cast<float>(x) + dx;
					bestCY = static_cast<float>(y) + dy;
				}
			}
		}

		const float macroX =
			wrapMod(static_cast<long long>(std::floor(bestCX)), worldW) /
			static_cast<float>(std::max(1, worldW - 1));

		const float macroY =
			wrapMod(static_cast<long long>(std::floor(bestCY)), worldH) /
			static_cast<float>(std::max(1, worldH - 1));

		// Warp the region center, not each individual pixel. This keeps biome
		// interiors coherent while making the biome layout less geometric.
		const float warp =
			hashSigned(bestBX, bestBY, settings.seed ^ 0xABCD1234u) *
			settings.biomeWarp;

		CaveBiomeWeights weights =
			weightsForRegion(
				macroX,
				macroY,
				warp,
				settings);

		return chooseWeightedBiome(
			weights,
			bestBX,
			bestBY,
			settings.seed ^ 0x7E11A11u);
	}

	// Per-window biome cache. Every decoration pass asks for the biome of every
	// cell, which used to recompute fractal noise ~15x per cell. The cache is keyed
	// to the window origin and shifted along with gMap when the window scrolls.
	static std::vector<unsigned char> gBiomeCache;
	static long long gCacheOX = 0, gCacheOY = 0;
	static std::uint32_t gCacheKey = 0;
	static constexpr unsigned char kBiomeUnknown = 255;

	static void resetBiomeCache(const CaveGenerationSettings& settings) {
		gBiomeCache.assign(static_cast<std::size_t>(GRID_W) * GRID_H, kBiomeUnknown);
		gCacheOX = gOriginX;
		gCacheOY = gOriginY;
		gCacheKey = settings.seed ^ (static_cast<std::uint32_t>(settings.biomeCellSize) * 2654435761u);
	}

	static CaveBiome caveBiomeAtInternal(
		int lx,
		int ly,
		const CaveGenerationSettings& settings) {
		if (!inBounds(lx, ly))
			return caveBiomeCompute(lx, ly, settings);

		std::uint32_t key = settings.seed ^ (static_cast<std::uint32_t>(settings.biomeCellSize) * 2654435761u);
		if (gBiomeCache.size() != static_cast<std::size_t>(GRID_W) * GRID_H ||
			gCacheOX != gOriginX || gCacheOY != gOriginY || gCacheKey != key)
			resetBiomeCache(settings);

		unsigned char& slot = gBiomeCache[static_cast<std::size_t>(ly) * GRID_W + lx];
		if (slot == kBiomeUnknown)
			slot = static_cast<unsigned char>(caveBiomeCompute(lx, ly, settings));
		return static_cast<CaveBiome>(slot);
	}

	// -----------------------------------------------------------------------------
	// Public biome helpers
	// -----------------------------------------------------------------------------

	// -----------------------------------------------------------------------------
	// Geometry sampling
	// -----------------------------------------------------------------------------

	static CaveCellInfo inspectCell(int x, int y) {
		CaveCellInfo info{};

		if (!inBounds(x, y))
			return info;

		info.inBounds = true;
		info.open = isOpenCell(x, y);
		info.solid = !info.open;

		static const int dx[kNeighborCount] = {
			-1, 0, 1,
			-1, 1,
			-1, 0, 1
		};

		static const int dy[kNeighborCount] = {
			-1, -1, -1,
			0, 0,
			1, 1, 1
		};

		for (int i = 0; i < kNeighborCount; ++i) {
			int nx = x + dx[i];
			int ny = y + dy[i];

			if (!inBounds(nx, ny))
				continue;

			if (isOpenCell(nx, ny))
				++info.openNeighbors;
			else
				++info.solidNeighbors;
		}

		bool belowSolid =
			isSolidCell(x, y + 1);

		bool aboveSolid =
			isSolidCell(x, y - 1);

		bool leftSolid =
			isSolidCell(x - 1, y);

		bool rightSolid =
			isSolidCell(x + 1, y);

		info.floor = info.open && belowSolid;
		info.ceiling = info.open && aboveSolid;
		info.leftWall = info.open && leftSolid;
		info.rightWall = info.open && rightSolid;

		info.corner =
			info.open &&
			((info.floor && info.leftWall) ||
				(info.floor && info.rightWall) ||
				(info.ceiling && info.leftWall) ||
				(info.ceiling && info.rightWall));

		info.exposed =
			info.open &&
			(info.floor ||
				info.ceiling ||
				info.leftWall ||
				info.rightWall);

		return info;
	}

	static bool isHorizontalOpenRun(
		int x,
		int y,
		int length) {
		if (length <= 0)
			return false;

		for (int i = 0; i < length; ++i) {
			if (!isOpenCell(x + i, y))
				return false;
		}

		return true;
	}

	static bool isHorizontalSolidRun(
		int x,
		int y,
		int length) {
		if (length <= 0)
			return false;

		for (int i = 0; i < length; ++i) {
			if (!isSolidCell(x + i, y))
				return false;
		}

		return true;
	}

	static int measureOpenRunRight(
		int x,
		int y,
		int maximum) {
		int length = 0;

		for (int i = 0; i < maximum; ++i) {
			if (!isOpenCell(x + i, y))
				break;

			++length;
		}

		return length;
	}

	static int measureOpenRunLeft(
		int x,
		int y,
		int maximum) {
		int length = 0;

		for (int i = 0; i < maximum; ++i) {
			if (!isOpenCell(x - i, y))
				break;

			++length;
		}

		return length;
	}

	static int measureSolidRunRight(
		int x,
		int y,
		int maximum) {
		int length = 0;

		for (int i = 0; i < maximum; ++i) {
			if (!isSolidCell(x + i, y))
				break;

			++length;
		}

		return length;
	}

	static int countOpenBelow(
		int x,
		int y,
		int depth) {
		int count = 0;

		for (int i = 1; i <= depth; ++i) {
			if (!isOpenCell(x, y + i))
				break;

			++count;
		}

		return count;
	}

	static int countOpenAbove(
		int x,
		int y,
		int depth) {
		int count = 0;

		for (int i = 1; i <= depth; ++i) {
			if (!isOpenCell(x, y - i))
				break;

			++count;
		}

		return count;
	}

	static bool hasSolidFloor(
		int x,
		int y,
		int depth) {
		for (int i = 1; i <= depth; ++i) {
			if (isSolidCell(x, y + i))
				return true;

			if (!isOpenCell(x, y + i))
				return false;
		}

		return false;
	}

	static bool hasSolidCeiling(
		int x,
		int y,
		int depth) {
		for (int i = 1; i <= depth; ++i) {
			if (isSolidCell(x, y - i))
				return true;

			if (!isOpenCell(x, y - i))
				return false;
		}

		return false;
	}

	// -----------------------------------------------------------------------------
	// Biome-specific rock/material conversion
	// -----------------------------------------------------------------------------

	static Material baseMaterialForBiome(
		CaveBiome biome,
		int x,
		int y,
		const CaveGenerationSettings& settings) {
		switch (biome) {
		case CaveBiome::REGULAR:
			return STONE;

		case CaveBiome::LUSH_CAVES:
			// Lush caves retain stone as their primary structural rock. Moss and
			// dirt are applied in a separate exposure pass.
			return MOSS;

		case CaveBiome::MINES: {
			return WOOD;

			return STONE;
		}

		case CaveBiome::ICE:
			if (randomChanceSalted(
					settings.iceReplaceStoneChance,
					x,
					y,
					settings.seed,
					0x1CEu))
				return ICE;

			return STONE;

		case CaveBiome::TEMPLE:
			if (randomChanceSalted(
					settings.templeMasonryChance,
					x,
					y,
					settings.seed,
					0x7E4B1Eu))
				return CAVE_TEMPLESTONE_MATERIAL;

			return STONE;

		default:
			return STONE;
		}
	}

	static void applyBiomeBaseMaterials(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(0, startX);
		int y0 = std::fmax(0, startY);

		int x1 = std::fmin(GRID_W, startX + width);
		int y1 = std::fmin(GRID_H, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (isOpenCell(x, y))
					continue;

				CaveBiome biome =
					caveBiomeAtInternal(x, y, settings);

				Material m =
					baseMaterialForBiome(
						biome,
						x,
						y,
						settings);

				if (m != cellMaterial(x, y))
					setMaterialForced(x, y, m);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Regular cave dressing
	// -----------------------------------------------------------------------------

	static void placeRegularStoneVariation(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		// Regular caves intentionally remain mostly stone. This pass does not
		// introduce exotic materials; it simply preserves the natural silhouette.
		//
		// Keeping this pass separate makes the "normal cave" biome a clean base
		// for future ore/material additions.
		(void)startX;
		(void)startY;
		(void)width;
		(void)height;
		(void)settings;
	}

	// -----------------------------------------------------------------------------
	// Grass generation
	// -----------------------------------------------------------------------------

	static bool canPlaceGrassAt(
		int x,
		int y) {
		if (!isOpenCell(x, y))
			return false;

		if (!isSolidCell(x, y + 1))
			return false;

		Material below = cellMaterial(x, y + 1);

		if (!(below == DIRT ||
				below == STONE ||
				below == MOSS ||
				below == TEMPLEBRICK))
			return false;

		return true;
	}

	static void growGrassPatch(
		int x,
		int y,
		int requestedLength,
		const CaveGenerationSettings& settings) {
		if (!canPlaceGrassAt(x, y))
			return;

		int length = clampInt(
			requestedLength,
			1,
			std::fmax(1, settings.lushGrassPatchLength));

		int left = length / 2;
		int right = length - left;

		for (int dx = -left; dx < right; ++dx) {
			int gx = x + dx;

			if (!canPlaceGrassAt(gx, y))
				continue;

			int chance = settings.lushGrassChance;

			// Patches taper at their ends.
			int edgeDistance = std::abs(dx);
			if (edgeDistance > length / 3)
				chance /= 2;

			if (randomChanceSalted(
					chance,
					gx,
					y,
					settings.seed,
					0x6A4555u))
				setMaterialIfOpen(gx, y, GRASS);
		}
	}

	static void generateLushGrass(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);

		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!canPlaceGrassAt(x, y))
					continue;

				CaveBiome biome =
					caveBiomeAtInternal(x, y, settings);

				if (biome != CaveBiome::LUSH_CAVES)
					continue;

				if (!randomChanceSalted(
						settings.lushGrassChance,
						x,
						y,
						settings.seed,
						0xA77Eu))
					continue;

				int patch = randomRange(
					3,
					std::fmax(
						3,
						settings.lushGrassPatchLength),
					x,
					y,
					settings.seed ^ 0x1234u);

				growGrassPatch(
					x,
					y,
					patch,
					settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Moss generation
	// -----------------------------------------------------------------------------

	static bool canPlaceMossOnWall(
		int x,
		int y) {
		if (!isOpenCell(x, y))
			return false;

		if (isSolidCell(x - 1, y) ||
			isSolidCell(x + 1, y) ||
			isSolidCell(x, y + 1) ||
			isSolidCell(x, y - 1))
			return true;

		return false;
	}

	static void generateLushMoss(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);

		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!canPlaceMossOnWall(x, y))
					continue;

				if (caveBiomeAtInternal(x, y, settings) !=
					CaveBiome::LUSH_CAVES)
					continue;

				int localChance =
					settings.lushMossChance;

				// Ceiling moss is slightly less dense than wall/floor moss.
				if (isSolidCell(x, y - 1) &&
					!isSolidCell(x, y + 1))
					localChance = localChance * 3 / 4;

				if (randomChanceSalted(
						localChance,
						x,
						y,
						settings.seed,
						0xA055u))
					setMaterialIfOpen(x, y, MOSS);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Vine generation
	// -----------------------------------------------------------------------------
	//
	// Vines are represented by vertical MOSS strands. A vine begins on a solid
	// ceiling and extends down through air. It stops when it reaches a floor,
	// another solid object, the configured maximum length, or a biome boundary.
	// This makes them look like hanging plants rather than random green pixels.
	// -----------------------------------------------------------------------------

	static bool isLushBiome(
		int x,
		int y,
		const CaveGenerationSettings& settings) {
		return caveBiomeAtInternal(x, y, settings) ==
			   CaveBiome::LUSH_CAVES;
	}

	static int measureVineLength(
		int x,
		int y,
		int maximum,
		const CaveGenerationSettings& settings) {
		int length = 0;

		for (int dy = 1; dy <= maximum; ++dy) {
			int gy = y + dy;

			if (!inBounds(x, gy))
				break;

			if (!isOpenCell(x, gy))
				break;

			if (!settings.softenBiomeEdges &&
				!isLushBiome(x, gy, settings))
				break;

			++length;
		}

		return length;
	}

	static void placeVine(
		int x,
		int y,
		int length,
		const CaveGenerationSettings& settings) {
		length = clampInt(
			length,
			1,
			std::fmax(1, settings.maximumVineLength));

		for (int i = 1; i <= length; ++i) {
			int gy = y + i;

			if (!inBounds(x, gy))
				break;

			if (!isOpenCell(x, gy))
				break;

			if (!settings.softenBiomeEdges &&
				!isLushBiome(x, gy, settings))
				break;

			// A small probability makes vines tapered instead of perfectly
			// uniform columns.
			int chance = 92;

			if (i > length * 2 / 3)
				chance = 72;

			if (randomChanceSalted(
					chance,
					x,
					gy,
					settings.seed,
					0xB1AEu))
				setMaterialIfOpen(
					x,
					gy,
					CAVE_VINE_MATERIAL);
		}
	}

	static void generateLushVines(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!isOpenCell(x, y))
					continue;

				if (!isSolidCell(x, y - 1))
					continue;

				if (!isLushBiome(x, y, settings))
					continue;

				if (!randomChanceSalted(
						settings.lushVineChance,
						x,
						y,
						settings.seed,
						0x91AEu))
					continue;

				int maxLength = clampInt(
					settings.maximumVineLength,
					1,
					std::fmax(1, settings.maximumVineLength));

				int length = measureVineLength(
					x,
					y,
					maxLength,
					settings);

				if (length < 2)
					continue;

				length = randomRange(
					2,
					length,
					x,
					y,
					settings.seed ^ 0xAC1Du);

				placeVine(
					x,
					y,
					length,
					settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Water pools
	// -----------------------------------------------------------------------------

	static bool isPoolCandidate(
		int x,
		int y) {
		if (!isOpenCell(x, y))
			return false;

		if (!isSolidCell(x, y + 1))
			return false;

		if (isOpenCell(x - 1, y) &&
			isOpenCell(x + 1, y))
			return false;

		return true;
	}

	static int poolWidth(
		int x,
		int y,
		int maximum) {
		int left = 0;
		int right = 0;

		for (int dx = 1; dx <= maximum; ++dx) {
			if (!isOpenCell(x - dx, y))
				break;

			if (!isSolidCell(x - dx, y + 1))
				break;

			++left;
		}

		for (int dx = 1; dx <= maximum; ++dx) {
			if (!isOpenCell(x + dx, y))
				break;

			if (!isSolidCell(x + dx, y + 1))
				break;

			++right;
		}

		return left + 1 + right;
	}

	static void fillWaterPool(
		int x,
		int y,
		int halfWidth,
		int depth,
		const CaveGenerationSettings& settings) {
		int width = halfWidth * 2 + 1;

		if (width <= 0)
			return;

		depth = clampInt(
			depth,
			1,
			std::fmax(1, settings.maximumWaterDepth));

		for (int dy = 0; dy < depth; ++dy) {
			float t =
				static_cast<float>(dy) /
				static_cast<float>(std::fmax(1, depth - 1));

			int rowHalfWidth =
				static_cast<int>(
					std::round(
						static_cast<float>(halfWidth) *
						(1.0f - t * 0.35f)));

			for (int dx = -rowHalfWidth;
				dx <= rowHalfWidth;
				++dx) {
				int gx = x + dx;
				int gy = y + dy;

				if (!inBounds(gx, gy))
					continue;

				if (!isOpenCell(gx, gy))
					continue;

				// Never turn the solid floor itself into water.
				if (!isOpenCell(gx, gy + 1) &&
					dy + 1 < depth)
					continue;

				setMaterialIfOpen(
					gx,
					gy,
					WATER);
			}
		}
	}

	static void generateLushWater(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 2, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!isPoolCandidate(x, y))
					continue;

				if (!isLushBiome(x, y, settings))
					continue;

				if (!randomChanceSalted(
						settings.lushWaterChance,
						x,
						y,
						settings.seed,
						0xA7E2u))
					continue;

				int maximumWidth =
					std::fmax(2, settings.maximumPlatformLength);

				int totalWidth =
					poolWidth(x, y, maximumWidth);

				if (totalWidth < 3)
					continue;

				int halfWidth =
					std::fmin(
						totalWidth / 2,
						randomRange(
							2,
							std::fmax(
								2,
								totalWidth / 2),
							x,
							y,
							settings.seed ^ 0x90u));

				int depth =
					randomRange(
						1,
						std::fmax(
							1,
							settings.maximumWaterDepth),
						x,
						y,
						settings.seed ^ 0x44u);

				fillWaterPool(
					x,
					y,
					halfWidth,
					depth,
					settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Mine platform detection
	// -----------------------------------------------------------------------------
	//
	// The important rule requested for the cave system is:
	//
	//   If a generated stone edge/corner borders open space, place WOOD along
	//   selected horizontal ledges.
	//
	// We do that explicitly here. A platform is anchored to actual stone. It is
	// not simply a random floating line.
	// -----------------------------------------------------------------------------

	static bool mineBiomeAt(
		int x,
		int y,
		const CaveGenerationSettings& settings) {
		return caveBiomeAtInternal(x, y, settings) ==
			   CaveBiome::MINES;
	}

	static bool isStoneEdgeCorner(
		int x,
		int y) {
		if (!isSolidCell(x, y))
			return false;

		bool openAbove =
			isOpenCell(x, y - 1);

		bool openBelow =
			isOpenCell(x, y + 1);

		bool openLeft =
			isOpenCell(x - 1, y);

		bool openRight =
			isOpenCell(x + 1, y);

		bool cornerA =
			openAbove && openLeft;

		bool cornerB =
			openAbove && openRight;

		bool cornerC =
			openBelow && openLeft;

		bool cornerD =
			openBelow && openRight;

		return cornerA ||
			   cornerB ||
			   cornerC ||
			   cornerD;
	}

	static bool isHorizontalLedgeAnchor(
		int x,
		int y) {
		if (!isSolidCell(x, y))
			return false;

		if (!isOpenCell(x, y - 1))
			return false;

		// A platform should have structural rock immediately below it.
		if (!isSolidCell(x, y + 1))
			return false;

		return true;
	}

	static int findLedgeLength(
		int x,
		int y,
		int maxLength) {
		int left = 0;
		int right = 0;

		for (int dx = 1; dx <= maxLength; ++dx) {
			if (!isHorizontalLedgeAnchor(x - dx, y))
				break;

			++left;
		}

		for (int dx = 1; dx <= maxLength; ++dx) {
			if (!isHorizontalLedgeAnchor(x + dx, y))
				break;

			++right;
		}

		return left + right + 1;
	}

	static void placeWoodPlatform(
		int x,
		int y,
		int halfWidth,
		const CaveGenerationSettings& settings) {
		int start = x - halfWidth;
		int end = x + halfWidth;

		for (int gx = start; gx <= end; ++gx) {
			if (!inBounds(gx, y))
				continue;

			// The platform occupies open space one cell above its stone ledge.
			int gy = y - 1;

			if (!isOpenCell(gx, gy))
				continue;

			if (!isSolidCell(gx, y))
				continue;

			setMaterialIfOpen(
				gx,
				gy,
				WOOD);
		}

		// Add slightly thicker end supports. This creates a balcony-like edge
		// without requiring a separate wood material.
		if (settings.mineSupportChance > 0) {
			if (randomChanceSalted(
					settings.mineSupportChance,
					start,
					y,
					settings.seed,
					0x5A90u)) {
				int supportY = y - 2;

				if (isOpenCell(start, supportY))
					setMaterialIfOpen(
						start,
						supportY,
						WOOD);
			}

			if (randomChanceSalted(
					settings.mineSupportChance,
					end,
					y,
					settings.seed,
					0x5A91u)) {
				int supportY = y - 2;

				if (isOpenCell(end, supportY))
					setMaterialIfOpen(
						end,
						supportY,
						WOOD);
			}
		}
	}

	static void placeWoodPlatformFromCorner(
		int x,
		int y,
		const CaveGenerationSettings& settings) {
		if (!isStoneEdgeCorner(x, y))
			return;

		if (!mineBiomeAt(x, y, settings))
			return;

		if (!randomChanceSalted(
				settings.minePlatformChance,
				x,
				y,
				settings.seed,
				0xED6Eu))
			return;

		int maximum =
			std::fmax(
				settings.minimumPlatformLength,
				settings.maximumPlatformLength);

		int length =
			findLedgeLength(
				x,
				y,
				maximum);

		if (length < settings.minimumPlatformLength)
			return;

		int desired =
			randomRange(
				settings.minimumPlatformLength,
				std::fmin(
					length,
					settings.maximumPlatformLength),
				x,
				y,
				settings.seed ^ 0xBA11u);

		placeWoodPlatform(
			x,
			y,
			desired / 2,
			settings);
	}

	static void generateMinePlatforms(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				placeWoodPlatformFromCorner(
					x,
					y,
					settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Mine balconies
	// -----------------------------------------------------------------------------

	static bool canPlaceBalcony(
		int x,
		int y,
		int width) {
		if (width < 3)
			return false;

		for (int dx = 0; dx < width; ++dx) {
			int gx = x + dx;

			if (!isOpenCell(gx, y))
				return false;

			if (!isSolidCell(gx, y + 1))
				return false;
		}

		return true;
	}

	static void placeMineBalcony(
		int x,
		int y,
		int width,
		const CaveGenerationSettings& settings) {
		if (!canPlaceBalcony(x, y, width))
			return;

		for (int dx = 0; dx < width; ++dx) {
			int gx = x + dx;

			setMaterialIfOpen(
				gx,
				y,
				WOOD);
		}

		// Side braces make balconies visually different from simple platforms.
		int braceLeftX = x;
		int braceRightX = x + width - 1;

		for (int dy = 1; dy <= 3; ++dy) {
			int gy = y + dy;

			if (isOpenCell(braceLeftX, gy))
				setMaterialIfOpen(
					braceLeftX,
					gy,
					WOOD);

			if (isOpenCell(braceRightX, gy))
				setMaterialIfOpen(
					braceRightX,
					gy,
					WOOD);
		}

		(void)settings;
	}

	static void generateMineBalconies(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 4, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!mineBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y))
					continue;

				if (!isSolidCell(x, y + 1))
					continue;

				if (!randomChanceSalted(
						settings.mineBalconyChance,
						x,
						y,
						settings.seed,
						0xBA1Cu))
					continue;

				int widthCandidate =
					randomRange(
						settings.mineBalconyWidthMin,
						std::fmax(
							settings.mineBalconyWidthMin,
							settings.mineBalconyWidthMax),
						x,
						y,
						settings.seed ^ 0xBA1Du);

				widthCandidate =
					std::fmin(
						widthCandidate,
						x1 - x);

				if (widthCandidate < 3)
					continue;

				placeMineBalcony(
					x,
					y,
					widthCandidate,
					settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Mine support beams
	// -----------------------------------------------------------------------------

	static bool canPlaceVerticalSupport(
		int x,
		int y,
		int length) {
		if (length <= 0)
			return false;

		for (int dy = 0; dy < length; ++dy) {
			if (!isOpenCell(x, y + dy))
				return false;
		}

		return true;
	}

	static void placeMineSupport(
		int x,
		int y,
		int length) {
		if (!canPlaceVerticalSupport(
				x,
				y,
				length))
			return;

		for (int dy = 0; dy < length; ++dy) {
			setMaterialIfOpen(
				x,
				y + dy,
				WOOD);
		}
	}

	static void generateMineSupports(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 4, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!mineBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y))
					continue;

				if (!isSolidCell(x, y - 1))
					continue;

				if (!isSolidCell(x, y + 4))
					continue;

				if (!randomChanceSalted(
						settings.mineSupportChance,
						x,
						y,
						settings.seed,
						0x5A920u))
					continue;

				int length =
					randomRange(
						3,
						8,
						x,
						y,
						settings.seed ^ 0x5A921u);

				length =
					std::fmin(
						length,
						y1 - y);

				placeMineSupport(
					x,
					y,
					length);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Ice cave material pass
	// -----------------------------------------------------------------------------

	static bool iceBiomeAt(
		int x,
		int y,
		const CaveGenerationSettings& settings) {
		return caveBiomeAtInternal(x, y, settings) ==
			   CaveBiome::ICE;
	}

	static void generateIceSnow(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 2, startX + width);
		int y1 = std::fmin(GRID_H - 2, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!iceBiomeAt(x, y, settings))
					continue;

				// A snow pile sits ON a flat open floor, rather than replacing
				// arbitrary exposed ice/stone pixels.
				if (!isOpenCell(x, y) || !isSolidCell(x, y + 1))
					continue;

				Material below = cellMaterial(x, y + 1);
				if (!(below == ICE || below == STONE || below == SNOW))
					continue;

				if (!randomChanceSalted(
						settings.iceSnowChance,
						x,
						y,
						settings.seed,
						0x5A0Fu))
					continue;

				int pileWidth = randomRange(
					2,
					std::max(2, settings.iceSnowPileWidth),
					x,
					y,
					settings.seed ^ 0x5A10u);

				int pileHeight = randomRange(
					1,
					std::max(1, settings.iceSnowPileHeight),
					x,
					y,
					settings.seed ^ 0x5A11u);

				int left = pileWidth / 2;
				for (int dx = -left; dx < pileWidth - left; ++dx) {
					int gx = x + dx;
					for (int h = 0; h < pileHeight; ++h) {
						int gy = y - h;
						if (!inBounds(gx, gy) || !isOpenCell(gx, gy))
							break;

						// Rounded/tapered pile profile.
						int edge = std::abs(dx);
						int maxH = std::max(1, pileHeight - edge / 2);
						if (h >= maxH)
							break;

						if (randomChanceSalted(
								86,
								gx,
								gy,
								settings.seed,
								0x5A12u))
							setMaterialIfOpen(gx, gy, SNOW);
					}
				}
			}
		}
	}

	static void generateMineGunpowderPiles(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 2, startX + width);
		int y1 = std::fmin(GRID_H - 2, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!mineBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y) || !isSolidCell(x, y + 1))
					continue;

				if (!randomChanceSalted(
						settings.mineGunpowderPileChance,
						x,
						y,
						settings.seed,
						0xC011u))
					continue;

				int pileWidth = randomRange(
					2,
					std::max(2, settings.mineGunpowderPileWidth),
					x,
					y,
					settings.seed ^ 0xC012u);

				int pileHeight = randomRange(
					1,
					std::max(1, settings.mineGunpowderPileHeight),
					x,
					y,
					settings.seed ^ 0xC013u);

				int left = pileWidth / 2;
				for (int dx = -left; dx < pileWidth - left; ++dx) {
					int gx = x + dx;
					for (int h = 0; h < pileHeight; ++h) {
						int gy = y - h;
						if (!inBounds(gx, gy) || !isOpenCell(gx, gy))
							break;

						int edge = std::abs(dx);
						int maxH = std::max(1, pileHeight - edge / 2);
						if (h >= maxH)
							break;

						if (randomChanceSalted(
								88,
								gx,
								gy,
								settings.seed,
								0xC014u))
							setMaterialIfOpen(gx, gy, GUNPOWDER);
					}
				}
			}
		}
	}

	static void generateIceWater(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 2, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!iceBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y))
					continue;

				if (!isSolidCell(x, y + 1))
					continue;

				if (!randomChanceSalted(
						settings.iceWaterChance,
						x,
						y,
						settings.seed,
						0x1CEAA7u))
					continue;

				int widthCandidate =
					randomRange(
						2,
						8,
						x,
						y,
						settings.seed ^ 0x1CE01u);

				for (int dx = -widthCandidate;
					dx <= widthCandidate;
					++dx) {
					int gx = x + dx;

					if (!isOpenCell(gx, y))
						continue;

					if (!iceBiomeAt(gx, y, settings))
						continue;

					setMaterialIfOpen(
						gx,
						y,
						WATER);
				}
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Ice shelves
	// -----------------------------------------------------------------------------

	static bool iceShelfAnchor(
		int x,
		int y) {
		if (!isSolidCell(x, y))
			return false;

		return isOpenCell(x, y - 1) &&
			   isSolidCell(x, y + 1);
	}

	static void placeIceShelf(
		int x,
		int y,
		int length,
		const CaveGenerationSettings& settings) {
		for (int dx = 0; dx < length; ++dx) {
			int gx = x + dx;

			if (!iceBiomeAt(gx, y, settings))
				continue;

			if (!iceShelfAnchor(gx, y))
				continue;

			int gy = y - 1;

			if (isOpenCell(gx, gy))
				setMaterialIfOpen(
					gx,
					gy,
					ICE);
		}
	}

	static void generateIceShelves(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 2, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!iceShelfAnchor(x, y))
					continue;

				if (!iceBiomeAt(x, y, settings))
					continue;

				if (!randomChanceSalted(
						settings.iceShelfChance,
						x,
						y,
						settings.seed,
						0x5AE1Fu))
					continue;

				int length =
					randomRange(
						3,
						14,
						x,
						y,
						settings.seed ^ 0x5AE20u);

				placeIceShelf(
					x,
					y,
					length,
					settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Ice stalactites / stalagmites
	// -----------------------------------------------------------------------------

	static int availableVerticalAir(
		int x,
		int y,
		int direction,
		int maximum) {
		int count = 0;

		for (int i = 1; i <= maximum; ++i) {
			int gy = y + direction * i;

			if (!inBounds(x, gy))
				break;

			if (!isOpenCell(x, gy))
				break;

			++count;
		}

		return count;
	}

	static void placeIceColumn(
		int x,
		int y,
		int direction,
		int length) {
		for (int i = 1; i <= length; ++i) {
			int gy = y + direction * i;

			if (!inBounds(x, gy))
				break;

			if (!isOpenCell(x, gy))
				break;

			setMaterialIfOpen(
				x,
				gy,
				ICE);
		}
	}

	static void generateIceColumns(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!iceBiomeAt(x, y, settings))
					continue;

				if (isSolidCell(x, y) &&
					isOpenCell(x, y + 1) &&
					randomChanceSalted(
						settings.iceStalactiteChance,
						x,
						y,
						settings.seed,
						0x57A1u)) {

					int available =
						availableVerticalAir(
							x,
							y,
							+1,
							16);

					if (available >= 2) {
						int length =
							randomRange(
								2,
								available,
								x,
								y,
								settings.seed ^ 0x57A2u);

						placeIceColumn(
							x,
							y,
							+1,
							length);
					}
				}

				if (isSolidCell(x, y) &&
					isOpenCell(x, y - 1) &&
					randomChanceSalted(
						settings.iceStalagmiteChance,
						x,
						y,
						settings.seed,
						0x57A3u)) {

					int available =
						availableVerticalAir(
							x,
							y,
							-1,
							16);

					if (available >= 2) {
						int length =
							randomRange(
								2,
								available,
								x,
								y,
								settings.seed ^ 0x57A4u);

						placeIceColumn(
							x,
							y,
							-1,
							length);
					}
				}
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Temple generation
	// -----------------------------------------------------------------------------
	//
	// Temple architecture is intentionally sparse. The cave silhouette remains
	// the primary shape; temple masonry is stamped only where open rooms provide
	// enough space.
	//
	// TEMPLEBRICK is used as the requested temple-stone material.
	// -----------------------------------------------------------------------------

	static bool templeBiomeAt(
		int x,
		int y,
		const CaveGenerationSettings& settings) {
		return caveBiomeAtInternal(x, y, settings) ==
			   CaveBiome::TEMPLE;
	}

	static bool openRoomCandidate(
		int x,
		int y,
		int width,
		int height) {
		if (width < 3 || height < 3)
			return false;

		for (int dy = 0; dy < height; ++dy) {
			for (int dx = 0; dx < width; ++dx) {
				if (!isOpenCell(
						x + dx,
						y + dy))
					return false;
			}
		}

		return true;
	}

	static void stampTempleRoom(
		int x,
		int y,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		if (!openRoomCandidate(
				x,
				y,
				width,
				height))
			return;

		int left = x;
		int right = x + width - 1;
		int top = y;
		int bottom = y + height - 1;

		// Outer frame.
		for (int px = left; px <= right; ++px) {
			if (isOpenCell(px, top))
				setMaterialIfOpen(
					px,
					top,
					CAVE_TEMPLESTONE_MATERIAL);

			if (isOpenCell(px, bottom))
				setMaterialIfOpen(
					px,
					bottom,
					CAVE_TEMPLESTONE_MATERIAL);
		}

		for (int py = top; py <= bottom; ++py) {
			if (isOpenCell(left, py))
				setMaterialIfOpen(
					left,
					py,
					CAVE_TEMPLESTONE_MATERIAL);

			if (isOpenCell(right, py))
				setMaterialIfOpen(
					right,
					py,
					CAVE_TEMPLESTONE_MATERIAL);
		}

		// Central aisle / divider.
		int centerX = (left + right) / 2;

		for (int py = top + 1;
			py < bottom;
			++py) {
			if (randomChanceSalted(
					70,
					centerX,
					py,
					settings.seed,
					0xA15EEu)) {
				if (isOpenCell(centerX, py))
					setMaterialIfOpen(
						centerX,
						py,
						CAVE_TEMPLESTONE_MATERIAL);
			}
		}

		// Small wood altar/beam detail.
		if (randomChanceSalted(
				settings.templeWoodChance,
				x,
				y,
				settings.seed,
				0xA17A0u)) {

			int altarY = bottom - 1;
			int altarLeft = centerX - 2;
			int altarRight = centerX + 2;

			for (int px = altarLeft;
				px <= altarRight;
				++px) {
				if (isOpenCell(px, altarY))
					setMaterialIfOpen(
						px,
						altarY,
						WOOD);
			}
		}
	}

	static void generateTempleRooms(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; y += 6) {
			for (int x = x0; x < x1; x += 8) {
				if (!templeBiomeAt(x, y, settings))
					continue;

				if (!randomChanceSalted(
						settings.templeRoomChance,
						x,
						y,
						settings.seed,
						0xA00Du))
					continue;

				int roomWidth =
					randomRange(
						8,
						20,
						x,
						y,
						settings.seed ^ 0xA001u);

				int roomHeight =
					randomRange(
						6,
						14,
						x,
						y,
						settings.seed ^ 0xA002u);

				roomWidth =
					std::fmin(
						roomWidth,
						x1 - x);

				roomHeight =
					std::fmin(
						roomHeight,
						y1 - y);

				if (roomWidth < 5 ||
					roomHeight < 5)
					continue;

				stampTempleRoom(
					x,
					y,
					roomWidth,
					roomHeight,
					settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Temple pillars
	// -----------------------------------------------------------------------------

	static void placeTemplePillar(
		int x,
		int y,
		int height) {
		for (int dy = 0; dy < height; ++dy) {
			int gy = y + dy;

			if (!inBounds(x, gy))
				break;

			if (!isOpenCell(x, gy))
				continue;

			setMaterialIfOpen(
				x,
				gy,
				CAVE_TEMPLESTONE_MATERIAL);
		}
	}

	static void generateTemplePillars(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!templeBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y))
					continue;

				if (!isSolidCell(x, y - 1))
					continue;

				if (!isSolidCell(x, y + 5))
					continue;

				if (!randomChanceSalted(
						settings.templePillarChance,
						x,
						y,
						settings.seed,
						0xF111u))
					continue;

				int pillarHeight =
					randomRange(
						3,
						8,
						x,
						y,
						settings.seed ^ 0xF112u);

				pillarHeight =
					std::fmin(
						pillarHeight,
						y1 - y);

				placeTemplePillar(
					x,
					y,
					pillarHeight);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Temple moss and vines
	// -----------------------------------------------------------------------------

	static void generateTempleMoss(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!templeBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y))
					continue;

				bool touchesTemple =
					cellMaterial(x - 1, y) == TEMPLEBRICK ||
					cellMaterial(x + 1, y) == TEMPLEBRICK ||
					cellMaterial(x, y - 1) == TEMPLEBRICK ||
					cellMaterial(x, y + 1) == TEMPLEBRICK;

				if (!touchesTemple)
					continue;

				if (randomChanceSalted(
						settings.templeMossChance,
						x,
						y,
						settings.seed,
						0xA0557u))
					setMaterialIfOpen(
						x,
						y,
						MOSS);
			}
		}
	}

	static void generateTempleVines(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!templeBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y))
					continue;

				bool templeCeiling =
					cellMaterial(x, y - 1) ==
					TEMPLEBRICK;

				if (!templeCeiling)
					continue;

				if (!randomChanceSalted(
						settings.templeVineChance,
						x,
						y,
						settings.seed,
						0x7E1AEu))
					continue;

				int length =
					measureVineLength(
						x,
						y,
						std::fmin(
							settings.maximumVineLength,
							14),
						settings);

				if (length >= 2)
					placeVine(
						x,
						y,
						std::fmin(length, 14),
						settings);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Temple wood structures
	// -----------------------------------------------------------------------------

	static void generateTempleWood(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!templeBiomeAt(x, y, settings))
					continue;

				if (!isOpenCell(x, y))
					continue;

				if (!isSolidCell(x, y + 1))
					continue;

				if (!randomChanceSalted(
						settings.templeWoodChance,
						x,
						y,
						settings.seed,
						0x7E0Du))
					continue;

				int length =
					randomRange(
						4,
						12,
						x,
						y,
						settings.seed ^ 0x7E0Eu);

				for (int dx = 0; dx < length; ++dx) {
					int gx = x + dx;

					if (!isOpenCell(gx, y))
						break;

					if (!templeBiomeAt(
							gx,
							y,
							settings))
						break;

					setMaterialIfOpen(
						gx,
						y,
						WOOD);
				}
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Biome-specific dispatch
	// -----------------------------------------------------------------------------

	static void generateLushFeatures(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		generateLushGrass(
			startX,
			startY,
			width,
			height,
			settings);

		generateLushMoss(
			startX,
			startY,
			width,
			height,
			settings);

		generateLushWater(
			startX,
			startY,
			width,
			height,
			settings);

		generateLushVines(
			startX,
			startY,
			width,
			height,
			settings);
	}

	static void generateMineFeatures(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		generateMinePlatforms(
			startX,
			startY,
			width,
			height,
			settings);

		generateMineBalconies(
			startX,
			startY,
			width,
			height,
			settings);

		generateMineSupports(
			startX,
			startY,
			width,
			height,
			settings);

		generateMineGunpowderPiles(
			startX,
			startY,
			width,
			height,
			settings);
	}

	static void generateIceFeatures(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		generateIceSnow(
			startX,
			startY,
			width,
			height,
			settings);

		generateIceWater(
			startX,
			startY,
			width,
			height,
			settings);

		generateIceShelves(
			startX,
			startY,
			width,
			height,
			settings);

		generateIceColumns(
			startX,
			startY,
			width,
			height,
			settings);
	}

	static void generateTempleFeatures(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		generateTempleRooms(
			startX,
			startY,
			width,
			height,
			settings);

		generateTemplePillars(
			startX,
			startY,
			width,
			height,
			settings);

		generateTempleMoss(
			startX,
			startY,
			width,
			height,
			settings);

		generateTempleVines(
			startX,
			startY,
			width,
			height,
			settings);

		generateTempleWood(
			startX,
			startY,
			width,
			height,
			settings);
	}

	// -----------------------------------------------------------------------------
	// Feature cleanup
	// -----------------------------------------------------------------------------

	static void removeIsolatedVegetation(
		int startX,
		int startY,
		int width,
		int height) {
		int x0 = std::fmax(1, startX);
		int y0 = std::fmax(1, startY);
		int x1 = std::fmin(GRID_W - 1, startX + width);
		int y1 = std::fmin(GRID_H - 1, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!isVegetationCell(x, y))
					continue;

				bool supported =
					isSolidCell(x - 1, y) ||
					isSolidCell(x + 1, y) ||
					isSolidCell(x, y - 1) ||
					isSolidCell(x, y + 1) ||
					isVegetationCell(x - 1, y) ||
					isVegetationCell(x + 1, y) ||
					isVegetationCell(x, y - 1);

				if (!supported)
					setMaterialForced(
						x,
						y,
						AIR);
			}
		}
	}

	static void preventWoodInWater(
		int startX,
		int startY,
		int width,
		int height) {
		int x0 = std::fmax(0, startX);
		int y0 = std::fmax(0, startY);
		int x1 = std::fmin(GRID_W, startX + width);
		int y1 = std::fmin(GRID_H, startY + height);

		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!isWoodCell(x, y))
					continue;

				if (isWaterCell(x - 1, y) &&
					isWaterCell(x + 1, y) &&
					isWaterCell(x, y + 1)) {
					setMaterialForced(
						x,
						y,
						WATER);
				}
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Wood on stone surfaces
	// -----------------------------------------------------------------------------
	// Lays WOOD on the open cells directly above stone tops, following slopes
	// column by column. Segment selection is keyed on ABSOLUTE coordinates so
	// the result doesn't depend on window origin or strip boundaries.
	static long long floorDivAbs(long long a, int b) {
		long long q = a / b;
		if ((a % b != 0) && ((a < 0) != (b < 0)))
			--q;
		return q;
	}

	static void generateStoneSurfaceWood(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		if (settings.stoneWoodChance <= 0)
			return;

		const int seg = std::max(4, settings.stoneWoodSegment);
		const int headroom = std::max(2, settings.stoneWoodHeadroom);
		const int thickness = std::max(2, settings.stoneWoodMaxThickness);

		const int x0 = std::max(1, startX);
		const int y0 = std::max(1, startY);
		const int x1 = std::min(GRID_W - 2, startX + width);
		const int y1 = std::min(GRID_H - 2, startY + height);

		// Scan actual horizontal ledges. A chosen ledge gets ONE constant Y
		// coordinate, so the wood top is a genuinely flat platform instead of
		// a staircase following every tiny Wang bump.
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				if (!isHorizontalLedgeAnchor(x, y))
					continue;

				const long long ax = gOriginX + x;
				const long long ay = gOriginY + y;
				const int segX = static_cast<int>(floorDivAbs(ax, seg));
				const int band = static_cast<int>(floorDivAbs(ay, seg * 4));

				bool on = false;
				{
					NoOriginScope noOrigin;
					on = randomChanceSalted(
						settings.stoneWoodChance,
						segX,
						band,
						settings.seed,
						0x57D0u);
				}
				if (!on)
					continue;

				// Find a real contiguous ledge, then choose a platform width.
				const int available =
					std::min(
						settings.maximumPlatformLength,
						findLedgeLength(
							x,
							y,
							std::max(
								settings.minimumPlatformLength,
								settings.maximumPlatformLength)));

				if (available < std::max(3, settings.minimumPlatformLength))
					continue;

				int platformLength = randomRange(
					std::max(3, settings.minimumPlatformLength),
					available,
					x,
					y,
					settings.seed ^ 0x57D1u);

				int left = platformLength / 2;
				int right = platformLength - left;

				// Require enough headroom across the WHOLE platform. This is what
				// prevents the thick plank from becoming a random wall in a tunnel.
				bool roomy = true;
				for (int gx = x - left; gx < x + right; ++gx) {
					if (!isHorizontalLedgeAnchor(gx, y)) {
						roomy = false;
						break;
					}
					for (int k = 1; k <= headroom; ++k) {
						if (!isOpenCell(gx, y - k)) {
							roomy = false;
							break;
						}
					}
					if (!roomy)
						break;
				}
				if (!roomy)
					continue;

				for (int gx = x - left; gx < x + right; ++gx) {
					for (int t = 1; t <= thickness; ++t)
						setMaterialIfOpen(gx, y - t, WOOD);
				}

				// A couple of vertical end posts make the plank read as a thick
				// wooden structure rather than a thin one-cell line.
				const int postDepth = std::max(2, thickness + 1);
				for (int px : { x - left, x + right - 1 }) {
					for (int t = 1; t <= postDepth; ++t) {
						if (isOpenCell(px, y + t))
							setMaterialIfOpen(px, y + t, WOOD);
					}
				}

				// Skip the rest of this ledge so overlapping candidates do not
				// repeatedly build the same platform.
				x += std::max(0, platformLength - 1);
			}
		}
	}

	// -----------------------------------------------------------------------------
	// Flat overworld surface
	// -----------------------------------------------------------------------------

	static void generateFlatSurface(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		const int surfaceY = clampInt(
			settings.surfaceHeight,
			4,
			std::max(5, GRID_H - settings.surfaceThickness - 4));
		const int thickness = std::max(4, settings.surfaceThickness);

		const int x0 = std::max(0, startX);
		const int x1 = std::min(GRID_W, startX + width);
		const int y0 = std::max(0, startY);
		const int y1 = std::min(GRID_H, startY + height);

		if (surfaceY < y0 || surfaceY >= y1)
			return;

		// Remove any Wang caves in the spawn/surface band and make a perfectly
		// level, broad platform. Everything below this band remains procedural.
		const int bottom = std::min(GRID_H, surfaceY + thickness);
		for (int y = surfaceY; y < bottom; ++y) {
			for (int x = x0; x < x1; ++x) {
				Material m = (y == surfaceY) ? GRASS : STONE;
				setMaterialForced(x, y, m);
			}
		}

		// Guarantee a clean air column above the land so spawn-finding cannot
		// accidentally select an underground cave pocket.
		for (int y = y0; y < surfaceY; ++y) {
			for (int x = x0; x < x1; ++x)
				setMaterialForced(x, y, AIR);
		}
	}


	// -----------------------------------------------------------------------------
	// Main advanced decoration pipeline
	// -----------------------------------------------------------------------------

	static void decoratePass(
		int startX,
		int startY,
		int width,
		int height,
		const CaveGenerationSettings& settings) {
		applyBiomeBaseMaterials(
			startX,
			startY,
			width,
			height,
			settings);

		placeRegularStoneVariation(
			startX,
			startY,
			width,
			height,
			settings);

		generateLushFeatures(
			startX,
			startY,
			width,
			height,
			settings);

		generateMineFeatures(
			startX,
			startY,
			width,
			height,
			settings);

		generateIceFeatures(
			startX,
			startY,
			width,
			height,
			settings);

		generateTempleFeatures(
			startX,
			startY,
			width,
			height,
			settings);

		generateStoneSurfaceWood(
			startX,
			startY,
			width,
			height,
			settings);

		removeIsolatedVegetation(
			startX,
			startY,
			width,
			height);

		preventWoodInWater(
			startX,
			startY,
			width,
			height);
	}

	// -----------------------------------------------------------------------------
	// Wang generation at scale
	// -----------------------------------------------------------------------------

	static inline float wangHash01(int x, int y) {
		unsigned int h = static_cast<unsigned int>(x) * 374761393u +
						 static_cast<unsigned int>(y) * 668265263u;
		h = (h ^ (h >> 13)) * 1274126177u;
		h ^= (h >> 16);
		return static_cast<float>(h & 0xFFFFu) / 65535.0f;
	}

	// Smooth value noise, one lattice point every `cell` grid cells.
	static float wangValueNoise(int x, int y, int cell) {
		int ix = x / cell, iy = y / cell;
		float fx = static_cast<float>(x % cell) / cell;
		float fy = static_cast<float>(y % cell) / cell;
		fx = fx * fx * (3.0f - 2.0f * fx);
		fy = fy * fy * (3.0f - 2.0f * fy);
		float a = wangHash01(ix, iy), b = wangHash01(ix + 1, iy);
		float c = wangHash01(ix, iy + 1), d = wangHash01(ix + 1, iy + 1);
		return (a + (b - a) * fx) + ((c + (d - c) * fx) - (a + (b - a) * fx)) * fy;
	}

	// Build (once per seed/scale) the coarse Wang mask for the whole wrapping
	// world. 2048x2048 bytes = 4 MB; stbhw runs once, not per window.
	static bool ensureMaster(std::uint32_t seed, int scale) {
		if (!gMaster.empty() && gMasterSeed == seed && gMasterScale == scale)
			return true;

		// stb_herringbone has a compiled-in max image size, so find the biggest
		// single image it accepts (halving until it works), then make the rest of
		// the variants at that size.
		std::vector<unsigned char> rgb;
		int dim = 2048;
		bool ok = false;
		for (; dim >= 64; dim /= 2) {
			std::srand(seed);
			if (generateWangBuffer(dim, dim, rgb)) {
				ok = true;
				break;
			}
		}
		if (!ok) {
			std::fprintf(stderr, "[WangCaveGen] could not generate a master Wang image at any size.\n");
			return false;
		}

		const std::size_t plane = static_cast<std::size_t>(dim) * dim;
		std::vector<unsigned char> variants(plane * kVariants, 0);

		for (int v = 0; v < kVariants; ++v) {
			if (v > 0) {
				std::srand(seed + 7919u * static_cast<std::uint32_t>(v));
				if (!generateWangBuffer(dim, dim, rgb))
					return false;
			}
			for (std::size_t i = 0; i < plane; ++i) {
				variants[plane * v + i] = classifyColorInternal(
											  rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2])
											  ? 1
											  : 0;
			}
		}

		std::fprintf(stdout,
			"[WangCaveGen] %d Wang variants of %dx%d; world wraps every %d cells (%dx%d macro grid)\n",
			kVariants, dim, dim, dim * kMacroGrid * scale, kMacroGrid, kMacroGrid);

		gMasterDim = dim;
		gMaster = std::move(variants);
		gMasterSeed = seed;
		gMasterScale = scale;
		return true;
	}

	static inline long long floorDivLL(long long a, int b) {
		long long q = a / b;
		if ((a % b != 0) && ((a < 0) != (b < 0)))
			--q;
		return q;
	}

	// One macro cell's Wang image at local pixel (lx, ly), with per-cell variant
	// choice and mirroring.
	static inline float macroCell(int mx, int my, int lx, int ly) {
		mx = wrapMod(mx, kMacroGrid);
		my = wrapMod(my, kMacroGrid);
		std::uint32_t h = mix32(
			gMasterSeed ^ mix32(static_cast<std::uint32_t>(mx) * 73856093u ^
								static_cast<std::uint32_t>(my) * 19349663u));
		int variant = static_cast<int>(h % kVariants);
		if (h & 0x100u)
			lx = gMasterDim - 1 - lx;
		if (h & 0x200u)
			ly = gMasterDim - 1 - ly;
		std::size_t plane = static_cast<std::size_t>(gMasterDim) * gMasterDim;
		return gMaster[plane * variant + static_cast<std::size_t>(ly) * gMasterDim + lx] ? 1.0f : 0.0f;
	}

	static inline float bandWeight(int k, int band) {
		// 0.5 at the macro-cell border, rising to 1.0 at `band` pixels inside.
		float s = static_cast<float>(k) / static_cast<float>(band);
		s = s * s * (3.0f - 2.0f * s);
		return 0.5f + 0.5f * s;
	}

	// Horizontal cross-fade with the left/right neighbour (mirrored so the two
	// sides agree exactly on the border).
	static inline float macroRow(int mx, int my, int lx, int ly, int band) {
		float v = macroCell(mx, my, lx, ly);
		if (lx < band) {
			float t = bandWeight(lx, band);
			return t * v + (1.0f - t) * macroCell(mx - 1, my, gMasterDim - 1 - lx, ly);
		}
		if (lx >= gMasterDim - band) {
			int k = gMasterDim - 1 - lx;
			float t = bandWeight(k, band);
			return t * v + (1.0f - t) * macroCell(mx + 1, my, k, ly);
		}
		return v;
	}

	// Wang-pixel coordinate (wx, wy) anywhere in the (wrapping) mosaic.
	static inline float masterAt(long long wx, long long wy) {
		const int D = gMasterDim;
		int band = std::fmax(4, std::fmin(24, D / 10));

		int mx = static_cast<int>(floorDivLL(wx, D));
		int my = static_cast<int>(floorDivLL(wy, D));
		int lx = wrapMod(wx, D);
		int ly = wrapMod(wy, D);

		float v = macroRow(mx, my, lx, ly, band);
		if (ly < band) {
			float t = bandWeight(ly, band);
			return t * v + (1.0f - t) * macroRow(mx, my - 1, lx, D - 1 - ly, band);
		}
		if (ly >= D - band) {
			int k = D - 1 - ly;
			float t = bandWeight(k, band);
			return t * v + (1.0f - t) * macroRow(mx, my + 1, lx, k, band);
		}
		return v;
	}

	static bool generateScaledWang(
		int startX,
		int startY,
		int width,
		int height,
		int scale) {
		if (!gTilesetLoaded)
			return false;
		if (scale < 1)
			scale = 1;
		if (!ensureMaster(gLastSettings.seed, scale))
			return false;

		const int noiseCell = scale > 3 ? scale : 3;
		const int worldW = worldCellsW();
		const int worldH = worldCellsH();

		for (int oy = 0; oy < height; ++oy) {
			for (int ox = 0; ox < width; ++ox) {
				int gx = startX + ox;
				int gy = startY + oy;
				if (!inBounds(gx, gy))
					continue;

				// Absolute, wrapped world cell.
				int ax = wrapMod(gOriginX + gx, worldW);
				int ay = wrapMod(gOriginY + gy, worldH);

				// Bilinear sample of the master mask at absolute coords.
				float u = (static_cast<float>(ax) + 0.5f) / scale - 0.5f;
				float v = (static_cast<float>(ay) + 0.5f) / scale - 0.5f;
				long long x0 = static_cast<long long>(std::floor(u));
				long long y0 = static_cast<long long>(std::floor(v));
				float fx = u - static_cast<float>(x0);
				float fy = v - static_cast<float>(y0);

				float d =
					(masterAt(x0, y0) * (1 - fx) + masterAt(x0 + 1, y0) * fx) * (1 - fy) +
					(masterAt(x0, y0 + 1) * (1 - fx) + masterAt(x0 + 1, y0 + 1) * fx) * fy;

				float n = wangValueNoise(ax, ay, noiseCell) - 0.5f;
				float n2 = wangValueNoise(ax + 977, ay + 311,
							   noiseCell > 2 ? noiseCell / 2 : 1) -
						   0.5f;
				d += (n * 0.30f + n2 * 0.15f);

				setMaterialForced(gx, gy, d > 0.5f ? STONE : AIR);
			}
		}
		return true;
	}

	// -----------------------------------------------------------------------------
	// Public API
	// -----------------------------------------------------------------------------

} // anonymous namespace

const char* caveBiomeName(CaveBiome biome) {
	switch (biome) {
	case CaveBiome::REGULAR:
		return "Regular Caves";

	case CaveBiome::LUSH_CAVES:
		return "Lush Caves";

	case CaveBiome::MINES:
		return "Mines";

	case CaveBiome::ICE:
		return "Ice";

	case CaveBiome::TEMPLE:
		return "Temple";

	default:
		return "Unknown";
	}
}


bool caveTilesetReady() {
	return gTilesetLoaded;
}

bool loadCaveTileset(const char* path) {
	return loadTilesetInternal(path);
}

void freeCaveTileset() {
	if (!gTilesetLoaded)
		return;

	stbhw_free_tileset(
		&gTileset);

	std::memset(
		&gTileset,
		0,
		sizeof(gTileset));

	gTilesetLoaded = false;
}

bool classifyWangColorIsSolid(
	unsigned char r,
	unsigned char g,
	unsigned char b) {
	return classifyColorInternal(
		r,
		g,
		b);
}

bool generateCaveMaterials(
	int startX,
	int startY,
	int w,
	int h,
	Material stoneMat,
	Material airMat) {
	if (!gTilesetLoaded) {
		std::fprintf(
			stderr,
			"[WangCaveGen] generateCaveMaterials called before "
			"loadCaveTileset().\n");

		return false;
	}

	if (w <= 0 || h <= 0)
		return false;

	std::vector<unsigned char> buffer;

	if (!generateWangBuffer(
			w,
			h,
			buffer))
		return false;

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			int gx = startX + x;
			int gy = startY + y;

			if (!inBounds(gx, gy))
				continue;

			std::size_t index =
				(static_cast<std::size_t>(y) *
						static_cast<std::size_t>(w) +
					static_cast<std::size_t>(x)) *
				3u;

			bool solid =
				classifyColorInternal(
					buffer[index + 0],
					buffer[index + 1],
					buffer[index + 2]);

			setMaterialForced(
				gx,
				gy,
				solid ? stoneMat : airMat);
		}
	}

	return true;
}

CaveBiome caveBiomeAt(
	int x,
	int y,
	const CaveGenerationSettings& settings) {
	if (!inBounds(x, y))
		return CaveBiome::REGULAR;

	return caveBiomeAtInternal(
		x,
		y,
		settings);
}

bool caveIsOpen(
	int x,
	int y) {
	return isOpenCell(
		x,
		y);
}

bool caveIsStoneLike(
	Material material) {
	return isStoneLikeInternal(
		material);
}

bool caveIsVegetation(
	Material material) {
	return isVegetationMaterialInternal(
		material);
}

bool caveIsWood(
	Material material) {
	return material == WOOD;
}

bool decorateExistingCave(
	int startX,
	int startY,
	int w,
	int h,
	const CaveGenerationSettings& settings) {
	if (w <= 0 || h <= 0)
		return false;

	gLastSettings = settings;
	gHaveLastSettings = true;

	decoratePass(
		startX,
		startY,
		w,
		h,
		settings);

	generateFlatSurface(
		startX,
		startY,
		w,
		h,
		settings);

	return true;
}

bool generateAdvancedCave(
	int startX,
	int startY,
	int w,
	int h,
	const CaveGenerationSettings& settings) {
	if (!gTilesetLoaded) {
		std::fprintf(
			stderr,
			"[WangCaveGen] generateAdvancedCave called before "
			"loadCaveTileset().\n");

		return false;
	}

	if (w <= 0 || h <= 0)
		return false;

	if (startX < 0 ||
		startY < 0 ||
		startX >= GRID_W ||
		startY >= GRID_H)
		return false;

	int clippedWidth =
		std::fmin(
			w,
			GRID_W - startX);

	int clippedHeight =
		std::fmin(
			h,
			GRID_H - startY);

	if (clippedWidth <= 0 ||
		clippedHeight <= 0)
		return false;

	gLastSettings = settings;
	gHaveLastSettings = true;

	// ------------------------------------------------------------------------
	// PASS 1: Wang cave silhouette
	// ------------------------------------------------------------------------
	//
	// This is deliberately the only pass allowed to decide the broad AIR /
	// STONE shape. Every later pass decorates or changes the material of that
	// shape.
	// ------------------------------------------------------------------------

	if (!generateScaledWang(
			startX,
			startY,
			clippedWidth,
			clippedHeight,
			settings.wangScale)) {
		return false;
	}

	// ------------------------------------------------------------------------
	// PASS 2: biome materials
	// ------------------------------------------------------------------------

	applyBiomeBaseMaterials(
		startX,
		startY,
		clippedWidth,
		clippedHeight,
		settings);

	// ------------------------------------------------------------------------
	// PASS 3: biome features
	// ------------------------------------------------------------------------

	decoratePass(
		startX,
		startY,
		clippedWidth,
		clippedHeight,
		settings);

	// Re-apply the surface after biome/features so no decoration pass can turn
	// the spawn plateau back into an underground cave or an uneven ledge.
	generateFlatSurface(
		startX,
		startY,
		clippedWidth,
		clippedHeight,
		settings);

	// ------------------------------------------------------------------------
	// PASS 4: final material/feature sanitation
	// ------------------------------------------------------------------------

	removeIsolatedVegetation(
		startX,
		startY,
		clippedWidth,
		clippedHeight);

	preventWoodInWater(
		startX,
		startY,
		clippedWidth,
		clippedHeight);

	// ------------------------------------------------------------------------
	// PASS 5: mark all cells as changed so render/sim synchronization sees
	// the newly generated world.
	// ------------------------------------------------------------------------

	for (int y = startY;
		y < startY + clippedHeight;
		++y) {

		for (int x = startX;
			x < startX + clippedWidth;
			++x) {

			markCellChanged(
				x,
				y);
		}
	}

	if (static_cast<long long>(clippedWidth) * clippedHeight > 100000)
		std::fprintf(
			stdout,
			"[WangCaveGen] generated advanced cave: "
			"%dx%d at (%d,%d), Wang scale=%d, seed=%u\n",
			clippedWidth,
			clippedHeight,
			startX,
			startY,
			std::fmax(1, settings.wangScale),
			settings.seed);

	return true;
}


// ============================================================================
// TUNING NOTES
// ============================================================================
//
// Regular caves
// -------------
// REGULAR is intentionally the baseline. The Wang silhouette remains STONE
// with no mandatory decoration. This is useful because large ordinary regions
// prevent the world from becoming a theme-park of special rooms.
//
// Lush caves
// ----------
// Lush caves use:
//   STONE  -> structural rock
//   DIRT   -> only indirectly available for future expansion
//   GRASS  -> floor vegetation
//   MOSS   -> wall/ceiling vegetation and vines
//   WATER  -> shallow pools
//
// The vegetation code is geometry-aware. Grass requires a solid floor and an
// open cell above it. Moss requires an exposed wall/ceiling/floor adjacency.
// Vines require a solid ceiling and then travel down through AIR.
//
// Mines
// -----
// Mine platforms are not randomly floating in the cave. The generator first
// searches for a solid stone edge/corner that borders an open cell. The open
// cell directly above that edge becomes WOOD. Neighboring edge cells are then
// extended into a platform.
//
// This means the requested "stone corner -> wood platform" relationship is
// explicit in the generator rather than being an accidental decoration.
//
// Mine balconies use a longer horizontal run and add short vertical braces.
// This creates a different structure from the simple ledge platform.
//
// Ice
// ---
// ICE is used for structural wall replacement. SNOW is applied to exposed
// surfaces. WATER can appear in low open pockets. Shelves, stalactites and
// stalagmites use ICE and are derived from actual open/solid boundaries.
//
// Temple
// ------
// TEMPLEBRICK is the actual material supplied by Material.h and therefore is
// the material used as CAVE_TEMPLESTONE_MATERIAL. Temple rooms are stamped
// only into completely open rectangles. This prevents architecture from
// blindly overwriting the Wang cave walls.
//
// Temple moss/vines are then applied to temple masonry boundaries. A small
// amount of WOOD is used for altar/platform details.
//
// Determinism
// -----------
// All biome decisions and feature decisions are based on integer hashes of
// world coordinates and the configured seed. The same seed therefore creates
// the same biome field and feature locations, assuming the same Wang tile
// output.
//
// Wang randomness itself is owned by stb_herringbone_wang_tile. If exact
// replay of the Wang silhouette is required across multiple generations,
// keep the same process/environment and reuse the same tileset. The feature
// layer remains deterministic independently.
//
// Performance
// -----------
// The generator deliberately avoids flood-filling every cave chamber for
// every feature. Most tests are local neighborhood checks. Large structure
// passes are sparse and gated by biome membership and hash chance.
//
// If GRID_W/GRID_H becomes very large, the easiest optimization is to run
// generateAdvancedCave() per world chunk and give each chunk a coordinate
// offset. The biome function already accepts absolute grid coordinates, so
// chunk boundaries can remain visually coherent.
//
// Extension points
// ----------------
// Additional materials can be inserted into baseMaterialForBiome(). New
// biomes can be added to CaveBiome and the corresponding weighted selection.
// New structures should preferably be added as another post-Wang pass.
//
// Important material constraint
// ----------------------------
// The supplied Material.h contains:
//   AIR, SAND, WATER, STONE, WOOD, LAVA, FIRE, ACID, ICE, GLASS, SMOKE,
//   GASS, MAGIC, SNOW, FIREFLY, OBSIDIAN, BRICK, SANDSTONE, OIL,
//   GUNPOWDER, MAGICGASS, POISONGASS, FUMMES, PORTAL, FOG, DECOR, STEAM,
//   NITROGLYCERIN, MOSS, GRASS, REDMAGIC, TOXICSLUDGE, ELECTRCITY, LIGHT,
//   STEEL, RUSTED_STEEL, TEMPLEBRICK, CYBERBRICK, STAR, SHOOTINGSTAR,
//   NANOBOTS, BLACKWIDOW, GLASSSHARD, ICESHARD, MAGMA, DIRT.
//
// There is no VINE material in that list. MOSS is therefore used for vines.
// There is no TEMPLESTONE material in that list. TEMPLEBRICK is therefore
// used for temple masonry.
//
// ============================================================================
// FEATURE CHECKLIST
// ============================================================================
//
// [x] Wang cave silhouette
// [x] Large contiguous biome field
// [x] Regular caves
// [x] Lush caves
// [x] Grass
// [x] Moss
// [x] Water pools
// [x] Hanging vine strands
// [x] Mines
// [x] Stone
// [x] Dirt
// [x] Wood platforms
// [x] Wood edge/corner detection
// [x] Wood balconies
// [x] Wood supports
// [x] Ice
// [x] Snow
// [x] Water in ice regions
// [x] Ice shelves
// [x] Ice stalactites
// [x] Ice stalagmites
// [x] Temple masonry
// [x] Temple moss
// [x] Temple vines
// [x] Temple wood structures
// [x] Deterministic feature placement
// [x] Existing Wang API preserved
//
// ============================================================================
// END OF TUNING NOTES
// ============================================================================

// ============================================================================
// GENERATION PIPELINE REFERENCE
// ============================================================================
//
// generateAdvancedCave()
//     |
//     +--> validate bounds
//     |
//     +--> generateScaledWang()
//     |       |
//     |       +--> stbhw_generate_image()
//     |       +--> classifyColorInternal()
//     |       +--> STONE/AIR base field
//     |
//     +--> applyBiomeBaseMaterials()
//     |       |
//     |       +--> caveBiomeAtInternal()
//     |       +--> baseMaterialForBiome()
//     |
//     +--> generateLushFeatures()
//     |       +--> grass
//     |       +--> moss
//     |       +--> water
//     |       +--> vines
//     |
//     +--> generateMineFeatures()
//     |       +--> edge/corner platforms
//     |       +--> balconies
//     |       +--> supports
//     |
//     +--> generateIceFeatures()
//     |       +--> snow
//     |       +--> water
//     |       +--> shelves
//     |       +--> stalactites/stalagmites
//     |
//     +--> generateTempleFeatures()
//     |       +--> rooms
//     |       +--> pillars
//     |       +--> moss
//     |       +--> vines
//     |       +--> wood
//     |
//     +--> cleanup
//     |       +--> remove isolated vegetation
//     |       +--> prevent wood in water
//     |
//     +--> mark grid changed
//
// ============================================================================
// EDGE / CORNER PLATFORM REFERENCE
// ============================================================================
//
// A mine platform candidate is accepted only when:
//
//     current cell     = solid
//     cell above       = AIR
//     cell below       = solid
//
// and the current cell participates in a corner/edge relation such as:
//
//          AIR
//       AIR X SOLID
//
// or:
//
//       SOLID
//       X
//       AIR
//
// The generator then writes WOOD into the open cell above the stone ledge.
// It expands left/right only through additional ledge anchors.
//
// This is intentionally conservative. It prevents the common procedural
// generation artifact where platforms float in the middle of empty space.
//
// ============================================================================
// BIOME SCALE REFERENCE
// ============================================================================
//
// biomeCellSize is deliberately much larger than wangScale.
//
// Example:
//
//     wangScale     = 4
//     biomeCellSize = 96
//
// A biome decision therefore covers roughly 24 Wang-scale units before the
// low-frequency warp is considered. This creates large regions similar in
// spirit to Noita-style biome progression rather than tiny noise patches.
//
// ============================================================================
// MATERIAL REFERENCE
// ============================================================================
//
// REGULAR
//     STONE
//
// LUSH_CAVES
//     STONE
//     DIRT-compatible surfaces
//     GRASS
//     MOSS
//     WATER
//     MOSS-as-vine
//
// MINES
//     STONE
//     DIRT
//     WOOD
//
// ICE
//     STONE
//     ICE
//     SNOW
//     WATER
//
// TEMPLE
//     TEMPLEBRICK
//     MOSS
//     MOSS-as-vine
//     WOOD
//
// ============================================================================
// END GENERATION PIPELINE REFERENCE
// ============================================================================

// ============================================================================
// LUSH CAVE TUNING
// ============================================================================
// lushGrassChance controls the number of floor grass candidates.
// lushMossChance controls exposed moss candidates.
// lushWaterChance controls low-pool candidate creation.
// lushVineChance controls ceiling vine candidates.
// lushGrassPatchLength controls horizontal grass grouping.
// maximumVineLength limits hanging moss strands.
// ============================================================================

// ============================================================================
// MINE TUNING
// ============================================================================
// mineDirtChance controls deterministic dirt seams in mine walls.
// minePlatformChance controls stone-edge platform creation.
// mineBalconyChance controls larger wooden balconies.
// mineSupportChance controls braces and vertical supports.
// minimumPlatformLength rejects tiny one-cell platforms.
// maximumPlatformLength prevents enormous wooden shelves.
// mineBalconyWidthMin controls the smallest balcony.
// mineBalconyWidthMax controls the largest balcony.
// ============================================================================

// ============================================================================
// ICE TUNING
// ============================================================================
// iceReplaceStoneChance controls structural ice coverage.
// iceSnowChance controls exposed snow coverage.
// iceWaterChance controls water pocket candidates.
// iceShelfChance controls horizontal ice shelves.
// iceStalactiteChance controls downward formations.
// iceStalagmiteChance controls upward formations.
// ============================================================================

// ============================================================================
// TEMPLE TUNING
// ============================================================================
// templeMasonryChance controls temple wall replacement.
// templeMossChance controls moss attached to masonry.
// templeVineChance controls moss-as-vine strands.
// templeWoodChance controls sparse wood details.
// templePillarChance controls vertical architectural elements.
// templeRoomChance controls room stamping.
// ============================================================================

// ============================================================================
// BIOME TUNING
// ============================================================================
// regularWeight is the baseline cave weight.
// lushWeight controls the baseline lush probability.
// minesWeight controls the baseline mine probability.
// iceWeight controls the baseline ice probability.
// templeWeight controls the baseline temple probability.
// biomeCellSize controls biome region scale.
// biomeWarp bends biome boundaries.
// ============================================================================


// -----------------------------------------------------------------------------
// Streaming API
// -----------------------------------------------------------------------------
void setCaveWorldOrigin(long long x, long long y) {
	gOriginX = x;
	gOriginY = y;
}

void getCaveWorldOrigin(long long& x, long long& y) {
	x = gOriginX;
	y = gOriginY;
}

int caveWorldWidth() { return worldCellsW(); }
int caveWorldHeight() { return worldCellsH(); }

// Strips waiting to be generated (window coordinates). Filled by
// shiftCaveWindow(..., deferGeneration=true), drained by pumpCaveWindowGeneration.
struct PendingRect {
	int x, y, w, h;
};
static std::vector<PendingRect> gPendingRects;

// -----------------------------------------------------------------------------
// Region cache glue (see WorldCache.h)
// -----------------------------------------------------------------------------
static bool gWindowHasContent = false; // false until the first window exists

// Cache regions are REGION_SIZE-aligned in UNWRAPPED absolute coords (floor(
// (origin + gx) / REGION_SIZE)). The world's wrap width is never a multiple of
// 26, so keying on wrapped coords would break alignment after a lap; unwrapped
// keys just mean a second lap around the (~790k cell) world doesn't share
// edits with the first, which is irrelevant in practice.
// Only requirement: the window origin must be region-aligned (shifts snap to it).
static bool cacheUsable() {
	static bool warned = false;
	const char* why = nullptr;
	if (!WorldCache::enabled())
		why = "cache not initialised - call initCaveCache() before regenerateCaveWindow()";
	else if (gOriginX % REGION_SIZE != 0 || gOriginY % REGION_SIZE != 0)
		why = "window origin is not a multiple of REGION_SIZE";
	if (why) {
		if (!warned) {
			std::fprintf(stderr, "[WorldCache] disabled: %s\n", why);
			warned = true;
		}
		return false;
	}
	return true;
}

static bool overlapsPending(int x0, int y0, int x1, int y1) {
	for (const auto& r : gPendingRects)
		if (r.x < x1 && r.x + r.w > x0 && r.y < y1 && r.y + r.h > y0)
			return true;
	return false;
}

static void absRegionOf(int gx, int gy, int& rx, int& ry) {
	rx = static_cast<int>(floorDivLL(gOriginX + gx, REGION_SIZE));
	ry = static_cast<int>(floorDivLL(gOriginY + gy, REGION_SIZE));
}

// Snapshot every region inside window rect [x0,x1) x [y0,y1) to the cache.
// Regions that are still waiting in the generation queue hold placeholder air
// and are skipped so they never overwrite good data on disk.
static void cacheSaveRect(int x0, int y0, int x1, int y1) {
	if (!cacheUsable())
		return;
	for (int gy = (y0 / REGION_SIZE) * REGION_SIZE; gy < y1; gy += REGION_SIZE)
		for (int gx = (x0 / REGION_SIZE) * REGION_SIZE; gx < x1; gx += REGION_SIZE) {
			if (gx < 0 || gy < 0 || gx + REGION_SIZE > GRID_W || gy + REGION_SIZE > GRID_H)
				continue;
			if (overlapsPending(gx, gy, gx + REGION_SIZE, gy + REGION_SIZE))
				continue;
			int rx, ry;
			absRegionOf(gx, gy, rx, ry);
			WorldCache::saveRegionAsync(rx, ry, gx, gy);
		}
}

// Fill a window rect: cached regions are loaded from disk, runs of uncached
// regions are generated (merged into as few generateAdvancedCave calls as
// possible). Falls back to plain generation if the cache can't be used.
static bool generateOrLoadRect(
	int x, int y, int w, int h, const CaveGenerationSettings& settings) {
	if (!cacheUsable() || x % REGION_SIZE || y % REGION_SIZE ||
		w % REGION_SIZE || h % REGION_SIZE)
		return generateAdvancedCave(x, y, w, h, settings);

	bool ok = true;
	const bool byCol = h > w; // walk along the long side so runs merge
	const int nOuter = (byCol ? w : h) / REGION_SIZE;
	const int nInner = (byCol ? h : w) / REGION_SIZE;
	for (int o = 0; o < nOuter; ++o) {
		int runStart = -1;
		for (int i = 0; i <= nInner; ++i) {
			bool cached = false;
			if (i < nInner) {
				const int gx = byCol ? x + o * REGION_SIZE : x + i * REGION_SIZE;
				const int gy = byCol ? y + i * REGION_SIZE : y + o * REGION_SIZE;
				int rx, ry;
				absRegionOf(gx, gy, rx, ry);
				cached = WorldCache::hasRegion(rx, ry) && WorldCache::loadRegion(rx, ry, gx, gy);
			}
			if (i < nInner && !cached) {
				if (runStart < 0)
					runStart = i;
			}
			else if (runStart >= 0) {
				const int len = (i - runStart) * REGION_SIZE;
				if (byCol)
					ok &= generateAdvancedCave(x + o * REGION_SIZE, y + runStart * REGION_SIZE, REGION_SIZE, len, settings);
				else
					ok &= generateAdvancedCave(x + runStart * REGION_SIZE, y + o * REGION_SIZE, len, REGION_SIZE, settings);
				runStart = -1;
			}
		}
	}
	return ok;
}

bool regenerateCaveWindow(
	long long originX,
	long long originY,
	const CaveGenerationSettings& settings) {
	gLastSettings = settings;
	ensureMaster(settings.seed, std::fmax(1, settings.wangScale)); // world size needed by the cache

	// Persist whatever is currently loaded before it is replaced.
	if (gWindowHasContent)
		cacheSaveRect(0, 0, GRID_W, GRID_H);
	gPendingRects.clear();

	gOriginX = originX;
	gOriginY = originY;
	const bool ok = generateOrLoadRect(0, 0, GRID_W, GRID_H, settings);
	gWindowHasContent = true;
	return ok;
}

// Cache lifetime helpers (call from Eternal.cpp).
void initCaveCache(const CaveGenerationSettings& settings) {
	WorldCache::init(settings.seed, std::max(1, settings.wangScale));
}

void saveCaveWindowToCache() {
	if (gWindowHasContent)
		cacheSaveRect(0, 0, GRID_W, GRID_H);
	WorldCache::flush();
}

void shutdownCaveCache() {
	saveCaveWindowToCache();
	WorldCache::shutdown();
}

// Scroll the window by (dx, dy) cells: slide existing cells (keeping sand/water
// and all edits), then generate ONLY the newly exposed strips. With
// deferGeneration the strips are cleared to air and queued; call
// pumpCaveWindowGeneration() each tick to fill them in a few ms at a time.
bool shiftCaveWindow(
	int dx,
	int dy,
	const CaveGenerationSettings& settings,
	bool deferGeneration) {
	if (dx == 0 && dy == 0)
		return true;

	if (std::abs(dx) >= GRID_W || std::abs(dy) >= GRID_H) {
		return regenerateCaveWindow(gOriginX + dx, gOriginY + dy, settings);
	}

	gLastSettings = settings;
	if (!ensureMaster(settings.seed, std::fmax(1, settings.wangScale)))
		return false;

	// Save the strips that are about to scroll out (old origin still valid).
	if (dx > 0)
		cacheSaveRect(0, 0, dx, GRID_H);
	else if (dx < 0)
		cacheSaveRect(GRID_W + dx, 0, GRID_W, GRID_H);
	if (dy > 0)
		cacheSaveRect(0, 0, GRID_W, dy);
	else if (dy < 0)
		cacheSaveRect(0, GRID_H + dy, GRID_W, GRID_H);

	const bool cacheOk =
		gBiomeCache.size() == static_cast<std::size_t>(GRID_W) * GRID_H &&
		gCacheOX == gOriginX && gCacheOY == gOriginY;

	// new[y][x] = old[y + dy][x + dx]
	const int copyW = GRID_W - std::abs(dx);
	const int dstX = dx < 0 ? -dx : 0;
	const int srcX = dx > 0 ? dx : 0;

	auto shiftRows = [&](int y) {
		int sy = y + dy;
		if (sy < 0 || sy >= GRID_H)
			return;
		std::memmove(&gMap[y][dstX], &gMap[sy][srcX], static_cast<std::size_t>(copyW) * sizeof(Pixel));
		if (cacheOk)
			std::memmove(&gBiomeCache[static_cast<std::size_t>(y) * GRID_W + dstX],
				&gBiomeCache[static_cast<std::size_t>(sy) * GRID_W + srcX],
				static_cast<std::size_t>(copyW));
	};
	if (dy >= 0)
		for (int y = 0; y < GRID_H; ++y)
			shiftRows(y);
	else
		for (int y = GRID_H - 1; y >= 0; --y)
			shiftRows(y);

	gOriginX += dx;
	gOriginY += dy;
	if (cacheOk) {
		gCacheOX = gOriginX;
		gCacheOY = gOriginY;
	}

	// Re-anchor any strips still waiting from an earlier shift.
	for (auto& r : gPendingRects) {
		r.x -= dx;
		r.y -= dy;
		int x1 = std::fmin(r.x + r.w, GRID_W), y1 = std::fmin(r.y + r.h, GRID_H);
		r.x = std::fmax(r.x, 0);
		r.y = std::max(r.y, 0);
		r.w = x1 - r.x;
		r.h = y1 - r.y;
	}
	gPendingRects.erase(
		std::remove_if(gPendingRects.begin(), gPendingRects.end(),
			[](const PendingRect& r) { return r.w <= 0 || r.h <= 0; }),
		gPendingRects.end());

	auto clearRect = [&](int x0, int y0, int x1, int y1) {
		for (int y = y0; y < y1; ++y)
			for (int x = x0; x < x1; ++x) {
				gMap[y][x] = Pixel{};
				gMap[y][x].type = AIR;
				gMap[y][x].color = gColors[AIR];
				if (cacheOk)
					gBiomeCache[static_cast<std::size_t>(y) * GRID_W + x] = kBiomeUnknown;
			}
	};

	// Queue a rect in slices along its long side, nearest-to-existing-content
	// first so each slice decorates against already-generated neighbours.
	auto queueRect = [&](int x0, int y0, int x1, int y1, bool fromHigh) {
		constexpr int kSlice = REGION_SIZE * 2; // region-aligned so cache loads line up
		int w = x1 - x0, h = y1 - y0;
		std::vector<PendingRect> slices;
		if (w >= h) { // slice along X
			for (int x = x0; x < x1; x += kSlice)
				slices.push_back({ x, y0, std::min(kSlice, x1 - x), h });
		}
		else {
			for (int y = y0; y < y1; y += kSlice)
				slices.push_back({ x0, y, w, std::min(kSlice, y1 - y) });
		}
		if (fromHigh) // existing content is on the low side -> go low to high
			;		  // already ascending
		else
			std::reverse(slices.begin(), slices.end());
		// gPendingRects is consumed from the back, so push in reverse order.
		for (auto it = slices.rbegin(); it != slices.rend(); ++it)
			gPendingRects.push_back(*it);
	};

	int sx0 = dx > 0 ? GRID_W - dx : 0, sx1 = dx > 0 ? GRID_W : -dx;
	int sy0 = dy > 0 ? GRID_H - dy : 0, sy1 = dy > 0 ? GRID_H : -dy;

	bool ok = true;
	if (dx != 0) {
		clearRect(sx0, 0, sx1, GRID_H);
		if (deferGeneration)
			queueRect(sx0, 0, sx1, GRID_H, dx > 0);
		else
			ok &= generateOrLoadRect(sx0, 0, sx1 - sx0, GRID_H, settings);
	}
	if (dy != 0) {
		clearRect(0, sy0, GRID_W, sy1);
		if (deferGeneration)
			queueRect(0, sy0, GRID_W, sy1, dy > 0);
		else
			ok &= generateOrLoadRect(0, sy0, GRID_W, sy1 - sy0, settings);
	}
	return ok;
}

// Generate queued strip slices until ~maxCells cells have been processed.
// Returns true while work remains.
bool pumpCaveWindowGeneration(
	int maxCells,
	const CaveGenerationSettings& settings) {
	long long done = 0;
	while (!gPendingRects.empty() && done < maxCells) {
		PendingRect r = gPendingRects.back();
		gPendingRects.pop_back();
		generateOrLoadRect(r.x, r.y, r.w, r.h, settings);
		done += static_cast<long long>(r.w) * r.h;
	}
	return !gPendingRects.empty();
}

// -----------------------------------------------------------------------------
// Parallax background layers
// -----------------------------------------------------------------------------
// Builds one horizontally-tileable RGBA layer of cave interior from the same
// Wang mosaic the playable world uses. layerIndex 0 is the far back wall
// (opaque); higher layers are nearer, darker rock silhouettes with transparent
// cave openings so the layers behind show through.
bool generateCaveBackgroundLayer(
	int layerIndex,
	int layerCount,
	int w,
	int h,
	const CaveGenerationSettings& settings,
	std::vector<unsigned char>& rgba) {
	if (w <= 0 || h <= 0 || layerCount <= 0)
		return false;
	if (!gTilesetLoaded)
		return false;
	if (!ensureMaster(settings.seed, std::fmax(1, settings.wangScale)))
		return false;

	const float t01 = layerCount > 1
						  ? static_cast<float>(layerIndex) / static_cast<float>(layerCount - 1)
						  : 0.0f;

	// Pixels per Wang pixel: nearer layers get chunkier shapes.
	const float ps = 4.0f + 3.5f * static_cast<float>(layerIndex);
	const float periodW = static_cast<float>(w) / ps; // one tile width, in Wang px
	const float baseX = 137.0f + 331.0f * static_cast<float>(layerIndex);
	const float baseY = 59.0f + 197.0f * static_cast<float>(layerIndex);
	const float threshold = 0.50f + 0.05f * static_cast<float>(layerIndex); // nearer = more open

	auto sampleF = [&](float wx, float wy) {
		long long x0 = static_cast<long long>(std::floor(wx));
		long long y0 = static_cast<long long>(std::floor(wy));
		float fx = wx - static_cast<float>(x0);
		float fy = wy - static_cast<float>(y0);
		return (masterAt(x0, y0) * (1 - fx) + masterAt(x0 + 1, y0) * fx) * (1 - fy) +
			   (masterAt(x0, y0 + 1) * (1 - fx) + masterAt(x0 + 1, y0 + 1) * fx) * fy;
	};

	// Seamless-in-X density: cross-fade the field with a copy shifted by one
	// period so column w-1 continues into column 0.
	std::vector<float> density(static_cast<std::size_t>(w) * h);
	const int noiseCell = std::fmax(4, static_cast<int>(ps * 2.0f));
	const int noiseCols = std::fmax(1, w / noiseCell);

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			float wx = baseX + static_cast<float>(x) / ps;
			float wy = baseY + static_cast<float>(y) / ps;
			float t = static_cast<float>(x) / static_cast<float>(w);
			float d = (1.0f - t) * sampleF(wx, wy) + t * sampleF(wx - periodW, wy);

			// Tileable low-frequency wobble so edges aren't perfectly smooth.
			int ix = x / noiseCell, iy = y / noiseCell;
			float fx = static_cast<float>(x % noiseCell) / noiseCell;
			float fy = static_cast<float>(y % noiseCell) / noiseCell;
			fx = fx * fx * (3.0f - 2.0f * fx);
			fy = fy * fy * (3.0f - 2.0f * fy);
			int ixn = (ix + 1) % noiseCols;
			ix %= noiseCols;
			float a = wangHash01(ix + layerIndex * 31, iy), b = wangHash01(ixn + layerIndex * 31, iy);
			float c = wangHash01(ix + layerIndex * 31, iy + 1), e = wangHash01(ixn + layerIndex * 31, iy + 1);
			float n = (a + (b - a) * fx) + ((c + (e - c) * fx) - (a + (b - a) * fx)) * fy;
			d += (n - 0.5f) * 0.25f;

			density[static_cast<std::size_t>(y) * w + x] = d;
		}
	}

	rgba.assign(static_cast<std::size_t>(w) * h * 4, 0);

	auto lerpI = [](float a, float b, float t) { return a + (b - a) * t; };
	// Far = light bluish haze, near = near-black.
	const float baseR = lerpI(78.0f, 16.0f, t01);
	const float baseG = lerpI(82.0f, 17.0f, t01);
	const float baseB = lerpI(104.0f, 26.0f, t01);

	for (int y = 0; y < h; ++y) {
		for (int x = 0; x < w; ++x) {
			std::size_t i = static_cast<std::size_t>(y) * w + x;
			float d = density[i];
			unsigned char* px = &rgba[i * 4];

			float speckle = (wangHash01(x * 7 + layerIndex, y * 13) - 0.5f) * 10.0f;

			if (layerIndex == 0) {
				// Opaque back wall; the mask just modulates brightness so the
				// far wall still reads as cave shapes.
				float shade = (d > threshold ? 0.0f : -14.0f) + (d - 0.5f) * 18.0f;
				float vy = (static_cast<float>(y) / h - 0.5f) * -10.0f;
				px[0] = static_cast<unsigned char>(clampInt(static_cast<int>(baseR - 38 + shade + vy + speckle), 0, 255));
				px[1] = static_cast<unsigned char>(clampInt(static_cast<int>(baseG - 40 + shade + vy + speckle), 0, 255));
				px[2] = static_cast<unsigned char>(clampInt(static_cast<int>(baseB - 36 + shade + vy + speckle), 0, 255));
				px[3] = 255;
				continue;
			}

			if (d <= threshold)
				continue; // open cave: transparent

			// Rim light where rock borders open space above/left.
			bool rim = false;
			if (y > 0 && density[i - w] <= threshold)
				rim = true;
			else if (x > 0 && density[i - 1] <= threshold)
				rim = true;
			float boost = rim ? 26.0f : 0.0f;

			px[0] = static_cast<unsigned char>(clampInt(static_cast<int>(baseR + boost + speckle), 0, 255));
			px[1] = static_cast<unsigned char>(clampInt(static_cast<int>(baseG + boost + speckle), 0, 255));
			px[2] = static_cast<unsigned char>(clampInt(static_cast<int>(baseB + boost * 1.15f + speckle), 0, 255));
			px[3] = 255;
		}
	}
	return true;
}
