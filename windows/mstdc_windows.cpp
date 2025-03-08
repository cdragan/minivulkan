// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2024 Chris Dragan

#include "../mstdc.h"

#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int WINAPI WinMain(HINSTANCE hinstance, HINSTANCE hprev_instance, PSTR cmd_line, INT cmd_show);

using PVFV = void (__cdecl*)();

extern "C" {
    #pragma const_seg(".CRT$XIA")
    const PVFV __xi_a = nullptr;
    #pragma const_seg(".CRT$XIZ")
    const PVFV __xi_z = nullptr;
    #pragma const_seg(".CRT$XCA")
    const PVFV __xc_a = nullptr;
    #pragma const_seg(".CRT$XCZ")
    const PVFV __xc_z = nullptr;
}

int __stdcall WinMainCRTStartup()
{
    // Call C++ initializers
    for (const PVFV* init = &__xi_a; init < &__xi_z; ++init)
        if (*init)
            (*init)();
    for (const PVFV* init = &__xc_a; init < &__xc_z; ++init)
        if (*init)
            (*init)();

    ExitProcess(WinMain(nullptr, nullptr, nullptr, 0));
}

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

    int atexit(void (__cdecl *func )(void))
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

        assert(num >= static_cast<int>(sizeof(buf)) || buf[num] == 0);

        static HANDLE out = INVALID_HANDLE_VALUE;
        if (out == INVALID_HANDLE_VALUE)
        {
            out = GetStdHandle(STD_OUTPUT_HANDLE);
        }
        if (out != INVALID_HANDLE_VALUE)
        {
            DWORD numWritten = 0;
            WriteFile(out, buf, static_cast<DWORD>(num), &numWritten, nullptr);
        }

        return num;
    }

    int snprintf(char* buf, size_t size, const char* format, ...)
    {
        va_list args;
        va_start(args, format);

        const int num = wvsprintf(buf, format, args);

        va_end(args);

        return num;
    }
#endif

// The code below is used in place of MS Visual C Runtime library in 32-bit builds.
// In 32-bit builds the compiler calls these functions in order to perform 64-bit arithmetic.
// This code has been copied from GitHub mmozeiko/win32_crt_float.cpp
#ifdef _M_IX86
#   include <immintrin.h>
#   define CRT_LOWORD(x) dword ptr [x+0]
#   define CRT_HIWORD(x) dword ptr [x+4]

    __declspec(naked) void _allmul()
    {
        #define A esp + 8  // stack address of a
        #define B esp + 16 // stack address of b

        __asm {
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

    __declspec(naked) void _alldiv()
    {
        #define DVND    esp + 16      // stack address of dividend (a)
        #define DVSR    esp + 24      // stack address of divisor (b)

        __asm {
            push    edi
            push    esi
            push    ebx

            ; Determine sign of the result (edi = 0 if result is positive, non-zero
            ; otherwise) and make operands positive.

            xor     edi, edi        ; result sign assumed positive

            mov     eax, CRT_HIWORD(DVND) ; hi word of a
            or      eax, eax        ; test to see if signed
            jge     short L1        ; skip rest if a is already positive
            inc     edi             ; complement result sign flag
            mov     edx, CRT_LOWORD(DVND) ; lo word of a
            neg     eax             ; make a positive
            neg     edx
            sbb     eax, 0
            mov     CRT_HIWORD(DVND), eax ; save positive value
            mov     CRT_LOWORD(DVND), edx
L1:
            mov     eax, CRT_HIWORD(DVSR) ; hi word of b
            or      eax, eax        ; test to see if signed
            jge     short L2        ; skip rest if b is already positive
            inc     edi             ; complement the result sign flag
            mov     edx, CRT_LOWORD(DVSR) ; lo word of a
            neg     eax             ; make b positive
            neg     edx
            sbb     eax,0
            mov     CRT_HIWORD(DVSR), eax ; save positive value
            mov     CRT_LOWORD(DVSR), edx
L2:

            ;
            ; Now do the divide.  First look to see if the divisor is less than 4194304K.
            ; If so, then we can use a simple algorithm with word divides, otherwise
            ; things get a little more complex.
            ;
            ; NOTE - eax currently contains the high order word of DVSR
            ;

            or      eax, eax        ; check to see if divisor < 4194304K
            jnz     short L3        ; nope, gotta do this the hard way
            mov     ecx, CRT_LOWORD(DVSR) ; load divisor
            mov     eax, CRT_HIWORD(DVND) ; load high word of dividend
            xor     edx, edx
            div     ecx             ; eax <- high order bits of quotient
            mov     ebx, eax        ; save high bits of quotient
            mov     eax, CRT_LOWORD(DVND) ; edx:eax <- remainder:lo word of dividend
            div     ecx             ; eax <- low order bits of quotient
            mov     edx, ebx        ; edx:eax <- quotient
            jmp     short L4        ; set sign, restore stack and return

            ;
            ; Here we do it the hard way.  Remember, eax contains the high word of DVSR
            ;

L3:
            mov     ebx, eax        ; ebx:ecx <- divisor
            mov     ecx, CRT_LOWORD(DVSR)
            mov     edx, CRT_HIWORD(DVND) ; edx:eax <- dividend
            mov     eax, CRT_LOWORD(DVND)
L5:
            shr     ebx, 1          ; shift divisor right one bit
            rcr     ecx, 1
            shr     edx, 1          ; shift dividend right one bit
            rcr     eax, 1
            or      ebx, ebx
            jnz     short L5        ; loop until divisor < 4194304K
            div     ecx             ; now divide, ignore remainder
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
            jc      short L6        ; carry means Quotient is off by 1

            ;
            ; do long compare here between original dividend and the result of the
            ; multiply in edx:eax.  If original is larger or equal, we are ok, otherwise
            ; subtract one (1) from the quotient.
            ;

            cmp     edx, CRT_HIWORD(DVND) ; compare hi words of result and original
            ja      short L6        ; if result > original, do subtract
            jb      short L7        ; if result < original, we are ok
            cmp     eax, CRT_LOWORD(DVND) ; hi words are equal, compare lo words
            jbe     short L7        ; if less or equal we are ok, else subtract
L6:
            dec     esi             ; subtract 1 from quotient
L7:
            xor     edx, edx        ; edx:eax <- quotient
            mov     eax, esi

            ;
            ; Just the cleanup left to do.  edx:eax contains the quotient.  Set the sign
            ; according to the save value, cleanup the stack, and return.
            ;

L4:
            dec     edi             ; check to see if result is negative
            jnz     short L8        ; if EDI == 0, result should be negative
            neg     edx             ; otherwise, negate the result
            neg     eax
            sbb     edx, 0

            ;
            ; Restore the saved registers and return.
            ;

L8:
            pop     ebx
            pop     esi
            pop     edi

            ret     16
        }

        #undef DVND
        #undef DVSR
    }

    __declspec(naked) void _aulldiv()
    {
        #define DVND esp + 12 // stack address of dividend (a)
        #define DVSR esp + 20 // stack address of divisor (b)

        __asm {
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

        __asm {
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
        __asm {
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
            shld    edx, eax, cl
            shl     eax, cl
            ret

            ;
            ; Handle shifts of between 32 and 63 bits
            ;
MORE32:
            mov     edx, eax
            xor     eax, eax
            and     cl, 31
            shl     edx, cl
            ret

            ;
            ; return 0 in edx:eax
            ;
RETZERO:
            xor     eax, eax
            xor     edx, edx
            ret
        }
    }

    __declspec(naked) void _aullshr()
    {
        __asm
        {
            cmp     cl, 64
            jae     short retzero
            ;
            ; Handle shifts of between 0 and 31 bits
            ;
            cmp     cl, 32
            jae     short more32
            shrd    eax, edx, cl
            shr     edx, cl
            ret
            ;
            ; Handle shifts of between 32 and 63 bits
            ;
    more32:
            mov     eax, edx
            xor     edx, edx
            and     cl, 31
            shr     eax, cl
            ret
            ;
            ; return 0 in edx:eax
            ;
    retzero:
            xor     eax, eax
            xor     edx, edx
            ret
        }
    }

#if 0
    // Reference implementation of _ultod3
    static inline uint32_t& low(uint64_t& value)
    {
        return reinterpret_cast<uint32_t&>(value);
    }

    static inline uint32_t& high(uint64_t& value)
    {
        return *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(&value) + 4);
    }

    static inline int clz(uint32_t value)
    {
#ifdef _MSC_VER
        unsigned long index;
        if (_BitScanReverse(&index, value))
            return 31 - static_cast<uint32_t>(index);
        else
            return 32;
#else
        return value ? __builtin_clz(value) : 32;
#endif
    }

    double ultod3(uint64_t value)
    {
        const int top = clz(high(value));
        int exponent = 63 - top;
        if (top <= 11) {
            const int shift = 11 - top;
            low(value) = (low(value) >> shift) | (high(value) << (32 - shift));
            high(value) >>= shift;
        }
        else if (top < 32) {
            const int shift = top - 11;
            high(value) = (high(value) << shift) | (low(value) >> (32 - shift));
            low(value) <<= shift;
        }
        else {
            const int bottom = clz(low(value));
            exponent = 31 - bottom;
            if (bottom <= 11) {
                const int shift = 11 - bottom;
                high(value) = low(value) >> shift;
                low(value) <<= (32 - shift);
            }
            else {
                const int shift = bottom - 11;
                high(value) = low(value) << shift;
                low(value) = 0;
            }
        }

        high(value) = (high(value) & 0x000F'FFFFu) | ((0x3FFu + exponent) << 20);

        return *reinterpret_cast<double*>(&value);
    }
#endif

    __declspec(naked) void _ultod3()
    {
        #define LOW_VALUE DWORD PTR [esp]
        __asm {
            push    ebx
            push    esi
            push    edi
            push    edx                             ; high(value)
            push    ecx                             ; low(value)
            mov     edi, edx                        ; edi := high(value)
            bsr     ecx, edi
            je      no_high_bits
            mov     eax, 31
            mov     ebx, 63
            sub     eax, ecx
            sub     ebx, eax
            cmp     eax, 11
            jg      high_more_than_11_zeroes
            mov     esi, 11
            mov     ecx, 32
            sub     esi, eax
            mov     edx, edi
            mov     eax, LOW_VALUE
            sub     ecx, esi
            shl     edx, cl
            mov     ecx, esi
            shr     eax, cl
            or      edx, eax
            shr     edi, cl
            mov     LOW_VALUE, edx
            jmp     set_exponent_and_done

    high_more_than_11_zeroes:
            cmp     eax, 32
            jge     no_high_bits
            lea     esi, LOW_VALUE
            mov     ecx, 32
            mov     eax, LOW_VALUE
            sub     ecx, esi
            mov     edx, eax
            shr     edx, cl
            mov     ecx, esi
            shl     eax, cl
            shl     edi, cl
            mov     LOW_VALUE, eax
            or      edi, edx
            jmp     set_exponent_and_done

    no_high_bits:
            mov     edi, LOW_VALUE
            bsr     eax, edi
            je      no_low_bits
            mov     ecx, 31
            mov     ebx, 31
            sub     ecx, eax
            sub     ebx, ecx
            cmp     ecx, 11
            jg      low_more_than_11_zeroes
            mov     edx, 11
            mov     eax, edi
            sub     edx, ecx
            mov     ecx, 32
            sub     ecx, edx
            shl     eax, cl
            mov     ecx, edx
            mov     LOW_VALUE, eax
            shr     edi, cl
            jmp     set_exponent_and_done

    no_low_bits:
            mov     ecx, 32
            or      ebx, -1
    low_more_than_11_zeroes:
            add     ecx, -11
            mov     LOW_VALUE, 0
            shl     edi, cl

    set_exponent_and_done:
            lea     eax, DWORD PTR [ebx+1023]
            and     edi, 1048575 ; 000fffffH
            shl     eax, 20
            or      eax, edi
            mov     DWORD PTR [esp+4], eax
            movsd   xmm0, QWORD PTR [esp]

            add     esp, 8
            pop     edi
            pop     esi
            pop     ebx
            ret
        }
        #undef LOW_VALUE
    }

#if 0
    // Reference implementation of _ftoui3
    unsigned ftoui3(float v)
    {
        return static_cast<unsigned>(_mm_cvttss_si32(__m128{v, 0, 0, 0}));
    }
#endif

    __declspec(naked) void _ftoui3()
    {
        __asm {
            movss     xmm0, DWORD PTR [esp + 8]
            cvttss2si eax, xmm0
            ret
        }
    }

    __declspec(naked) void _ftol3()
    {
        __asm {
            movss     xmm0, DWORD PTR [esp + 8]
            cvttss2si eax, xmm0
            cdq
            ret
        }
    }
#endif
}
