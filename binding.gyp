{
  "targets": [
    {
      "target_name": "bsonToJson",
      "sources": [
        "src/bson-to-json.cc",
        "deps/double_conversion/double-to-string.cc",
        "deps/double_conversion/cached-powers.cc",
        "deps/double_conversion/bignum.cc",
        "deps/double_conversion/bignum-dtoa.cc",
        "deps/double_conversion/fast-dtoa.cc",
        "deps/double_conversion/fixed-dtoa.cc",
      ],
      "include_dirs": [
        "<!@(node -p \"require('node-addon-api').include\")"
      ],
      "dependencies": [
        "<!(node -p \"require('node-addon-api').gyp\")"
      ],
      "defines": [
        "NAPI_DISABLE_CPP_EXCEPTIONS"
      ],
      "cflags":[
        "-fvisibility=hidden",
        "-march=native",
        "-falign-loops=32",
        "-Wno-unused-function", # CPU feature detection only used on Win
        "-Wno-unused-const-variable" # cpuid regs
      ],
      "msvs_settings": {
        "VCCLCompilerTool": {
          "EnableEnhancedInstructionSet": 0 # /arch:
          # 0-not set, 1-sse, 2-sse2, 3-avx, 4-ia32, 5-avx2
          # Not set: spurious warning with /arch:SSE2, which is baseline anyway
        }
      },
      "xcode_settings": {
        "OTHER_CPLUSPLUSFLAGS": [
          "-fvisibility=hidden",
          "-march=native",
          "-Wno-unused-function", # CPU feature detection only used on Win
          "-Wno-unused-const-variable"
        ]
      }
    }
  ]
}
