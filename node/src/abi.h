// node/src/abi.h -- the C ABI version handshake the addon runs when it
// loads.
//
// lrvx_capi.h asks every caller to "compare LRVX_CAPI_ABI_VERSION against
// lrvx_capi_abi_version() once at startup and refuse the mismatch". The
// addon compiles against the header in this tree and links whichever
// liblrvx the loader found, and the structs on that boundary carry no
// reserved tail: a skew reads as wrong numbers rather than a failed load,
// so require() is the last place where noticing it is still cheap.
//
// registerAbi throws from Init on a mismatch, which fails the require
// with a message naming both versions. The pair is exported too, so a
// caller can report what it is running against.

#pragma once
#include <napi.h>

#include "lrvx/capi/abi_check.hpp"

#include <string>

namespace node_lrvx
{

inline bool registerAbi(Napi::Env env, Napi::Object exports)
{
  std::string message;
  if (!lrvx::capi::checkAbiVersion(LRVX_CAPI_ABI_VERSION, &message))
  {
    Napi::Error::New(env, message).ThrowAsJavaScriptException();
    return false;
  }

  exports.Set("CAPI_ABI_VERSION",
              Napi::Number::New(env, static_cast<double>(LRVX_CAPI_ABI_VERSION)));
  exports.Set("capiAbiVersion",
              Napi::Function::New(env,
                                  [](const Napi::CallbackInfo& info)
                                  {
                                    return Napi::Number::New(
                                        info.Env(),
                                        static_cast<double>(lrvx_capi_abi_version()));
                                  }));
  // Why the last call that reports failure by its return value failed:
  // FundingSchedule.loadTape, recompress, extractSymbols, extractTimeRange.
  // Each clears it on entry, so "" after one of them means it did not fail.
  exports.Set("lastError", Napi::Function::New(env,
                                               [](const Napi::CallbackInfo& info)
                                               {
                                                 return Napi::String::New(
                                                     info.Env(), lrvx_last_error_message());
                                               }));
  return true;
}

}  // namespace node_lrvx
