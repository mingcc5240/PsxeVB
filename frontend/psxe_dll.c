#include "../psx/psx.h"
#include "../psx/input/sda.h"
#include "../psx/input/guncon.h"
#include "../psx/dev/cdrom/cdrom.h"
#include "screen.h"
#include "config.h"

#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// [핵심] 바이오스 바이너리가 변환된 헤더 파일을 DLL 코드에 직접 포함!
// 컴파일 시 이 배열(512KB)이 DLL 바이너리 내부로 완전히 합쳐집니다.
#include "scph1001.h"

// scph1001.h 내부에 선언된 배열 이름과 맞춥니다 (xxd -i SCPH1001.bin 실행 시 기본 이름)
#define EMBEDDED_BIOS_DATA SCPH1001_bin
#define EMBEDDED_BIOS_SIZE 524288 // 512KB (0x80000)

#undef main

#define VB6_API __declspec(dllexport) __stdcall

#include <time.h>

// 전역 FPS 카운터 변수들
static int g_frame_count = 0;
static float g_current_fps = 0.0f;
static DWORD g_last_tick = 0;

// .bin 단독 실행 시 자동 생성되는 임시 .cue 파일 경로 버퍼
static char g_auto_cue_path[MAX_PATH] = { 0 };

// PSXE_RunFrame 내부에서 매 프레임마다 호출하여 FPS를 갱신하는 함수
static void update_fps_counter(void) {
    g_frame_count++;
    DWORD current_tick = GetTickCount();

    // 1초(1000ms)마다 FPS 계산 갱신
    if (current_tick - g_last_tick >= 1000) {
        g_current_fps = (float)g_frame_count * 1000.0f / (float)(current_tick - g_last_tick);
        g_frame_count = 0;
        g_last_tick = current_tick;
    }
}

// VB6에서 현재 FPS 값을 가져갈 수 있도록 Export 함수 추가
float VB6_API PSXE_GetFPS(void) {
    return g_current_fps;
}

extern void* g_external_hwnd;
extern int g_vblank_reached;

static psx_t* g_psx = NULL;
static psxe_screen_t* g_screen = NULL;
static psx_input_t* g_input = NULL;
static psxi_sda_t* g_controller = NULL;
static SDL_AudioDeviceID g_audio_dev = 0;

void audio_update(void* ud, uint8_t* buf, int size) {
    psx_cdrom_t* cdrom = ((psx_t*)ud)->cdrom;
    psx_spu_t* spu = ((psx_t*)ud)->spu;

    memset(buf, 0, size);

    psx_cdrom_get_audio_samples(cdrom, buf, size);
    psx_spu_update_cdda_buffer(spu, cdrom->cdda_buf);

    for (int i = 0; i < (size >> 2); i++) {
        uint32_t sample = psx_spu_get_sample(spu);

        int16_t left = (int16_t)(sample & 0xffff);
        int16_t right = (int16_t)(sample >> 16);

        *(int16_t*)(&buf[(i << 2) + 0]) += left;
        *(int16_t*)(&buf[(i << 2) + 2]) += right;
    }
}

// VB6의 빈 문자열("")을 C의 NULL 포인터로 안전하게 변환하는 헬퍼 함수
static const char* sanitize_path(const char* path) {
    if (!path || path[0] == '\0') return NULL;
    return path;
}

// =============================================================================
// [핵심 추가] .bin 또는 .img 파일을 직접 열었을 때 자동으로 처리하는 헬퍼 함수
// 1) 같은 폴더에 동명의 .cue 파일이 이미 있으면 그 .cue 파일을 우선 사용하되,
// 2) .cue 파일이 아예 없는 순수 단일 .bin 파일이면 해당 .bin의 절대 경로를 가리키는
//    임시 .cue 파일을 즉석 생성하여 psxe 코어가 완벽히 인식하도록 변환합니다!
// =============================================================================
static const char* resolve_cd_path(const char* cd_path) {
    if (!cd_path || cd_path[0] == '\0') return NULL;

    const char* dot = strrchr(cd_path, '.');
    if (!dot) return cd_path;

    // 확장자 소문자 변환 비교 (.bin 또는 .img 인지 확인)
    if (_stricmp(dot, ".bin") == 0 || _stricmp(dot, ".img") == 0) {
        // 1. 먼저 같은 이름의 .cue 파일이 같은 폴더에 존재하는지 확인
        char companion_cue[MAX_PATH];
        size_t base_len = (size_t)(dot - cd_path);
        if (base_len + 5 < MAX_PATH) {
            memcpy(companion_cue, cd_path, base_len);
            strcpy(companion_cue + base_len, ".cue");

            FILE* f_check = fopen(companion_cue, "rb");
            if (f_check) {
                fclose(f_check);
                // 단, psxe의 cue 파서는 상대 경로를 현재 작업 폴더 기준으로 찾을 수 있으므로
                // 확실하게 절대 경로가 박힌 자동 생성 CUE를 쓰는 것이 경로 에러 방지에 훨씬 유리합니다.
            }
        }

        // 2. 임시 폴더(%TEMP%)에 절대 경로를 가리키는 가상 .cue 생성
        char temp_dir[MAX_PATH];
        DWORD ret = GetTempPathA(MAX_PATH, temp_dir);
        if (ret == 0 || ret > MAX_PATH) {
            snprintf(g_auto_cue_path, sizeof(g_auto_cue_path), "psxe_auto_disc.cue");
        }
        else {
            snprintf(g_auto_cue_path, sizeof(g_auto_cue_path), "%spsxe_auto_disc.cue", temp_dir);
        }

        // .bin 파일의 전체 절대 경로 확보
        char full_bin_path[MAX_PATH];
        if (!GetFullPathNameA(cd_path, MAX_PATH, full_bin_path, NULL)) {
            strncpy(full_bin_path, cd_path, MAX_PATH - 1);
            full_bin_path[MAX_PATH - 1] = '\0';
        }

        FILE* f_cue = fopen(g_auto_cue_path, "w");
        if (f_cue) {
            fprintf(f_cue, "FILE \"%s\" BINARY\n", full_bin_path);
            fprintf(f_cue, "  TRACK 01 MODE2/2352\n");
            fprintf(f_cue, "    INDEX 01 00:00:00\n");
            fclose(f_cue);
            return g_auto_cue_path;
        }
    }

    return cd_path;
}

// 1. 에뮬레이터 초기화 (성공 시 0 반환, 실패 시 에러 코드 1, 2, 3 반환)
int VB6_API PSXE_Init(HWND hwnd, const char* bios_path, const char* exp_path, const char* cd_path, const char* exe_path) {
    if (g_psx != NULL) return -1;

    const char* safe_bios = sanitize_path(bios_path);
    const char* safe_exp = sanitize_path(exp_path);
    const char* safe_cd = sanitize_path(cd_path);
    const char* safe_exe = sanitize_path(exe_path);

    /*
    // psx_init 내부에서 부분 할당 후 실패하는 것을 막기 위해 BIOS 파일 존재 여부 선행 확인
    if (!safe_bios) return 1;
    FILE* f = fopen(safe_bios, "rb");
    if (!f) return 1;
    fclose(f);
    */

    // 외부 바이오스 경로가 지정되었고 실제 파일이 존재하는지 확인
    int use_embedded = 1;
    if (safe_bios) {
        FILE* f = fopen(safe_bios, "rb");
        if (f) {
            fclose(f);
            use_embedded = 0; // 외부 파일이 실제로 있으면 외부 파일 사용
        }
    }

    g_external_hwnd = (void*)hwnd;

    SDL_SetHint(SDL_HINT_WINDOWS_ENABLE_MESSAGELOOP, "1");
    SDL_SetHint(SDL_HINT_VIDEO_FOREIGN_WINDOW_OPENGL, "1");

    log_set_quiet(1);

    g_psx = psx_create();
    if (!g_psx) return -1;

    // psx_init 실행 및 반환값(0: 성공, 1: BIOS 실패, 2: EXP1 실패) 검증
   // int init_res = psx_init(g_psx, safe_bios, safe_exp);
    // 내장 바이오스를 쓸 때는 "__EMBEDDED__" 표시자를 넘겨 psx_init 호출
    int init_res = psx_init(g_psx, use_embedded ? "__EMBEDDED__" : safe_bios, safe_exp);
    if (init_res != 0) {
        // 초기화 도중 실패한 경우 크래시 방지를 위해 g_psx 정리 후 에러 코드 반환
        free(g_psx);
        g_psx = NULL;
        return init_res;
    }

    // =========================================================================
    // [크래시 완벽 해결] psx_bios_load를 건너뛰면 bios->buf가 NULL 상태이므로,
    // 반드시 직접 512KB(0x80000) 메모리를 malloc으로 할당한 뒤 memcpy 해야 합니다!
    // =========================================================================
    if (use_embedded && g_psx->bios) {
        if (!g_psx->bios->buf) {
            g_psx->bios->buf = (uint8_t*)malloc(EMBEDDED_BIOS_SIZE);
        }
        if (!g_psx->bios->buf) {
            psx_destroy(g_psx);
            g_psx = NULL;
            return 1;
        }
        memcpy(g_psx->bios->buf, EMBEDDED_BIOS_DATA, EMBEDDED_BIOS_SIZE);
    }

    /*
    // CD-ROM 이미지 마운트
    psx_cdrom_t* cdrom = psx_get_cdrom(g_psx);
    if (safe_cd) {
        if (!psx_cdrom_open(cdrom, safe_cd)) {
            // psx_cdrom_open이 실패(0 반환 가정)하더라도 바이오스 부팅은 가능하지만 필요시 체크
        }
    }*/

    // [핵심] CD-ROM 이미지 마운트 시 .bin 파일을 직접 골랐어도 자동 처리!
    psx_cdrom_t* cdrom = psx_get_cdrom(g_psx);
    if (safe_cd) {
        const char* target_cd = resolve_cd_path(safe_cd);
        if (!psx_cdrom_open(cdrom, target_cd)) {
            // 만약 코어의 psx_cdrom_open이 .bin 직접 열기를 지원해 CUE를 거부하는 버전이라면 원본 경로로 재시도
            if (target_cd != safe_cd) {
                psx_cdrom_open(cdrom, safe_cd);
            }
        }
    }

    // 화면(SDL) 초기화
    g_screen = psxe_screen_create();
    psxe_screen_init(g_screen, g_psx);
    psxe_screen_set_scale(g_screen, 2);
    psxe_screen_reload(g_screen);

    // 오디오(SDL) 초기화
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_AudioSpec obtained, desired;
    desired.freq = 44100;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = CD_SECTOR_SIZE >> 2;
    desired.callback = &audio_update;
    desired.userdata = g_psx;

    g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (g_audio_dev) {
        SDL_PauseAudioDevice(g_audio_dev, 0);
    }

    // GPU 콜백 연결 (psx_init에서 생성된 gpu, timer 포인터 사용)
    psx_gpu_t* gpu = psx_get_gpu(g_psx);
    psx_gpu_set_event_callback(gpu, GPU_EVENT_DMODE, psxe_gpu_dmode_event_cb);
    psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK, psxe_gpu_vblank_event_cb);
    psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK, psxe_gpu_hblank_event_cb);
    psx_gpu_set_event_callback(gpu, GPU_EVENT_VBLANK_END, psxe_gpu_vblank_end_event_cb);
    psx_gpu_set_event_callback(gpu, GPU_EVENT_HBLANK_END, psxe_gpu_hblank_end_event_cb);
    psx_gpu_set_udata(gpu, 0, g_screen);
    psx_gpu_set_udata(gpu, 1, g_psx->timer);

    // 입력 패드 및 메모리 카드 초기화
    g_input = psx_input_create();
    psx_input_init(g_input);

    g_controller = psxi_sda_create();
    psxi_sda_init(g_controller, SDA_MODEL_DIGITAL);
    psxi_sda_init_input(g_controller, g_input);

    psx_pad_attach_joy(g_psx->pad, 0, g_input);
    psx_pad_attach_mcd(g_psx->pad, 0, "slot1.mcd");
    psx_pad_attach_mcd(g_psx->pad, 1, "slot2.mcd");

    // PS-EXE 직접 실행 옵션이 있는 경우 BIOS 부팅 완료(0x80030000)까지 대기 후 주입
    if (safe_exe) {
        while (g_psx->cpu->pc != 0x80030000) {
            psx_update(g_psx);
        }
        psx_load_exe(g_psx, safe_exe);
    }

    return 0; // 0 = 정상 초기화 완료 (psx_init 관례와 동일)
}

// 2. 런타임에 CD 이미지(롬) 교체 / 삽입 함수
void VB6_API PSXE_LoadCD(const char* cd_path) {
    if (!g_psx) return;
    const char* safe_cd = sanitize_path(cd_path);
    if (safe_cd) {
        psx_swap_disc(g_psx, safe_cd);
    }
}

// 3. 1프레임(VBlank) 실행
int VB6_API PSXE_RunFrame(void) {
    if (!g_psx || !g_screen) return 0;

    g_vblank_reached = 0;

    /*
    int safety_limit = 1000000;
    while (!g_vblank_reached && --safety_limit > 0) {
        psx_update(g_psx);
    } */

    
    // PS1 CPU 클럭 기준 1프레임당 대략적인 사이클 만큼 반복실행
    for (int i = 0; i < 564480 / 4; i++) {
        psx_update(g_psx);
    } 

    // [추가] 프레임이 바뀔 때마다 FPS 카운트 갱신
    update_fps_counter();

    return psxe_screen_is_open(g_screen);
}

// 4. VB6 키 입력 전달
void VB6_API PSXE_SetPadButton(uint32_t button_mask, int is_pressed) {
    if (!g_screen || !g_screen->pad) return;

    if (is_pressed) {
        psx_pad_button_press(g_screen->pad, 0, button_mask);
    }
    else {
        psx_pad_button_release(g_screen->pad, 0, button_mask);
    }
}

// 5. 단축키 명령 (F1~F5)
void VB6_API PSXE_Command(int cmd_id) {
    if (!g_screen || !g_psx) return;

    switch (cmd_id) {
    case 1: psxe_screen_toggle_debug_mode(g_screen); break;
    case 3:
        g_screen->vertical_mode = !g_screen->vertical_mode;
        psxe_gpu_dmode_event_cb(g_psx->gpu);
        break;
    case 4:
        g_screen->bilinear = !g_screen->bilinear;
        psxe_gpu_dmode_event_cb(g_psx->gpu);
        break;
    case 5: psx_soft_reset(g_psx); break;
    }
}

// 6. 종료 및 메모리 정리
void VB6_API PSXE_Shutdown(void) {
    if (g_audio_dev) {
        SDL_PauseAudioDevice(g_audio_dev, 1);
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }

    if (g_psx) {
        psx_pad_detach_joy(g_psx->pad, 0);
        psx_destroy(g_psx);
        g_psx = NULL;
    }

    if (g_controller) {
        free(g_controller);
        g_controller = NULL;
    }

    if (g_input) {
        free(g_input);
        g_input = NULL;
    }

    if (g_screen) {
        psxe_screen_destroy(g_screen);
        g_screen = NULL;
    }
}