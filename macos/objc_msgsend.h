// SPDX-License-Identifier: MIT
// SPDX-FileCopyrightText: Copyright (c) 2021-2026 Chris Dragan

// This header file contains wrapper functions which allow us to call objective-C
// functions from C++.

#pragma once

#include <objc/message.h>
#include <objc/runtime.h>
#include <CoreGraphics/CGGeometry.h>

template<typename Ret, typename... Args>
static inline Ret send_message(id self, SEL op, Args... args)
{
    return reinterpret_cast<Ret (*)(id, SEL, Args...)>(objc_msgSend)(self, op, args...);
}

template<typename Ret, typename... Args>
static inline Ret send_message_struct(id self, SEL op, Args... args)
{
#if defined(__x86_64__)
    // On x86_64, methods which return large structs use a different entry point
    return reinterpret_cast<Ret (*)(id, SEL, Args...)>(objc_msgSend_stret)(self, op, args...);
#else
    return reinterpret_cast<Ret (*)(id, SEL, Args...)>(objc_msgSend)(self, op, args...);
#endif
}

static inline id get_class(const char* name)
{
    return reinterpret_cast<id>(objc_getClass(name));
}

// On x86_64 BOOL is signed char, but on arm64 it's bool
#if defined(__x86_64__)
#   define OBJC_BOOL_ENCODING "c"
#else
#   define OBJC_BOOL_ENCODING "B"
#endif
