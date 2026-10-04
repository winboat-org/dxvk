{
  pkgs,
  sources,
  dependencies,
  target,
  configuration,
}:
let
  architecture = if target == "engine-x86" then "x86" else "x64";
  archives = [
    "src/dxvk/libdxvk.a"
    "src/d3d11/libhelios_d3d11_static.a"
    "subprojects/dxbc-spirv/libdxbc_spv.a"
    "subprojects/libdisplay-info/libdisplay-info.a"
    "src/spirv/libspirv.a"
    "src/util/libutil.a"
    "src/wsi/libwsi.a"
    "src/vulkan/libvkcommon.a"
  ];
in
pkgs.runCommand "helios-dxvk-msvc-cross-${architecture}-${configuration}"
  {
    nativeBuildInputs = [
      pkgs.meson
      pkgs.ninja
      pkgs.pkg-config
      pkgs.glslang
      pkgs.stdenv.cc
      pkgs.llvmPackages_22.lld
      pkgs.llvmPackages_22.llvm
      (pkgs.python3.withPackages (p: [ p.jinja2 ]))
    ];
    env.LIB = pkgs.lib.concatStringsSep ";" (
      map (path: "${dependencies.msvcSysroot}/${path}/${architecture}") [
        "crt/lib"
        "sdk/lib/ucrt"
        "sdk/lib/um"
      ]
    );
  }
  ''
    cp -R ${sources.dxvk} source
    chmod -R u+w source
    patchShebangs source
    meson setup build source --wrap-mode=nodownload \
      --cross-file ${dependencies.msvcCrossFile} \
      --buildtype ${if configuration == "debug" then "debug" else "release"} \
      -Db_vscrt=mt '-Dc_args=/Z7 /FI${sources.helios}/umd/build-support/dxvk_c_compat.h' \
      -Denable_d3d8=false -Denable_d3d9=false -Denable_d3d10=false
    ninja -C build -j "$NIX_BUILD_CORES" ${pkgs.lib.escapeShellArgs archives}
    ${pkgs.lib.concatMapStringsSep "\n" (archive: ''
      mkdir -p "$out/$(dirname '${archive}')"
      cp 'build/${archive}' "$out/${archive}"
    '') archives}
    find build -type f -name '*.h' | while IFS= read -r header; do
      relative="''${header#build/}"
      mkdir -p "$out/$(dirname "$relative")"
      cp "$header" "$out/$relative"
    done
    mkdir -p "$out/licenses/dxvk" "$out/share/winboat"
    cp -R ${dependencies.msvcSysroot}/share/licenses/. "$out/licenses/"
    find ${sources.dxvk} -type f \( -iname 'LICENSE*' -o -iname 'COPYING*' -o -iname 'NOTICE*' \) \
      | while IFS= read -r notice; do
        relative="''${notice#${sources.dxvk}/}"
        mkdir -p "$out/licenses/dxvk/$(dirname "$relative")"
        cp "$notice" "$out/licenses/dxvk/$relative"
      done
    cp build/compile_commands.json "$out/share/winboat/compile_commands.json"
    python3 ${dependencies.msvcInspector} "$out" ${pkgs.llvmPackages_22.llvm}/bin/llvm-readobj \
      --architecture=${architecture} > "$out/images.json"
  ''
