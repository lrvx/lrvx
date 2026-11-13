/*
 * lrvx Engine
 * Developed by lrvx contributors (https://github.com/lrvx)
 *
 * Copyright (c) 2025 lrvx contributors
 * Licensed under the MIT License. See LICENSE file in the project root for full
 * license information.
 */

// Translate lrvx::LrvxError thrown from C++ engine code into a JavaScript
// Error with `.code` / `.helpUrl` extra properties set on the instance.
//
// Usage at the boundary of a NAPI method that calls into lrvx:
//
//     Napi::Value myMethod(const Napi::CallbackInfo& info) {
//       return node_lrvx::tryLrvx(info.Env(), [&] {
//         // ... call lrvx C++ that may throw LrvxError ...
//         return info.Env().Undefined();
//       });
//     }
//
// On LrvxError: a JS Error is thrown with `.code` and `.helpUrl` set,
// and the lambda's nominal return value is replaced with `env.Undefined()`.
// On Napi::Error (an argument check's TypeError or RangeError): rethrown as is.
// On std::exception: a generic JS Error is thrown with the .what() text.
// On success: the lambda's return value is returned unchanged.

#pragma once

#include <napi.h>

#include "lrvx/error/lrvx_error.h"

#include <exception>

namespace node_lrvx
{

template <typename Fn>
inline Napi::Value tryLrvx(Napi::Env env, Fn fn)
{
  try
  {
    return fn();
  }
  catch (const lrvx::LrvxError& e)
  {
    auto err = Napi::Error::New(env, e.message());
    err.Value().Set("code", Napi::String::New(env, e.code()));
    err.Value().Set("helpUrl", Napi::String::New(env, e.helpUrl()));
    err.Value().Set("name", Napi::String::New(env, "LrvxError"));
    err.ThrowAsJavaScriptException();
    return env.Undefined();
  }
  catch (const Napi::Error&)
  {
    // An argument check's TypeError or RangeError keeps its class; caught as
    // a std::exception below it became a plain Error.
    throw;
  }
  catch (const std::exception& e)
  {
    Napi::Error::New(env, e.what()).ThrowAsJavaScriptException();
    return env.Undefined();
  }
}

}  // namespace node_lrvx
