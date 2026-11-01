[BITS 16]
[ORG 0x7C00]

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x7C00

    ; 1. GDT 로드 및 보호 모드(32비트) 진입
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp 0x08:init_pm

; ----------------------------------------------------
; GDT (32비트 & 64비트 디스크립터 포함)
; ----------------------------------------------------
align 16
gdt_start:
    ; 0x00: Null Descriptor
    dq 0x0000000000000000

gdt_code32:
    ; 0x08: 32비트 코드 세그먼트 (D=1, L=0)
    dw 0xFFFF, 0x0000
    db 0x00, 10011010b, 11001111b, 0x00

gdt_data32:
    ; 0x10: 32비트 데이터 세그먼트
    dw 0xFFFF, 0x0000
    db 0x00, 10010010b, 11001111b, 0x00

gdt_code64:
    ; 0x18: 64비트 코드 세그먼트 (D=0, L=1)
    dw 0x0000, 0x0000
    db 0x00, 10011010b, 00100000b, 0x00

gdt_data64:
    ; 0x20: 64비트 데이터 세그먼트
    dw 0x0000, 0x0000
    db 0x00, 10010010b, 00000000b, 0x00
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

; ----------------------------------------------------
; 32비트 보호 모드: 페이징 구성 및 64비트 전환 준비
; ----------------------------------------------------
[BITS 32]
init_pm:
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, 0x7C00

    ; 1. 페이징 테이블 영역(0x1000 ~ 0x4000) 초기화 (0으로 클리어)
    mov edi, 0x1000
    mov cr3, edi                ; CR3 = PML4 베이스 주소 (0x1000)
    xor eax, eax
    mov ecx, 3072               ; 12KB 클리어
    rep stosd

    ; 2. 2MB Identity Mapping 구성
    ; PML4[0] (0x1000) -> PDPT (0x2000)
    mov dword [0x1000], 0x2003  ; Present + Writable
    ; PDPT[0] (0x2000) -> Page Directory (0x3000)
    mov dword [0x2000], 0x3003  ; Present + Writable
    ; Page Directory[0] (0x3000) -> 0x0 ~ 2MB Huge Page (PS bit set)
    mov dword [0x3000], 0x0083  ; Present + Writable + Page Size(2MB)

    ; 3. CR4.PAE 활성화 (Bit 5 = 1)
    mov eax, cr4
    or eax, (1 << 5)
    mov cr4, eax

    ; 4. IA32_EFER MSR에 LME(Long Mode Enable, bit 8) 세팅
    mov ecx, 0xC0000080         ; EFER MSR 번호
    rdmsr
    or eax, (1 << 8)            ; LME 비트
    wrmsr

    ; 5. CR0.PG(Paging Enable, bit 31) 켜기 -> 호환성 모드 활성화
    mov eax, cr0
    or eax, (1 << 31)
    mov cr0, eax

    ; 6. 64비트 코드 세그먼트(0x18)로 Far Jump -> 64비트 롱 모드 진입!
    jmp 0x18:init_long_mode

; ----------------------------------------------------
; 64비트 롱 모드 진입 지점
; ----------------------------------------------------
[BITS 64]
init_long_mode:
    ; 데이터 세그먼트 초기화
    mov ax, 0x20
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov fs, ax
    mov gs, ax
    mov rsp, 0x90000

    ; 시리얼 포트(0x3F8)로 64비트 모드 진입 메시지 출력
    mov rsi, msg_lm
    mov rdx, 0x03F8

print_loop_64:
    lodsb
    test al, al
    jz lm_done
    out dx, al
    jmp print_loop_64

lm_done:
    ; 64비트 범용 레지스터 동작 확인용 매직 넘버
    mov rax, 0x6464646464646464 ; "dddddddd" (64비트 레지스터 가득 채움)
    mov rbx, 0xDEADBEEFCAFEBABE
    hlt

hang_lm:
    jmp hang_lm

msg_lm:
    db 13, 10
    db "========================================", 13, 10
    db "  [Orbite] Entered 64-bit Long Mode!    ", 13, 10
    db "  [Orbite] Paging active, 64-bit CPU OK ", 13, 10
    db "========================================", 13, 10, 0
