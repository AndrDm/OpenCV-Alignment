 EUROASM DEBUG=OFF, PROFILE=OFF, AUTOSEGMENT=ON, CPU=X64, SIMD=AVX2, AMD=ENABLED
 AVX2Process PROGRAM FORMAT=DLL, Entry=DllEntryPoint, MODEL=FLAT, WIDTH=64, ICONFILE=

	U255 DY 32*BYTE 255

;x64 calling conventions:
;RAX	Return value register
;RCX	First integer argument
;RDX	Second integer argument
;R8	Third integer argument
;R9	Fourth integer argument
;RAX, RCX, RDX, R8-R11 are volatile. Must be preserved as needed by caller;
;RBX, RBP, RDI, RSI, RSP, R12-R15, and XMM6-XMM15 nonvolatile. They must be saved and restored by a function that uses them.

        align 16

EXPORT fnProcessU
fnProcessU PROC

	push rsi
	push rdi
	push rbx ;cpuid trashes EAX, EBX, ECX, EDX
        
	shr r8, 7;     ;divide by 128 (7 * 32 byte registers)
	vmovdqu ymm5, [U255]
	mov rsi, rdx   ;copy to non volatile regs	
	mov rdi, rcx

	cpuid ; force all previous instructions to complete and reset rax...rdx registerss!
	rdtsc ; read time stamp counter
	mov r10d, eax ; save EAX for later
	mov r11d, edx ; save EDX for later
LU:
        vmovdqu  ymm1, [rsi]
        vmovdqu  ymm2, [rsi+32]
        vmovdqu  ymm3, [rsi+64]
        vmovdqu  ymm4, [rsi+96]
	VPSUBB ymm1, ymm5, ymm1
	VPSUBB ymm2, ymm5, ymm2
	VPSUBB ymm3, ymm5, ymm3
	VPSUBB ymm4, ymm5, ymm4
        vmovdqu [rdi], ymm1
        vmovdqu [rdi+32], ymm2
        vmovdqu [rdi+64], ymm3
        vmovdqu [rdi+96], ymm4
        add rdi, 128
        add rsi, 128
        dec r8
        jnz LU

	vzeroupper	

	cpuid ; wait to complete before RDTSC
	rdtsc ; read time stamp counter
	sub eax, r10d ; subtract the most recent CPU ticks from the original CPU ticks
	sbb edx, r11d ; now, subtract with borrow
        shl rax, 32
	shrd rax, rdx, 32

	pop rbx
	pop rdi
	pop rsi

        RET
ENDP fnProcessU


        align 16
EXPORT fnProcessA ;aligned processing
fnProcessA PROC
	push rsi
	push rdi
	push rbx

        shr r8, 7
	vmovdqu ymm5, [U255]
	mov rsi, rdx
	mov rdi, rcx

	cpuid ; force all previous instructions to complete and reset rax...rdx registerss!
	rdtsc ; read time stamp counter
	mov r10d, eax ; save EAX for later
	mov r11d, edx ; save EDX for later
LA:
        vmovdqa  ymm1, [rsi]
        vmovdqa  ymm2, [rsi+32*1]
        vmovdqa  ymm3, [rsi+32*2]
        vmovdqa  ymm4, [rsi+32*3]
	VPSUBB ymm1, ymm5, ymm1
	VPSUBB ymm2, ymm5, ymm2
	VPSUBB ymm3, ymm5, ymm3
	VPSUBB ymm4, ymm5, ymm4
        vmovntdq [rdi], ymm1     
        vmovntdq [rdi+32*1], ymm2
        vmovntdq [rdi+32*2], ymm3
        vmovntdq [rdi+32*3], ymm4
        add rdi, 128
        add rsi, 128
        dec r8
        jnz LA
	
	vzeroupper	

	cpuid ; wait for FDIV to complete before RDTSC
	rdtsc ; read time stamp counter
	sub eax, r10d ; subtract the most recent CPU ticks from the original CPU ticks
	sbb edx, r11d ; now, subtract with borrow
        shl rax, 32
	shrd rax, rdx, 32

	pop rbx
	pop rdi
	pop rsi

        RET
ENDP fnProcessA

DllEntryPoint PROC                      
	mov rax, 1
	ret
ENDPROC DllEntryPoint

ENDPROGRAM AVX2Process
