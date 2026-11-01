#include "mdec.h"
#include "../log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

int zigzag[] = {
    0 , 1 , 5 , 6 , 14, 15, 27, 28,
    2 , 4 , 7 , 13, 16, 26, 29, 42,
    3 , 8 , 12, 17, 25, 30, 41, 43,
    9 , 11, 18, 24, 31, 40, 44, 53,
    10, 19, 23, 32, 39, 45, 52, 54,
    20, 22, 33, 38, 46, 51, 55, 60,
    21, 34, 37, 47, 50, 56, 59, 61,
    35, 36, 48, 49, 57, 58, 62, 63
};

int zagzig[] = {
     0,  1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

float scalezag[] = {
    0.125000, 0.173380, 0.173380, 0.163320, 0.240485, 0.163320, 0.146984, 0.226532,
    0.226532, 0.146984, 0.125000, 0.203873, 0.213388, 0.203873, 0.125000, 0.098212,
    0.173380, 0.192044, 0.192044, 0.173380, 0.098212, 0.067650, 0.136224, 0.163320,
    0.172835, 0.163320, 0.136224, 0.067650, 0.034487, 0.093833, 0.128320, 0.146984,
    0.146984, 0.128320, 0.093833, 0.034487, 0.047835, 0.088388, 0.115485, 0.125000,
    0.115485, 0.088388, 0.047835, 0.045060, 0.079547, 0.098212, 0.098212, 0.079547,
    0.045060, 0.040553, 0.067650, 0.077165, 0.067650, 0.040553, 0.034487, 0.053152,
    0.053152, 0.034487, 0.027097, 0.036612, 0.027097, 0.018664, 0.018664, 0.009515
};

#define EXTS10(v) (((int16_t)((v) << 6)) >> 6)
#define CLAMP(v, l, h) ((v <= l) ? l : ((v >= h) ? h : v))
#define MAX(a, b) (a > b ? a : b)

void real_idct(int16_t* blk, int16_t* scale) {
    int16_t buf[64];

    int16_t* src = blk;
    int16_t* dst = buf;

    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++) {
            for (int y = 0; y < 8; y++) {
                int sum = 0;

                for (int z = 0; z < 8; z++)
                    sum += (int32_t)src[y + z * 8] * ((int32_t)scale[x + z * 8] / 8);

                dst[x + y * 8] = (sum + 0xfff) / 0x2000;
            }
        }

        int16_t* temp = src;

        src = dst;
        dst = temp;
    }
}

#define IDCT_FUNC(blk, scale) real_idct(blk, scale)

// [수정] src_end 포인터를 함께 전달받아 입력 버퍼를 넘어서는 메모리 침범(Access Violation) 원천 차단!
uint16_t* rl_decode_block(int16_t* blk, uint16_t* src, uint16_t* src_end, uint8_t* quant, int16_t* scale) {
    int k = 0;

    for (int i = 0; i < 64; i++)
        blk[i] = 0;

    if (src >= src_end) return src;

    uint16_t n = *src++;

    // 블록 시작 전 패딩(0xfe00) 건너뛰기 (버퍼 끝 체크 포함)
    while (n == 0xfe00) {
        if (src >= src_end) return src;
        n = *src++;
    }

    int q_scale = (n >> 10) & 0x3f;
    int16_t val = EXTS10(n & 0x3ff) * quant[k];

    while (k < 64) {
        if (!q_scale)
            val = EXTS10(n & 0x3ff) * 2;

        val = CLAMP(val, -0x400, 0x3ff);

        if (q_scale > 0)
            blk[zagzig[k]] = val;
        else
            blk[k] = val;

        if (k == 63 || src >= src_end)
            break;

        n = *src++;

        // [핵심 수정 1] 0xfe00은 EOB(End Of Block) 마커이므로 즉시 현재 블록 디코딩을 종료해야 함!
        if (n == 0xfe00)
            break;

        k += ((n >> 10) & 0x3f) + 1;

        // [핵심 수정 2] k가 64 이상인지 먼저 검사한 뒤에 quant[k] 배열에 접근해야 오버플로우가 나지 않음!
        if (k >= 64)
            break;

        val = (EXTS10(n & 0x3ff) * quant[k] * q_scale + 4) / 8;
    }

    IDCT_FUNC(blk, scale);

    return src;
}

void yuv_to_rgb(psx_mdec_t* mdec, uint8_t* buf, int xx, int yy) {
    for (int y = 0; y < 8; y++) {
        for (int x = 0; x < 8; x++) {
            int16_t r = mdec->crblk[((x + xx) >> 1) + ((y + yy) >> 1) * 8];
            int16_t b = mdec->cbblk[((x + xx) >> 1) + ((y + yy) >> 1) * 8];
            int16_t g = (-0.3437f * (float)b) + (-0.7143f * (float)r);

            r = (1.402f * (float)r);
            b = (1.772f * (float)b);

            int16_t l = mdec->yblk[x + y * 8];

            r = CLAMP(l + r, -128, 127);
            g = CLAMP(l + g, -128, 127);
            b = CLAMP(l + b, -128, 127);

            if (!mdec->output_signed) {
                r ^= 0x80;
                g ^= 0x80;
                b ^= 0x80;
            }

            if (mdec->output_depth == 3) {
                uint16_t r5 = ((uint8_t)r) >> 3;
                uint16_t g5 = ((uint8_t)g) >> 3;
                uint16_t b5 = ((uint8_t)b) >> 3;

                uint16_t rgb = (b5 << 10) | (g5 << 5) | r5;

                if (mdec->output_bit15)
                    rgb |= 0x8000;

                buf[0 + ((x + xx) + (y + yy) * 16) * 2] = rgb & 0xff;
                buf[1 + ((x + xx) + (y + yy) * 16) * 2] = rgb >> 8;
            }
            else {
                buf[0 + ((x + xx) + (y + yy) * 16) * 3] = r & 0xff;
                buf[1 + ((x + xx) + (y + yy) * 16) * 3] = g & 0xff;
                buf[2 + ((x + xx) + (y + yy) * 16) * 3] = b & 0xff;
            }
        }
    }
}

void mdec_nop(psx_mdec_t* mdec) { /* Do nothing */ }

void mdec_decode_macroblock(psx_mdec_t* mdec) {
    if (!mdec->input || mdec->input_size == 0) return;

    uint16_t* in = (uint16_t*)mdec->input;
    uint16_t* in_end = (uint16_t*)((uint8_t*)mdec->input + mdec->input_size);

    // 이전 프레임의 출력 버퍼가 남아있다면 안전하게 정리 후 새로 할당
    if (mdec->output) {
        free(mdec->output);
        mdec->output = NULL;
    }

    if (mdec->output_depth < 2) {
        size_t block_size = (mdec->output_depth == 3) ? 512 : 768;
        size_t size = block_size;

        mdec->output = calloc(1, size);
        if (!mdec->output) return;

        rl_decode_block(mdec->yblk, in, in_end, mdec->y_quant_table, mdec->scale_table);

        for (int i = 0; i < 64; i++) {
            int16_t y = mdec->yblk[i] & 0xff;

            if (mdec->output_depth == 1) {
                mdec->output[i] = y;
            }
            else {
                mdec->output[i] = 0;
            }
        }

        mdec->output_words_remaining = ((mdec->output_depth == 1) ? 64 : 32) >> 2;
        mdec->output_empty = 0;
        mdec->output_index = 0;
    }
    else {
        size_t block_size = (mdec->output_depth == 3) ? 512 : 768;
        size_t size = block_size;

        int block_count = 1;

        while (in < in_end) {
            // 패딩(0xfe00)만 남아있는 경우 불필요한 빈 블록 디코딩을 방지하고 종료
            while (in < in_end && *in == 0xfe00) {
                in++;
            }
            if (in >= in_end) break;

            uint8_t* new_out = realloc(mdec->output, block_count * size);
            if (!new_out) break;
            mdec->output = new_out;

            in = rl_decode_block(mdec->crblk, in, in_end, mdec->uv_quant_table, mdec->scale_table);
            in = rl_decode_block(mdec->cbblk, in, in_end, mdec->uv_quant_table, mdec->scale_table);
            in = rl_decode_block(mdec->yblk, in, in_end, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 0, 0);
            in = rl_decode_block(mdec->yblk, in, in_end, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 8, 0);
            in = rl_decode_block(mdec->yblk, in, in_end, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 0, 8);
            in = rl_decode_block(mdec->yblk, in, in_end, mdec->y_quant_table, mdec->scale_table);
            yuv_to_rgb(mdec, &mdec->output[(block_count * size) - block_size], 8, 8);

            ++block_count;
        }

        mdec->output_words_remaining = ((block_count - 1) * block_size) >> 2;
        mdec->output_empty = (mdec->output_words_remaining == 0) ? 1 : 0;
        mdec->output_index = 0;
    }
}

void mdec_set_iqtab(psx_mdec_t* mdec) {
    if (!mdec->input) return;

    memcpy(mdec->y_quant_table, mdec->input, 64);

    if (mdec->recv_color && mdec->input_size >= 128)
        memcpy(mdec->uv_quant_table, &mdec->input[16], 64);
}

void mdec_set_scale(psx_mdec_t* mdec) {
    if (!mdec->input || mdec->input_size < 128) return;
    memcpy(mdec->scale_table, mdec->input, 128);
}

mdec_fn_t g_mdec_cmd_table[] = {
    mdec_nop,
    mdec_decode_macroblock,
    mdec_set_iqtab,
    mdec_set_scale,
    mdec_nop,
    mdec_nop,
    mdec_nop,
    mdec_nop
};

psx_mdec_t* psx_mdec_create(void) {
    return (psx_mdec_t*)malloc(sizeof(psx_mdec_t));
}

void psx_mdec_init(psx_mdec_t* mdec) {
    memset(mdec, 0, sizeof(psx_mdec_t));

    mdec->io_base = PSX_MDEC_BEGIN;
    mdec->io_size = PSX_MDEC_SIZE;

    mdec->state = MDEC_RECV_CMD;
}

uint32_t psx_mdec_read32(psx_mdec_t* mdec, uint32_t offset) {
    switch (offset) {
    case 0: {
        if (mdec->output && mdec->output_words_remaining) {
            --mdec->output_words_remaining;
            return ((uint32_t*)mdec->output)[mdec->output_index++];
        }
        else {
            mdec->output_empty = 1;
            mdec->output_index = 0;
            mdec->output_request = 0;
            return 0xaaaaaaaa;
        }
    } break;
    case 4: {
        uint32_t status = 0;

        status |= mdec->words_remaining;
        status |= mdec->current_block << 16;
        status |= mdec->output_bit15 << 23;
        status |= mdec->output_signed << 24;
        status |= mdec->output_depth << 25;
        status |= mdec->output_request << 27;
        status |= mdec->input_request << 28;
        status |= mdec->busy << 29;
        status |= mdec->input_full << 30;
        status |= mdec->output_empty << 31;

        return status;
    } break;
    }

    return 0x0;
}

// [핵심 수정 3] exit(1) 강제 종료를 제거하고 32비트 읽기 결과에서 추출하여 반환하도록 수정
uint16_t psx_mdec_read16(psx_mdec_t* mdec, uint32_t offset) {
    uint32_t val = psx_mdec_read32(mdec, offset & ~3);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

uint8_t psx_mdec_read8(psx_mdec_t* mdec, uint32_t offset) {
    uint32_t val = psx_mdec_read32(mdec, offset & ~3);
    return (uint8_t)(val >> ((offset & 3) * 8));
}

void psx_mdec_write32(psx_mdec_t* mdec, uint32_t offset, uint32_t value) {
    switch (offset) {
    case 0: {
        if (mdec->words_remaining) {
            if (mdec->input) {
                mdec->input[mdec->input_index++] = value;
            }

            --mdec->words_remaining;

            if (!mdec->words_remaining) {
                mdec->output_empty = 0;
                mdec->input_full = 1;
                mdec->input_request = 0;
                mdec->busy = 0;
                mdec->output_request = mdec->enable_dma1;

                g_mdec_cmd_table[mdec->cmd >> 29](mdec);

                if (mdec->input) {
                    free(mdec->input);
                    mdec->input = NULL; // [필수] NULL 초기화로 Double Free 방지!
                }
            }

            break;
        }

        mdec->cmd = value;
        mdec->output_request = 0;
        mdec->output_empty = 1;
        mdec->output_bit15 = (value >> 25) & 1;
        mdec->output_signed = (value >> 26) & 1;
        mdec->output_depth = (value >> 27) & 3;
        mdec->input_index = 0;
        mdec->input_full = 0;
        mdec->busy = 1;

        switch (mdec->cmd >> 29) {
        case MDEC_CMD_NOP: {
            mdec->busy = 0;
            mdec->words_remaining = 0;
        } break;

        case MDEC_CMD_DECODE: {
            mdec->words_remaining = mdec->cmd & 0xffff;
        } break;

        case MDEC_CMD_SET_QT: {
            mdec->recv_color = mdec->cmd & 1;
            mdec->words_remaining = mdec->recv_color ? 32 : 16;
        } break;

        case MDEC_CMD_SET_ST: {
            mdec->words_remaining = 32;
        } break;

        default: {
            mdec->busy = 0;
            mdec->words_remaining = 0;
        } break;
        }

        if (mdec->words_remaining) {
            mdec->input_request = 1;
            mdec->input_size = mdec->words_remaining * sizeof(uint32_t);
            mdec->input_full = 0;
            mdec->input_index = 0;

            if (mdec->input) {
                free(mdec->input);
            }
            mdec->input = calloc(1, mdec->input_size);
        }
    } break;

    case 4: {
        mdec->enable_dma0 = (value & 0x40000000) != 0;
        mdec->enable_dma1 = (value & 0x20000000) != 0;

        // [핵심 수정 4] 동영상 스킵/종료 시 MDEC Reset(0x80000000)이 들어오면 진행 중이던 버퍼도 안전하게 정리
        if (value & 0x80000000) {
            if (mdec->input) {
                free(mdec->input);
                mdec->input = NULL;
            }
            mdec->input_size = 0;
            mdec->input_index = 0;
            mdec->busy = 0;
            mdec->words_remaining = 0;
            mdec->output_bit15 = 0;
            mdec->output_signed = 0;
            mdec->output_depth = 0;
            mdec->input_request = 0;
            mdec->output_request = 0;
            mdec->input_full = 0;
            mdec->output_empty = 1;
            mdec->current_block = 4;
        }
    } break;
    }
}

void psx_mdec_write16(psx_mdec_t* mdec, uint32_t offset, uint16_t value) {
    // 16비트 쓰기 무시 또는 하위 전달
}

void psx_mdec_write8(psx_mdec_t* mdec, uint32_t offset, uint8_t value) {
    // 8비트 쓰기 무시
}

void psx_mdec_destroy(psx_mdec_t* mdec) {
    if (!mdec) return;
    if (mdec->input) free(mdec->input);
    if (mdec->output) free(mdec->output);
    free(mdec);
}

#undef CLAMP
#undef MAX