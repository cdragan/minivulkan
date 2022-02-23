// SPDX-License-Identifier: MIT
// Copyright (c) 2021-2022 Chris Dragan

#include "mstdc.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {

    int _fltused;

#   pragma function(memset)
    void* memset(void* dest_ptr, int c, size_t num_bytes)
    {
        assert(dest_ptr);
        assert(num_bytes);

        uint8_t* dest_byte = static_cast<uint8_t*>(dest_ptr);

        while (num_bytes--)
            *(dest_byte++) = static_cast<uint8_t>(c);

        return dest_ptr;
    }

#   pragma function(memcpy)
    void* memcpy(void* dest_ptr, const void* src, size_t num_bytes)
    {
        mstd::mem_copy(dest_ptr, src, static_cast<uint32_t>(num_bytes));
        return dest_ptr;
    }

    int _atexit(void (__cdecl *func )(void))
    {
        return 0;
    }

#ifndef NDEBUG
    void _wassert(const wchar_t* message, const wchar_t* filename, unsigned line)
    {
        OutputDebugStringW(message);
        DebugBreak();
    }

    int printf(const char* format, ...)
    {
        va_list args;
        va_start(args, format);

        char buf[1024];
        const int num = wvsprintf(buf, format, args);

        va_end(args);

        OutputDebugString(buf);

        return num;
    }
#endif

// The code below is used in place of MS Visual C Runtime library in 32-bit builds.
// In 32-bit builds the compiler calls these functions in order to perform 64-bit arithmetic.
// This code has been copied from GitHub mmozeiko/win32_crt_float.cpp
#ifdef _M_IX86
#   define CRT_LOWORD(x) dword ptr [x+0]
#   define CRT_HIWORD(x) dword ptr [x+4]

    __declspec(naked) void _allmul()
    {
        #define A esp + 8  // stack address of a
        #define B esp + 16 // stack address of b

        __asm
        {
            push    ebx

            mov     eax, CRT_HIWORD(A)
            mov     ecx, CRT_LOWORD(B)
            mul     ecx                 ; eax has AHI, ecx has BLO, so AHI * BLO
            mov     ebx, eax            ; save result

            mov     eax, CRT_LOWORD(A)
            mul     CRT_HIWORD(B)       ; ALO * BHI
            add     ebx, eax            ; ebx = ((ALO * BHI) + (AHI * BLO))

            mov     eax, CRT_LOWORD(A)  ; ecx = BLO
            mul     ecx                 ; so edx:eax = ALO*BLO
            add     edx, ebx            ; now edx has all the LO*HI stuff

            pop     ebx

            ret     16                  ; callee restores the stack
        }

        #undef A
        #undef B
    }

   __declspec(naked) void _aulldiv()
    {
        #define DVND esp + 12 // stack address of dividend (a)
        #define DVSR esp + 20 // stack address of divisor (b)

        __asm
        {
            push    ebx
            push    esi

            ;
            ; Now do the divide.  First look to see if the divisor is less than 4194304K.
            ; If so, then we can use a simple algorithm with word divides, otherwise
            ; things get a little more complex.
            ;

            mov     eax, CRT_HIWORD(DVSR) ; check to see if divisor < 4194304K
            or      eax, eax
            jnz     short L1        ; nope, gotta do this the hard way
            mov     ecx, CRT_LOWORD(DVSR) ; load divisor
            mov     eax, CRT_HIWORD(DVND) ; load high word of dividend
            xor     edx, edx
            div     ecx             ; get high order bits of quotient
            mov     ebx, eax        ; save high bits of quotient
            mov     eax, CRT_LOWORD(DVND) ; edx:eax <- remainder:lo word of dividend
            div     ecx             ; get low order bits of quotient
            mov     edx, ebx        ; edx:eax <- quotient hi:quotient lo
            jmp     short L2        ; restore stack and return

            ;
            ; Here we do it the hard way.  Remember, eax contains DVSRHI
            ;

L1:
            mov     ecx, eax        ; ecx:ebx <- divisor
            mov     ebx, CRT_LOWORD(DVSR)
            mov     edx, CRT_HIWORD(DVND) ; edx:eax <- dividend
            mov     eax, CRT_LOWORD(DVND)
L3:
            shr     ecx, 1          ; shift divisor right one bit; hi bit <- 0
            rcr     ebx, 1
            shr     edx, 1          ; shift dividend right one bit; hi bit <- 0
            rcr     eax, 1
            or      ecx, ecx
            jnz     short L3        ; loop until divisor < 4194304K
            div     ebx             ; now divide, ignore remainder
            mov     esi, eax        ; save quotient

            ;
            ; We may be off by one, so to check, we will multiply the quotient
            ; by the divisor and check the result against the orignal dividend
            ; Note that we must also check for overflow, which can occur if the
            ; dividend is close to 2**64 and the quotient is off by 1.
            ;

            mul     CRT_HIWORD(DVSR) ; QUOT * CRT_HIWORD(DVSR)
            mov     ecx, eax
            mov     eax, CRT_LOWORD(DVSR)
            mul     esi             ; QUOT * CRT_LOWORD(DVSR)
            add     edx, ecx        ; EDX:EAX = QUOT * DVSR
            jc      short L4        ; carry means Quotient is off by 1

            ;
            ; do long compare here between original dividend and the result of the
            ; multiply in edx:eax.  If original is larger or equal, we are ok, otherwise
            ; subtract one (1) from the quotient.
            ;

            cmp     edx, CRT_HIWORD(DVND) ; compare hi words of result and original
            ja      short L4        ; if result > original, do subtract
            jb      short L5        ; if result < original, we are ok
            cmp     eax, CRT_LOWORD(DVND) ; hi words are equal, compare lo words
            jbe     short L5        ; if less or equal we are ok, else subtract
L4:
            dec     esi             ; subtract 1 from quotient
L5:
            xor     edx, edx        ; edx:eax <- quotient
            mov     eax, esi

            ;
            ; Just the cleanup left to do.  edx:eax contains the quotient.
            ; Restore the saved registers and return.
            ;

L2:
            pop     esi
            pop     ebx

            ret     16
        }

        #undef DVND
        #undef DVSR
    }

    __declspec(naked) void _aullrem()
    {
        #define DVND    esp + 8       // stack address of dividend (a)
        #define DVSR    esp + 16      // stack address of divisor (b)

        __asm
        {
            push    ebx

            ; Now do the divide.  First look to see if the divisor is less than 4194304K.
            ; If so, then we can use a simple algorithm with word divides, otherwise
            ; things get a little more complex.
            ;

            mov     eax, CRT_HIWORD(DVSR) ; check to see if divisor < 4194304K
            or      eax, eax
            jnz     short L1        ; nope, gotta do this the hard way
            mov     ecx, CRT_LOWORD(DVSR) ; load divisor
            mov     eax, CRT_HIWORD(DVND) ; load high word of dividend
            xor     edx, edx
            div     ecx             ; edx <- remainder, eax <- quotient
            mov     eax, CRT_LOWORD(DVND) ; edx:eax <- remainder:lo word of dividend
            div     ecx             ; edx <- final remainder
            mov     eax, edx        ; edx:eax <- remainder
            xor     edx, edx
            jmp     short L2        ; restore stack and return

            ;
            ; Here we do it the hard way.  Remember, eax contains DVSRHI
            ;

L1:
            mov     ecx, eax        ; ecx:ebx <- divisor
            mov     ebx, CRT_LOWORD(DVSR)
            mov     edx, CRT_HIWORD(DVND) ; edx:eax <- dividend
            mov     eax, CRT_LOWORD(DVND)
L3:
            shr     ecx, 1          ; shift divisor right one bit; hi bit <- 0
            rcr     ebx, 1
            shr     edx, 1          ; shift dividend right one bit; hi bit <- 0
            rcr     eax, 1
            or      ecx, ecx
            jnz     short L3        ; loop until divisor < 4194304K
            div     ebx             ; now divide, ignore remainder

            ;
            ; We may be off by one, so to check, we will multiply the quotient
            ; by the divisor and check the result against the orignal dividend
            ; Note that we must also check for overflow, which can occur if the
            ; dividend is close to 2**64 and the quotient is off by 1.
            ;

            mov     ecx, eax        ; save a copy of quotient in ECX
            mul     CRT_HIWORD(DVSR)
            xchg    ecx, eax        ; put partial product in ECX, get quotient in EAX
            mul     CRT_LOWORD(DVSR)
            add     edx, ecx        ; EDX:EAX = QUOT * DVSR
            jc      short L4        ; carry means Quotient is off by 1

            ;
            ; do long compare here between original dividend and the result of the
            ; multiply in edx:eax.  If original is larger or equal, we're ok, otherwise
            ; subtract the original divisor from the result.
            ;
            cmp     edx, CRT_HIWORD(DVND) ; compare hi words of result and original
            ja      short L4        ; if result > original, do subtract
            jb      short L5        ; if result < original, we're ok
            cmp     eax, CRT_LOWORD(DVND) ; hi words are equal, compare lo words
            jbe     short L5        ; if less or equal we're ok, else subtract
L4:
            sub     eax, CRT_LOWORD(DVSR) ; subtract divisor from result
            sbb     edx, CRT_HIWORD(DVSR)
L5:
            ;
            ; Calculate remainder by subtracting the result from the original dividend.
            ; Since the result is already in a register, we will perform the subtract in
            ; the opposite direction and negate the result to make it positive.
            ;
            sub     eax, CRT_LOWORD(DVND) ; subtract original dividend from result
            sbb     edx, CRT_HIWORD(DVND)
            neg     edx             ; and negate it
            neg     eax
            sbb     edx, 0
            ;
            ; Just the cleanup left to do.  dx:ax contains the remainder.
            ; Restore the saved registers and return.
            ;
L2:
            pop     ebx
            ret     16
        }
        #undef DVND
        #undef DVSR
    }

    __declspec(naked) void _allshl()
    {
        __asm
        {
            ;
            ; Handle shifts of 64 or more bits (all get 0)
            ;
            cmp     cl, 64
            jae     short RETZERO

            ;
            ; Handle shifts of between 0 and 31 bits
            ;
            cmp     cl, 32
            jae     short MORE32
            shld    edx,eax,cl
            shl     eax,cl
            ret

            ;
            ; Handle shifts of between 32 and 63 bits
            ;
MORE32:
            mov     edx,eax
            xor     eax,eax
            and     cl,31
            shl     edx,cl
            ret

            ;
            ; return 0 in edx:eax
            ;
RETZERO:
            xor     eax,eax
            xor     edx,edx
            ret
        }
    }

#endif
}
