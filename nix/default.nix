{
  pkgs,
  sources,
  dependencies ? { },
  target ? "win64",
  configuration ? "release",
  toolchain ? { },
  schemaVersion ? 1,
}:
assert toolchain == { };
assert schemaVersion == 1;
if (target == "engine-x64" || target == "engine-x86") && dependencies ? msvcCrossFile then
  import ./cross-engine.nix {
    inherit
      pkgs
      sources
      dependencies
      target
      configuration
      ;
  }
else if target == "engine-x64" || target == "engine-x86" then
  {
    backend = "devbox";
    purpose = "build";
    abi = "msvc";
    crt = "mt";
    architecture = if target == "engine-x86" then "x86" else "x64";
    nativeFile = if target == "engine-x86" then ./clang-cl-x86.ini else ./clang-cl-x64.ini;
    commands = [
      [
        "meson"
        "setup"
        "@buildDirectory@"
        "@sourceDirectory@"
        "--wrap-mode=nodownload"
        "--buildtype"
        (if configuration == "debug" then "debug" else "release")
        "--native-file"
        "@nativeFile@"
        "-Db_vscrt=mt"
        "-Dc_args=/FI@heliosSourceDirectory@/umd/build-support/dxvk_c_compat.h"
        "-Dcpp_args=/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH"
        "-Denable_d3d8=false"
        "-Denable_d3d9=false"
        "-Denable_d3d10=false"
      ]
      [
        "ninja"
        "-C"
        "@buildDirectory@"
        "src/dxvk/libdxvk.a"
        "src/d3d11/libhelios_d3d11_static.a"
        "subprojects/dxbc-spirv/libdxbc_spv.a"
        "subprojects/libdisplay-info/libdisplay-info.a"
        "src/spirv/libspirv.a"
        "src/util/libutil.a"
        "src/wsi/libwsi.a"
        "src/vulkan/libvkcommon.a"
      ]
    ];
    outputs = [
      "src/dxvk/libdxvk.a"
      "src/d3d11/libhelios_d3d11_static.a"
      "subprojects/dxbc-spirv/libdxbc_spv.a"
      "subprojects/libdisplay-info/libdisplay-info.a"
      "src/spirv/libspirv.a"
      "src/util/libutil.a"
      "src/wsi/libwsi.a"
      "src/vulkan/libvkcommon.a"
    ];
    requirements = [
      "LLVM-22.1.8-clang-cl-lld-link"
      "MSVC-v143"
      "SDK-10.0.26100.0"
      "fixed-shader-tools"
    ];
  }
else
  assert target == "win64";
  let
    cross = pkgs.pkgsCross.mingwW64;
    compiler = cross.stdenv.cc.override (old: {
      cc = old.cc.override {
        threadsCross = {
          model = "win32";
          package = null;
        };
      };
    });
    stdenv = pkgs.overrideCC cross.stdenv compiler;
  in
  stdenv.mkDerivation {
    pname = "helios-dxvk-mingw64";
    version = "3.0.2";
    src = sources.dxvk;
    buildInputs = [ cross.windows.pthreads ];
    nativeBuildInputs = [
      pkgs.meson
      pkgs.ninja
      pkgs.pkg-config
      pkgs.glslang
      (pkgs.python3.withPackages (p: [ p.jinja2 ]))
    ];
    mesonBuildType = if configuration == "debug" then "debug" else "debugoptimized";
    mesonWrapMode = "nodownload";
    mesonFlags = [
      "-Dbuild_id=true"
      "-Denable_d3d8=false"
    ];
    dontStrip = true; # Keep DWARF symbols in the manifested DLLs.
    postPatch = "patchShebangs --build subprojects/libdisplay-info/tool";
    postInstall = ''
      mkdir -p $out/share/licenses/dxvk
      cp ../LICENSE $out/share/licenses/dxvk/
      for module in ../include/vulkan ../include/spirv ../include/native/directx ../subprojects/dxbc-spirv ../subprojects/libdisplay-info ../subprojects/dxbc-spirv/submodules/spirv_headers; do
        name=$(basename "$module")
        mkdir -p "$out/share/licenses/dxvk/$name"
        find "$module" -maxdepth 1 -type f \( -iname '*license*' -o -iname '*copying*' \) -exec cp {} "$out/share/licenses/dxvk/$name/" \;
      done
    '';
  }
