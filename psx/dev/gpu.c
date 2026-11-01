#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdio.h>

#include "gpu.h"
#include "../log.h"

#define SE10(v) ((int16_t)((uint16_t)(v) << 5) >> 5)
#define CLAMP(v, d, u) (((v) <= (d)) ? (d) : (((v) >= (u)) ? (u) : (v)))
#define EDGE(a, b, c) (((b).x - (a).x) * ((c).y - (a).y) - ((b).y - (a).y) * ((c).x - (a).x))
#define TL(z, a, b) (((z) < 0) || (((z) == 0) && (((b).y > (a).y) || (((b).y == (a).y) && ((b).x < (a).x)))))

static const int g_psx_gpu_dither_kernel[16] = {
    -4, +0, -3, +1,
    +2, -2, +3, -1,
    -3, +1, -4, +0,
    +3, -1, +2, -2,
};

static inline uint16_t gpu_to_bgr555(uint32_t color) {
    return ((color & 0x0000f8) >> 3) |
           ((color & 0x00f800) >> 6) |
           ((color & 0xf80000) >> 9);
}

#define BGR555(c) \
    ((((c) & 0x0000f8) >> 3) | \
     (((c) & 0x00f800) >> 6) | \
     (((c) & 0xf80000) >> 9))

static inline int min(int x0, int x1) { return (x0 <= x1) ? x0 : x1; }
static inline int max(int x0, int x1) { return (x0 >= x1) ? x0 : x1; }

static inline int min3(int a, int b, int c) {
    int m = (a <= b) ? a : b;
    return (m <= c) ? m : c;
}

static inline int max3(int a, int b, int c) {
    int m = (a > b) ? a : b;
    return (m > c) ? m : c;
}

static inline int clamp_u8(int v) {
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static inline int clamp_u5(int v) {
    if (v < 0) return 0;
    if (v > 31) return 31;
    return v;
}

/* 정수 기반 고속 반투명 블렌딩 헬퍼 (BGR555 직접 연산) */
static inline uint16_t gpu_blend_color(uint16_t back, uint16_t front, int transp_mode) {
    int br = (back >> 0)  & 0x1f;
    int bg = (back >> 5)  & 0x1f;
    int bb = (back >> 10) & 0x1f;

    int cr = (front >> 0)  & 0x1f;
    int cg = (front >> 5)  & 0x1f;
    int cb = (front >> 10) & 0x1f;

    switch (transp_mode) {
        case 0: // 0.5 x B + 0.5 x F
            cr = (br + cr) >> 1;
            cg = (bg + cg) >> 1;
            cb = (bb + cb) >> 1;
            break;
        case 1: // 1.0 x B + 1.0 x F
            cr = clamp_u5(br + cr);
            cg = clamp_u5(bg + cg);
            cb = clamp_u5(bb + cb);
            break;
        case 2: // 1.0 x B - 1.0 x F
            cr = clamp_u5(br - cr);
            cg = clamp_u5(bg - cg);
            cb = clamp_u5(bb - cb);
            break;
        case 3: // 1.0 x B + 0.25 x F
            cr = clamp_u5(br + (cr >> 2));
            cg = clamp_u5(bg + (cg >> 2));
            cb = clamp_u5(bb + (cb >> 2));
            break;
    }

    return (uint16_t)(cr | (cg << 5) | (cb << 10) | (front & 0x8000));
}

psx_gpu_t* psx_gpu_create(void) {
    return (psx_gpu_t*)malloc(sizeof(psx_gpu_t));
}

void psx_gpu_init(psx_gpu_t* gpu, psx_ic_t* ic) {
    memset(gpu, 0, sizeof(psx_gpu_t));

    gpu->io_base = PSX_GPU_BEGIN;
    gpu->io_size = PSX_GPU_SIZE;

    gpu->vram = (uint16_t*)malloc(PSX_GPU_VRAM_SIZE);
    gpu->empty = malloc(PSX_GPU_VRAM_SIZE);

    memset(gpu->empty, 0, PSX_GPU_VRAM_SIZE);

    gpu->state = GPU_STATE_RECV_CMD;
    gpu->gpustat |= 0x800000;
    gpu->display_mode = 1;
    gpu->ic = ic;
}

uint32_t psx_gpu_read32(psx_gpu_t* gpu, uint32_t offset) {
    switch (offset) {
        case 0x00: {
            uint32_t data = 0x0;

            if (gpu->c0_tsiz) {
                data |= gpu->vram[gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt << 10))];
                if (++gpu->c0_xcnt == gpu->c0_xsiz) {
                    gpu->c0_ycnt++;
                    gpu->c0_xcnt = 0;
                }

                data |= gpu->vram[gpu->c0_addr + (gpu->c0_xcnt + (gpu->c0_ycnt << 10))] << 16;
                if (++gpu->c0_xcnt == gpu->c0_xsiz) {
                    gpu->c0_ycnt++;
                    gpu->c0_xcnt = 0;
                }

                gpu->c0_tsiz -= 2;
            }

            if (gpu->gp1_10h_req) {
                switch (gpu->gp1_10h_req & 7) {
                    case 2:
                        data = ((gpu->texw_oy >> 3) << 15) | ((gpu->texw_ox >> 3) << 10) |
                               ((gpu->texw_my >> 3) << 5)  | (gpu->texw_mx >> 3);
                        break;
                    case 3: data = (gpu->draw_y1 << 10) | gpu->draw_x1; break;
                    case 4: data = (gpu->draw_y2 << 10) | gpu->draw_x2; break;
                    case 5: data = (gpu->off_y << 10) | gpu->off_x; break;
                }
                gpu->gp1_10h_req = 0;
            }

            return data;
        }
        case 0x04: return gpu->gpustat | 0x1c000000;
    }

    log_warn("Unhandled 32-bit GPU read at offset %08x", offset);
    return 0x0;
}

uint16_t psx_gpu_read16(psx_gpu_t* gpu, uint32_t offset) {
    printf("Unhandled 16-bit GPU read at offset %08x\n", offset);
    return 0;
}

uint8_t psx_gpu_read8(psx_gpu_t* gpu, uint32_t offset) {
    printf("Unhandled 8-bit GPU read at offset %08x\n", offset);
    return 0;
}

static inline uint16_t gpu_fetch_texel(psx_gpu_t* gpu, uint16_t tx, uint16_t ty, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth) {
    tx = ((tx & ~gpu->texw_mx) | (gpu->texw_ox & gpu->texw_mx)) & 0xff;
    ty = ((ty & ~gpu->texw_my) | (gpu->texw_oy & gpu->texw_my)) & 0xff;

    uint32_t row_offset = ((tpy + ty) & 0x1ff) << 10;

    switch (depth) {
        case 0: { // 4-bit
            uint16_t texel = gpu->vram[((tpx + (tx >> 2)) & 0x3ff) + row_offset];
            int index = (texel >> ((tx & 0x3) << 2)) & 0xf;
            return gpu->vram[((clutx + index) & 0x3ff) + (cluty << 10)];
        }
        case 1: { // 8-bit
            uint16_t texel = gpu->vram[((tpx + (tx >> 1)) & 0x3ff) + row_offset];
            int index = (texel >> ((tx & 0x1) << 3)) & 0xff;
            return gpu->vram[((clutx + index) & 0x3ff) + (cluty << 10)];
        }
        default: { // 15-bit
            return gpu->vram[((tpx + tx) & 0x3ff) + row_offset];
        }
    }
}

/* 호환성 유지를 위해 남겨두되 정수 연산으로 가속한 바이리니어 함수 */
uint16_t gpu_fetch_texel_bilinear(psx_gpu_t* gpu, float tx, float ty, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth) {
    return gpu_fetch_texel(gpu, (uint16_t)tx, (uint16_t)ty, tpx, tpy, clutx, cluty, depth);
}

void gpu_render_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, poly_data_t data, int edge) {
    vertex_t a = v0, b, c;

    /* Winding order 보정 */
    if (EDGE(v0, v1, v2) < 0) {
        b = v2;
        c = v1;
    } else {
        b = v1;
        c = v2;
    }

    a.x += gpu->off_x; a.y += gpu->off_y;
    b.x += gpu->off_x; b.y += gpu->off_y;
    c.x += gpu->off_x; c.y += gpu->off_y;

    int xmin = min3(a.x, b.x, c.x);
    int ymin = min3(a.y, b.y, c.y);
    int xmax = max3(a.x, b.x, c.x);
    int ymax = max3(a.y, b.y, c.y);

    if (((xmax - xmin) > 1024) || ((ymax - ymin) > 512))
        return;

    int32_t area = EDGE(a, b, c);
    if (area <= 0)
        return;

    /* 클리핑 영역(Drawing Area)을 루프 진입 전에 미리 적용 (핵심 최적화) */
    xmin = max(xmin, gpu->draw_x1);
    ymin = max(ymin, gpu->draw_y1);
    xmax = min(xmax, (int)gpu->draw_x2 + 1);
    ymax = min(ymax, (int)gpu->draw_y2 + 1);

    if (xmin >= xmax || ymin >= ymax)
        return;

    int tpx = (data.texp & 0xf) << 6;
    int tpy = (data.texp & 0x10) << 4;
    int clutx = (data.clut & 0x3f) << 4;
    int cluty = (data.clut >> 6) & 0x1ff;
    int depth = (data.texp >> 7) & 3;

    int is_shaded   = (data.attrib & PA_SHADED) != 0;
    int is_textured = (data.attrib & PA_TEXTURED) != 0;
    int is_transp   = (data.attrib & PA_TRANSP) != 0;
    int is_raw      = (data.attrib & PA_RAW) != 0;
    int transp_mode = is_textured ? ((data.texp >> 5) & 3) : ((gpu->gpustat >> 5) & 3);

    /* Vertex 색상 사전 추출 */
    int a_r = (a.c >> 0) & 0xff, a_g = (a.c >> 8) & 0xff, a_b = (a.c >> 16) & 0xff;
    int b_r = (b.c >> 0) & 0xff, b_g = (b.c >> 8) & 0xff, b_b = (b.c >> 16) & 0xff;
    int c_r = (c.c >> 0) & 0xff, c_g = (c.c >> 8) & 0xff, c_b = (c.c >> 16) & 0xff;

    uint32_t flat_mod = data.v[0].c;
    int flat_mr = (flat_mod >> 0) & 0xff;
    int flat_mg = (flat_mod >> 8) & 0xff;
    int flat_mb = (flat_mod >> 16) & 0xff;
    uint16_t flat_color_555 = BGR555(flat_mod);

    /* 증분 Edge Function 스텝 계산 */
    int32_t dz0_dx = b.y - c.y, dz0_dy = c.x - b.x;
    int32_t dz1_dx = c.y - a.y, dz1_dy = a.x - c.x;
    int32_t dz2_dx = a.y - b.y, dz2_dy = b.x - a.x;

    /* Top-Left Rule 바이어스 사전 계산 (루프 내 복합 조건문 제거) */
    int bias0 = ((b.y > c.y) || (b.y == c.y && b.x < c.x)) ? -1 : 0;
    int bias1 = ((c.y > a.y) || (c.y == a.y && c.x < a.x)) ? -1 : 0;
    int bias2 = ((a.y > b.y) || (a.y == b.y && a.x < b.x)) ? -1 : 0;

    vertex_t p_start = { .x = xmin, .y = ymin };
    int32_t z0_row = EDGE(b, c, p_start);
    int32_t z1_row = EDGE(c, a, p_start);
    int32_t z2_row = EDGE(a, b, p_start);

    /* 나눗셈 제거를 위한 64비트 고정소수점 역수 (1 << 30 스케일) */
    int64_t inv_area = (1LL << 30) / area;

    for (int y = ymin; y < ymax; y++) {
        uint16_t* vram_row = &gpu->vram[y << 10];
        const int* dither_row = &g_psx_gpu_dither_kernel[(y & 3) << 2];

        int32_t z0 = z0_row;
        int32_t z1 = z1_row;
        int32_t z2 = z2_row;

        for (int x = xmin; x < xmax; x++) {
           // if (((z0 + bias0) | (z1 + bias1) | (z2 + bias2)) >= 0) {
            // 수정: 공유 변(z == 0)에서의 픽셀 누락(Seam) 방지
            if ((z0 | z1 | z2) >= 0) {
                int mr, mg, mb;
                uint16_t color;
                int pixel_transp = is_transp;

                if (is_shaded) {
                    int64_t w_r = (int64_t)z0 * a_r + (int64_t)z1 * b_r + (int64_t)z2 * c_r;
                    int64_t w_g = (int64_t)z0 * a_g + (int64_t)z1 * b_g + (int64_t)z2 * c_g;
                    int64_t w_b = (int64_t)z0 * a_b + (int64_t)z1 * b_b + (int64_t)z2 * c_b;

                    int dither = dither_row[x & 3];
                   // mr = clamp_u8((int)((w_r * inv_area) >> 30) + dither);
                   // mg = clamp_u8((int)((w_g * inv_area) >> 30) + dither);
                   // mb = clamp_u8((int)((w_b * inv_area) >> 30) + dither);
                    // (1LL << 29)를 더해 반올림 처리
                    mr = clamp_u8((int)(((w_r * inv_area) + (1LL << 29)) >> 30) + dither);
                    mg = clamp_u8((int)(((w_g * inv_area) + (1LL << 29)) >> 30) + dither);
                    mb = clamp_u8((int)(((w_b * inv_area) + (1LL << 29)) >> 30) + dither);
                } else {
                    mr = flat_mr;
                    mg = flat_mg;
                    mb = flat_mb;
                }

                if (is_textured) {
                    int64_t w_tx = (int64_t)z0 * a.tx + (int64_t)z1 * b.tx + (int64_t)z2 * c.tx;
                    int64_t w_ty = (int64_t)z0 * a.ty + (int64_t)z1 * b.ty + (int64_t)z2 * c.ty;

                    //uint16_t tx = (uint16_t)((w_tx * inv_area) >> 30);
                    //uint16_t ty = (uint16_t)((w_ty * inv_area) >> 30);
                    // 텍스처 UV 좌표 보간 시 반올림 바이어스 적용 (검은색 경계선/점 제거 핵심!)
                    uint16_t tx = (uint16_t)(((w_tx * inv_area) + (1LL << 29)) >> 30);
                    uint16_t ty = (uint16_t)(((w_ty * inv_area) + (1LL << 29)) >> 30);

                    uint16_t texel = gpu_fetch_texel(gpu, tx, ty, tpx, tpy, clutx, cluty, depth);
                    if (!texel)
                        goto next_pixel;

                    if (is_transp)
                        pixel_transp = (texel & 0x8000) != 0;

                    if (is_raw) {
                        color = texel;
                    } else {
                        int tr = (texel >> 0)  & 0x1f;
                        int tg = (texel >> 5)  & 0x1f;
                        int tb = (texel >> 10) & 0x1f;

                        /* (tr * 8 * mr) / 128 == (tr * mr) >> 4, 5-bit 유지 시 >> 7 */
                        int cr = clamp_u5((tr * mr) >> 7);
                        int cg = clamp_u5((tg * mg) >> 7);
                        int cb = clamp_u5((tb * mb) >> 7);

                        color = (uint16_t)(cr | (cg << 5) | (cb << 10) | (texel & 0x8000));
                    }
                } else {
                    if (is_shaded) {
                        color = (uint16_t)((mr >> 3) | ((mg >> 3) << 5) | ((mb >> 3) << 10));
                    } else {
                        color = flat_color_555;
                    }
                }

                if (pixel_transp) {
                    color = gpu_blend_color(vram_row[x], color, transp_mode);
                }

                vram_row[x] = color;
            }

        next_pixel:
            z0 += dz0_dx;
            z1 += dz1_dx;
            z2 += dz2_dx;
        }

        z0_row += dz0_dy;
        z1_row += dz1_dy;
        z2_row += dz2_dy;
    }
}

void gpu_render_rect(psx_gpu_t* gpu, rect_data_t data) {
    uint16_t width, height;

    switch ((data.attrib >> 3) & 3) {
        case RS_VARIABLE: width = data.width; height = data.height; break;
        case RS_1X1     : width = 1;          height = 1;           break;
        case RS_8X8     : width = 8;          height = 8;           break;
        case RS_16X16   : width = 16;         height = 16;          break;
        default         : width = 0;          height = 0;           break;
    }

    int textured    = (data.attrib & RA_TEXTURED) != 0;
    int is_transp   = (data.attrib & RA_TRANSP) != 0;
    int is_raw      = (data.attrib & RA_RAW) != 0;
    int transp_mode = (gpu->gpustat >> 5) & 3;

    int clutx = (data.clut & 0x3f) << 4;
    int cluty = (data.clut >> 6) & 0x1ff;

    int x0 = SE10(data.v0.x + gpu->off_x);
    int y0 = SE10(data.v0.y + gpu->off_y);

    int xmax = x0 + width;
    int ymax = y0 + height;

    /* 클리핑 영역 사전 교차 계산 (내부 루프에서 경계 검사 제거 및 UV 오프셋 보정) */
    int xmin = max(x0, (int)gpu->draw_x1);
    int ymin = max(y0, (int)gpu->draw_y1);
    xmax = min(xmax, (int)gpu->draw_x2 + 1);
    ymax = min(ymax, (int)gpu->draw_y2 + 1);

    if (xmin >= xmax || ymin >= ymax)
        return;

    int xc_start = xmin - x0;
    int yc = ymin - y0;

    int mr = (data.v0.c >> 0)  & 0xff;
    int mg = (data.v0.c >> 8)  & 0xff;
    int mb = (data.v0.c >> 16) & 0xff;
    uint16_t flat_color = BGR555(data.v0.c);

    for (int y = ymin; y < ymax; y++, yc++) {
        uint16_t* vram_row = &gpu->vram[y << 10];
        int xc = xc_start;

        for (int x = xmin; x < xmax; x++, xc++) {
            uint16_t color;
            int pixel_transp = is_transp;

            if (textured) {
                uint16_t texel = gpu_fetch_texel(
                    gpu,
                    data.v0.tx + xc, data.v0.ty + yc,
                    gpu->texp_x, gpu->texp_y,
                    clutx, cluty,
                    gpu->texp_d
                );

                if (!texel)
                    continue;

                if (is_transp)
                    pixel_transp = (texel & 0x8000) != 0;

                if (is_raw) {
                    color = texel;
                } else {
                    int tr = (texel >> 0)  & 0x1f;
                    int tg = (texel >> 5)  & 0x1f;
                    int tb = (texel >> 10) & 0x1f;

                    int cr = clamp_u5((tr * mr) >> 7);
                    int cg = clamp_u5((tg * mg) >> 7);
                    int cb = clamp_u5((tb * mb) >> 7);

                    color = (uint16_t)(cr | (cg << 5) | (cb << 10) | (texel & 0x8000));
                }
            } else {
                color = flat_color;
            }

            if (pixel_transp) {
                color = gpu_blend_color(vram_row[x], color, transp_mode);
            }

            vram_row[x] = color;
        }
    }
}

void plotLineLow(psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int yi = 1;
    if (dy < 0) {
        yi = -1;
        dy = -dy;
    }
    int d = (2 * dy) - dx;
    int y = y0;

    int min_x = max(0, (int)gpu->draw_x1);
    int max_x = min(1023, (int)gpu->draw_x2);
    int min_y = max(0, (int)gpu->draw_y1);
    int max_y = min(511, (int)gpu->draw_y2);

    for (int x = x0; x < x1; x++) {
        if (x >= min_x && x <= max_x && y >= min_y && y <= max_y)
            gpu->vram[x + (y << 10)] = color;

        if (d > 0) {
            y += yi;
            d += (2 * (dy - dx));
        } else {
            d += 2 * dy;
        }
    }
}

void plotLineHigh(psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
    int dx = x1 - x0;
    int dy = y1 - y0;
    int xi = 1;
    if (dx < 0) {
        xi = -1;
        dx = -dx;
    }
    int d = (2 * dx) - dy;
    int x = x0;

    int min_x = max(0, (int)gpu->draw_x1);
    int max_x = min(1023, (int)gpu->draw_x2);
    int min_y = max(0, (int)gpu->draw_y1);
    int max_y = min(511, (int)gpu->draw_y2);

    for (int y = y0; y < y1; y++) {
        if (x >= min_x && x <= max_x && y >= min_y && y <= max_y)
            gpu->vram[x + (y << 10)] = color;

        if (d > 0) {
            x += xi;
            d += (2 * (dx - dy));
        } else {
            d += 2 * dx;
        }
    }
}

void plotLine(psx_gpu_t* gpu, int x0, int y0, int x1, int y1, uint16_t color) {
    if (abs(y1 - y0) < abs(x1 - x0)) {
        if (x0 > x1) plotLineLow(gpu, x1, y1, x0, y0, color);
        else         plotLineLow(gpu, x0, y0, x1, y1, color);
    } else {
        if (y0 > y1) plotLineHigh(gpu, x1, y1, x0, y0, color);
        else         plotLineHigh(gpu, x0, y0, x1, y1, color);
    }
}

void gpu_render_flat_line(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, uint32_t color) {
    v0.x += gpu->off_x;
    v0.y += gpu->off_y;
    v1.x += gpu->off_x;
    v1.y += gpu->off_y;

    plotLine(gpu, v0.x, v0.y, v1.x, v1.y, color);
}

void gpu_render_flat_rectangle(psx_gpu_t* gpu, vertex_t v, uint32_t w, uint32_t h, uint32_t color) {
    v.x += gpu->off_x;
    v.y += gpu->off_y;

    int xmin = max(v.x, (int)gpu->draw_x1);
    int ymin = max(v.y, (int)gpu->draw_y1);
    int xmax = min(v.x + (int)w, (int)gpu->draw_x2 + 1);
    int ymax = min(v.y + (int)h, (int)gpu->draw_y2 + 1);

    for (int y = ymin; y < ymax; y++) {
        uint16_t* vram_row = &gpu->vram[y << 10];
        for (int x = xmin; x < xmax; x++) {
            vram_row[x] = (uint16_t)color;
        }
    }
}

void gpu_render_textured_rectangle(psx_gpu_t* gpu, vertex_t v, uint32_t w, uint32_t h, uint16_t clutx, uint16_t cluty, uint32_t color) {
    vertex_t a = v;
    a.x += gpu->off_x;
    a.y += gpu->off_y;

    int xmin = max(a.x, (int)gpu->draw_x1);
    int ymin = max(a.y, (int)gpu->draw_y1);
    int xmax = min(a.x + (int)w, (int)gpu->draw_x2 + 1);
    int ymax = min(a.y + (int)h, (int)gpu->draw_y2 + 1);

    if (xmin >= xmax || ymin >= ymax)
        return;

    uint32_t xc_start = xmin - a.x;
    uint32_t yc = ymin - a.y;

    for (int y = ymin; y < ymax; y++, yc++) {
        uint16_t* vram_row = &gpu->vram[y << 10];
        uint32_t xc = xc_start;

        for (int x = xmin; x < xmax; x++, xc++) {
            vram_row[x] = gpu_fetch_texel(
                gpu,
                a.tx + xc, a.ty + yc,
                gpu->texp_x, gpu->texp_y,
                clutx, cluty,
                gpu->texp_d
            );
        }
    }
}

void gpu_render_flat_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t color) {
    poly_data_t poly = {0};
    poly.v[0].c = color;
    gpu_render_triangle(gpu, v0, v1, v2, poly, 0);
}

void gpu_render_shaded_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2) {
    poly_data_t poly = {0};
    poly.attrib = PA_SHADED;
    gpu_render_triangle(gpu, v0, v1, v2, poly, 0);
}

void gpu_render_textured_triangle(psx_gpu_t* gpu, vertex_t v0, vertex_t v1, vertex_t v2, uint32_t tpx, uint32_t tpy, uint16_t clutx, uint16_t cluty, int depth) {
    poly_data_t poly = {0};
    poly.attrib = PA_TEXTURED | PA_RAW;
    poly.texp = ((tpx >> 6) & 0xf) | (((tpy >> 4) & 0x10)) | ((depth & 3) << 7);
    poly.clut = (clutx >> 4) | (cluty << 6);
    gpu_render_triangle(gpu, v0, v1, v2, poly, 0);
}

void gpu_rect(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;

            int size = (gpu->buf[0] >> 27) & 3;
            int textured = (gpu->buf[0] & 0x04000000) != 0;

            gpu->cmd_args_remaining = 1 + (size == RS_VARIABLE) + textured;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                rect_data_t rect;

                rect.attrib = gpu->buf[0] >> 24;

                int textured = (rect.attrib & RA_TEXTURED) != 0;
                int raw      = (rect.attrib & RA_RAW) != 0;
                int size_offset = 2 + textured;

                rect.v0.c   = gpu->buf[0] & 0xffffff;
                rect.v0.x   = SE10(gpu->buf[1] & 0xffff);
                rect.v0.y   = SE10(gpu->buf[1] >> 16);
                rect.v0.tx  = (gpu->buf[2] >> 0) & 0xff;
                rect.v0.ty  = (gpu->buf[2] >> 8) & 0xff;
                rect.clut   = gpu->buf[2] >> 16;
                rect.width  = gpu->buf[size_offset] & 0xffff;
                rect.height = gpu->buf[size_offset] >> 16;

                if (textured && raw)
                    rect.v0.c = 0x808080;

                gpu_render_rect(gpu, rect);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_poly(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;

            int shaded   = (gpu->buf[0] & 0x10000000) != 0;
            int quad     = (gpu->buf[0] & 0x08000000) != 0;
            int textured = (gpu->buf[0] & 0x04000000) != 0;

            int fields_per_vertex = 1 + shaded + textured;
            int vertices = 3 + quad;

            gpu->cmd_args_remaining = (fields_per_vertex * vertices) - shaded;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                poly_data_t poly;

                poly.attrib = gpu->buf[0] >> 24;

                int shaded   = (poly.attrib & PA_SHADED) != 0;
                int textured = (poly.attrib & PA_TEXTURED) != 0;

                int color_offset = shaded * (2 + textured);
                int vert_offset = 1 + (textured | shaded) + (textured & shaded);
                int texc_offset = textured * (2 + shaded);
                int texp_offset = textured * (4 + shaded);

                poly.clut = gpu->buf[2] >> 16;
                poly.texp = gpu->buf[texp_offset] >> 16;

                if (textured) {
                    gpu->texp_x = (poly.texp & 0xf) << 6;
                    gpu->texp_y = (poly.texp & 0x10) << 4;
                    gpu->texp_d = (poly.texp >> 7) & 0x3;
                    gpu->gpustat &= 0xfffffe00;
                    gpu->gpustat |= poly.texp & 0x1ff;
                }

                for (int i = 0; i < 4; i++) {
                    poly.v[i].c  = gpu->buf[0 + i * color_offset] & 0xffffff;
                    poly.v[i].x  = SE10(gpu->buf[1 + i * vert_offset] & 0xffff);
                    poly.v[i].y  = SE10(gpu->buf[1 + i * vert_offset] >> 16);
                    poly.v[i].tx = gpu->buf[2 + i * texc_offset] & 0xff;
                    poly.v[i].ty = (gpu->buf[2 + i * texc_offset] >> 8) & 0xff;
                }

                if (poly.attrib & PA_QUAD) {
                    gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 1);
                    gpu_render_triangle(gpu, poly.v[1], poly.v[2], poly.v[3], poly, 1);
                } else {
                    gpu_render_triangle(gpu, poly.v[0], poly.v[1], poly.v[2], poly, 0);
                }

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_line(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;

            int shaded   = (gpu->buf[0] & 0x10000000) != 0;
            int polyline = (gpu->buf[0] & 0x08000000) != 0;

            gpu->cmd_args_remaining = polyline ? -1 : (shaded ? 3 : 2);
            gpu->line_done = 0;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (gpu->buf[0] & 0x08000000) {
                if ((gpu->buf[gpu->buf_index - 1] & 0xf000f000) == 0x50005000) {
                    gpu->state = GPU_STATE_RECV_CMD;
                    return;
                }
            } else if (!gpu->cmd_args_remaining) {
                vertex_t v0, v1;

                if (gpu->buf[0] & 0x10000000) {
                    v0.c = gpu->buf[0] & 0xffffff;
                    v1.c = gpu->buf[2] & 0xffffff;
                    v0.x = SE10(gpu->buf[1] & 0xffff);
                    v0.y = SE10(gpu->buf[1] >> 16);
                    v1.x = SE10(gpu->buf[3] & 0xffff);
                    v1.y = SE10(gpu->buf[3] >> 16);
                } else {
                    v0.c = gpu->buf[0] & 0xffffff;
                    v1.c = gpu->buf[0] & 0xffffff;
                    v0.x = SE10(gpu->buf[1] & 0xffff);
                    v0.y = SE10(gpu->buf[1] >> 16);
                    v1.x = SE10(gpu->buf[2] & 0xffff);
                    v1.y = SE10(gpu->buf[2] >> 16);
                }

                gpu_render_flat_line(gpu, v0, v1, BGR555(gpu->buf[0] & 0xffffff));

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_a0(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->state = GPU_STATE_RECV_DATA;

                gpu->xpos = gpu->buf[1] & 0x3ff;
                gpu->ypos = (gpu->buf[1] >> 16) & 0x1ff;
                gpu->xsiz = gpu->buf[2] & 0xffff;
                gpu->ysiz = gpu->buf[2] >> 16;
                gpu->xsiz = ((gpu->xsiz - 1) & 0x3ff) + 1;
                gpu->ysiz = ((gpu->ysiz - 1) & 0x1ff) + 1;
                gpu->tsiz = ((gpu->xsiz * gpu->ysiz) + 1) & 0xfffffffe;
                gpu->addr = gpu->xpos + (gpu->ypos << 10);
                gpu->xcnt = 0;
                gpu->ycnt = 0;
            }
        } break;

        case GPU_STATE_RECV_DATA: {
            unsigned int xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
            unsigned int ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;

            gpu->vram[xpos + (ypos << 10)] = gpu->recv_data & 0xffff;

            if (++gpu->xcnt == gpu->xsiz) {
                ++gpu->ycnt;
                gpu->xcnt = 0;
            }

            xpos = (gpu->xpos + gpu->xcnt) & 0x3ff;
            ypos = (gpu->ypos + gpu->ycnt) & 0x1ff;

            gpu->vram[xpos + (ypos << 10)] = gpu->recv_data >> 16;

            if (++gpu->xcnt == gpu->xsiz) {
                ++gpu->ycnt;
                gpu->xcnt = 0;
            }

            gpu->tsiz -= 2;

            if (!gpu->tsiz) {
                gpu->xcnt = 0;
                gpu->ycnt = 0;
                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_c0(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->c0_xcnt = 0;
                gpu->c0_ycnt = 0;
                uint32_t c0_xpos = gpu->buf[1] & 0x3ff;
                uint32_t c0_ypos = (gpu->buf[1] >> 16) & 0x1ff;
                gpu->c0_xsiz = ((gpu->buf[2] & 0xffff) - 1 & 0x3ff) + 1;
                gpu->c0_ysiz = (((gpu->buf[2] >> 16) - 1) & 0x1ff) + 1;
                gpu->c0_tsiz = ((gpu->c0_xsiz * gpu->c0_ysiz) + 1) & 0xfffffffe;
                gpu->c0_addr = c0_xpos + (c0_ypos << 10);

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_02(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 2;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                gpu->color = gpu->buf[0] & 0xffffff;
                gpu->v0.x  = (gpu->buf[1] & 0xffff) & 0x3f0;
                gpu->v0.y  = (gpu->buf[1] >> 16) & 0x1ff;
                gpu->xsiz  = (((gpu->buf[2] & 0xffff) & 0x3ff) + 0x0f) & 0xfffffff0;
                gpu->ysiz  = (gpu->buf[2] >> 16) & 0x1ff;

                uint16_t color = BGR555(gpu->color);

                for (int y = 0; y < gpu->ysiz; y++) {
                    uint16_t* row = &gpu->vram[((gpu->v0.y + y) & 0x1ff) << 10];
                    for (int x = 0; x < gpu->xsiz; x++) {
                        row[(gpu->v0.x + x) & 0x3ff] = color;
                    }
                }

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

void gpu_cmd_80(psx_gpu_t* gpu) {
    switch (gpu->state) {
        case GPU_STATE_RECV_CMD: {
            gpu->state = GPU_STATE_RECV_ARGS;
            gpu->cmd_args_remaining = 3;
        } break;

        case GPU_STATE_RECV_ARGS: {
            if (!gpu->cmd_args_remaining) {
                uint32_t srcx = gpu->buf[1] & 0x3ff;
                uint32_t srcy = (gpu->buf[1] >> 16) & 0x1ff;
                uint32_t dstx = gpu->buf[2] & 0x3ff;
                uint32_t dsty = (gpu->buf[2] >> 16) & 0x1ff;
                uint32_t xsiz = (((gpu->buf[3] & 0xffff) - 1) & 0x3ff) + 1;
                uint32_t ysiz = (((gpu->buf[3] >> 16) - 1) & 0x1ff) + 1;

                for (uint32_t y = 0; y < ysiz; y++) {
                    uint16_t* src_row = &gpu->vram[((srcy + y) & 0x1ff) << 10];
                    uint16_t* dst_row = &gpu->vram[((dsty + y) & 0x1ff) << 10];
                    for (uint32_t x = 0; x < xsiz; x++) {
                        dst_row[(dstx + x) & 0x3ff] = src_row[(srcx + x) & 0x3ff];
                    }
                }

                gpu->state = GPU_STATE_RECV_CMD;
            }
        } break;
    }
}

/* 레거시 래퍼 함수들 (psx_gpu_update_cmd에서 type 1,2,3으로 이미 처리되지만 심볼 호환성 유지) */
void gpu_cmd_28(psx_gpu_t* gpu) { gpu_poly(gpu); }
void gpu_cmd_30(psx_gpu_t* gpu) { gpu_poly(gpu); }
void gpu_cmd_38(psx_gpu_t* gpu) { gpu_poly(gpu); }
void gpu_cmd_3c(psx_gpu_t* gpu) { gpu_poly(gpu); }
void gpu_cmd_2c(psx_gpu_t* gpu) { gpu_poly(gpu); }
void gpu_cmd_24(psx_gpu_t* gpu) { gpu_poly(gpu); }
void gpu_cmd_2d(psx_gpu_t* gpu) { gpu_poly(gpu); }
void gpu_cmd_64(psx_gpu_t* gpu) { gpu_rect(gpu); }
void gpu_cmd_7c(psx_gpu_t* gpu) { gpu_rect(gpu); }
void gpu_cmd_74(psx_gpu_t* gpu) { gpu_rect(gpu); }
void gpu_cmd_60(psx_gpu_t* gpu) { gpu_rect(gpu); }
void gpu_cmd_68(psx_gpu_t* gpu) { gpu_rect(gpu); }
void gpu_cmd_40(psx_gpu_t* gpu) { gpu_line(gpu); }

void psx_gpu_update_cmd(psx_gpu_t* gpu) {
    int type = (gpu->buf[0] >> 29) & 7;

    switch (type) {
        case 1: gpu_poly(gpu); return;
        case 2: gpu_line(gpu); return;
        case 3: gpu_rect(gpu); return;
    }

    switch (gpu->buf[0] >> 24) {
        case 0x00: /* nop */ break;
        case 0x01: /* Cache clear */ break;
        case 0x02: gpu_cmd_02(gpu); break;
        case 0x80: gpu_cmd_80(gpu); break;
        case 0xa0: gpu_cmd_a0(gpu); break;
        case 0xc0: gpu_cmd_c0(gpu); break;
        case 0xe1: {
            gpu->gpustat &= 0xfffff800;
            gpu->gpustat |= gpu->buf[0] & 0x7ff;
            gpu->texp_x = (gpu->gpustat & 0xf) << 6;
            gpu->texp_y = (gpu->gpustat & 0x10) << 4;
            gpu->texp_d = (gpu->gpustat >> 7) & 0x3;
        } break;
        case 0xe2: {
            gpu->texw_mx = ((gpu->buf[0] >> 0 ) & 0x1f) << 3;
            gpu->texw_my = ((gpu->buf[0] >> 5 ) & 0x1f) << 3;
            gpu->texw_ox = ((gpu->buf[0] >> 10) & 0x1f) << 3;
            gpu->texw_oy = ((gpu->buf[0] >> 15) & 0x1f) << 3;
        } break;
        case 0xe3: {
            gpu->draw_x1 = (gpu->buf[0] >> 0 ) & 0x3ff;
            gpu->draw_y1 = (gpu->buf[0] >> 10) & 0x1ff;
        } break;
        case 0xe4: {
            gpu->draw_x2 = (gpu->buf[0] >> 0 ) & 0x3ff;
            gpu->draw_y2 = (gpu->buf[0] >> 10) & 0x1ff;
        } break;
        case 0xe5: {
            gpu->off_x = ((int32_t)(((gpu->buf[0] >> 0 ) & 0x7ff) << 21)) >> 21;
            gpu->off_y = ((int32_t)(((gpu->buf[0] >> 11) & 0x7ff) << 21)) >> 21;
        } break;
        case 0xe6: {
            /* Mask bit setting */
        } break;
        default: break;
    }
}

void psx_gpu_write32(psx_gpu_t* gpu, uint32_t offset, uint32_t value) {
    switch (offset) {
        case 0x00: { // GP0
            switch (gpu->state) {
                case GPU_STATE_RECV_CMD: {
                    gpu->buf_index = 0;
                    gpu->buf[gpu->buf_index++] = value;
                    psx_gpu_update_cmd(gpu);
                } break;

                case GPU_STATE_RECV_ARGS: {
                    gpu->buf[gpu->buf_index++] = value;
                    gpu->cmd_args_remaining--;
                    psx_gpu_update_cmd(gpu);
                } break;

                case GPU_STATE_RECV_DATA: {
                    gpu->recv_data = value;
                    psx_gpu_update_cmd(gpu);
                } break;
            }
            return;
        }

        case 0x04: { // GP1
            uint8_t cmd = value >> 24;

            switch (cmd) {
                case 0x03:
                    gpu->gpustat &= ~0x00800000;
                    gpu->gpustat |= (value << 23) & 0x00800000;
                    break;
                case 0x04:
                    break;
                case 0x05:
                    gpu->disp_x = value & 0x3ff;
                    gpu->disp_y = (value >> 10) & 0x1ff;
                    break;
                case 0x06:
                    gpu->disp_x1 = value & 0xfff;
                    gpu->disp_x2 = (value >> 12) & 0xfff;
                    break;
                case 0x07:
                    gpu->disp_y1 = value & 0x1ff;
                    gpu->disp_y2 = (value >> 10) & 0x1ff;
                    break;
                case 0x08:
                    gpu->display_mode = value & 0xffffff;
                    if (gpu->event_cb_table[GPU_EVENT_DMODE])
                        gpu->event_cb_table[GPU_EVENT_DMODE](gpu);
                    break;
                case 0x10:
                    gpu->gp1_10h_req = value & 7;
                    break;
            }

            log_error("GP1(%02Xh) args=%06x", value >> 24, value & 0xffffff);
            return;
        }
    }

    log_warn("Unhandled 32-bit GPU write at offset %08x (%08x)", offset, value);
}

void psx_gpu_write16(psx_gpu_t* gpu, uint32_t offset, uint16_t value) {
    printf("Unhandled 16-bit GPU write at offset %08x (%04x)\n", offset, value);
}

void psx_gpu_write8(psx_gpu_t* gpu, uint32_t offset, uint8_t value) {
    printf("Unhandled 8-bit GPU write at offset %08x (%02x)\n", offset, value);
}

void psx_gpu_set_event_callback(psx_gpu_t* gpu, int event, psx_gpu_event_callback_t cb) {
    gpu->event_cb_table[event] = cb;
}

void psx_gpu_set_udata(psx_gpu_t* gpu, int index, void* udata) {
    gpu->udata[index] = udata;
}

#define GPU_CYCLES_PER_HDRAW_NTSC 2560.0f
#define GPU_CYCLES_PER_SCANL_NTSC 3413.0f
#define GPU_SCANS_PER_VDRAW_NTSC 240
#define GPU_SCANS_PER_FRAME_NTSC 263

void gpu_hblank_event(psx_gpu_t* gpu) {
    if (gpu->line < GPU_SCANS_PER_VDRAW_NTSC) {
        if (gpu->line & 1) {
            gpu->gpustat |= 1u << 31;
        } else {
            gpu->gpustat &= ~(1u << 31);
        }
    } else {
        gpu->gpustat &= ~(1u << 31);
    }

    gpu->line++;

    if (gpu->line == GPU_SCANS_PER_VDRAW_NTSC) {
        if (gpu->event_cb_table[GPU_EVENT_VBLANK])
            gpu->event_cb_table[GPU_EVENT_VBLANK](gpu);

        psx_ic_irq(gpu->ic, IC_VBLANK);
    } else if (gpu->line == GPU_SCANS_PER_FRAME_NTSC) {
        if (gpu->event_cb_table[GPU_EVENT_VBLANK_END])
            gpu->event_cb_table[GPU_EVENT_VBLANK_END](gpu);

        gpu->line = 0;
    }
}

void psx_gpu_update(psx_gpu_t* gpu, int cyc) {
    int prev_hblank = (gpu->cycles >= GPU_CYCLES_PER_HDRAW_NTSC) &&
                      (gpu->cycles <= GPU_CYCLES_PER_SCANL_NTSC);

    gpu->cycles += (float)cyc * (PSX_GPU_CLOCK_FREQ_NTSC / PSX_CPU_FREQ);

    int curr_hblank = (gpu->cycles >= GPU_CYCLES_PER_HDRAW_NTSC) &&
                      (gpu->cycles <= GPU_CYCLES_PER_SCANL_NTSC);

    if (curr_hblank && !prev_hblank) {
        if (gpu->event_cb_table[GPU_EVENT_HBLANK])
            gpu->event_cb_table[GPU_EVENT_HBLANK](gpu);

        gpu_hblank_event(gpu);
    } else if (prev_hblank && !curr_hblank) {
        if (gpu->event_cb_table[GPU_EVENT_HBLANK_END])
            gpu->event_cb_table[GPU_EVENT_HBLANK_END](gpu);

        gpu->cycles -= GPU_CYCLES_PER_SCANL_NTSC;
    }
}

void* psx_gpu_get_display_buffer(psx_gpu_t* gpu) {
    if (gpu->gpustat & 0x800000)
        return gpu->empty;

    return gpu->vram + (gpu->disp_x + (gpu->disp_y << 10));
}

void psx_gpu_destroy(psx_gpu_t* gpu) {
    free(gpu->vram);
    free(gpu->empty);
    free(gpu);
}