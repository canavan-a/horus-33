# The vision pipeline, as a real Nix derivation (unlike web/, see the plan's
# M10 notes on why that one is built by an activation script instead). Mirrors
# capture-eye/flake.nix's devShell buildInputs exactly — that flake stays the
# fast inner-loop shell for day-to-day C++ work; this is what packages the
# same sources for a NixOS deployment.
{ lib
, stdenv
, cmake
, ninja
, pkg-config
, opencv4
, onnxruntime
, nlohmann_json
, libserialport
, curl
, openssl
, catch2_3
, ffmpeg_8
, libva
, openvino
# Second inference backend, off by default: OpenVINO is a large dependency and
# most deployments only ever run the ONNX one. A host that wants to benchmark or
# run it deploys packages.capture-eye-openvino instead.
, withOpenVINO ? false
}:

stdenv.mkDerivation {
  pname = "capture-eye" + lib.optionalString withOpenVINO "-openvino";
  version = "0.7.0"; # M7: config file + control relay

  # Git-tracked files only — a plain `../capture-eye` path copies whatever is
  # actually on disk, including a stray local build/ directory from `ninja -C
  # build` (its CMakeCache.txt records the host's absolute path and breaks
  # CMake's out-of-source-dir check the moment it lands in the sandbox at a
  # different path). Caught building the NixOS module's system.build.toplevel.
  src = lib.fileset.toSource {
    root = ../capture-eye;
    fileset = lib.fileset.gitTracked ../capture-eye;
  };

  nativeBuildInputs = [ cmake ninja pkg-config ];
  buildInputs = [
    opencv4
    onnxruntime
    nlohmann_json
    libserialport
    curl
    openssl
    catch2_3
    # Must match the ffmpeg major that opencv4 links, or both ABIs end up in
    # one process — see capture-eye/flake.nix's comment on ffmpeg_8 for the
    # libavcodec.so.62-vs-.so.63 mismatch this pin was found fixing.
    ffmpeg_8
    libva
  ] ++ lib.optional withOpenVINO openvino;

  cmakeFlags = [
    "-DCMAKE_BUILD_TYPE=Release"
    ("-DCAPTURE_EYE_OPENVINO=" + (if withOpenVINO then "ON" else "OFF"))
  ];

  # Where OpenVINOConfig.cmake lives depends on the nixpkgs vintage: newer
  # openvino splits a "dev" output with the file under the standard
  # lib/cmake/openvino, older ones ship a single output with it under
  # runtime/cmake, which CMake's config-mode search does not look in. The
  # module builds this package with the *host's* pkgs, so it has to cope with
  # both rather than with whichever one this repo's flake happens to pin.
  # Probing at build time keeps that honest — no import-from-derivation, and a
  # third layout would announce itself as a clear configure error rather than a
  # wrong path silently pointing nowhere.
  preConfigure = lib.optionalString withOpenVINO ''
    for dir in "${lib.getDev openvino}/lib/cmake/openvino" "${openvino}/runtime/cmake"; do
      if [ -f "$dir/OpenVINOConfig.cmake" ]; then
        cmakeFlagsArray+=("-DOpenVINO_DIR=$dir")
        break
      fi
    done
  '';

  doCheck = true;
  checkTarget = "test";

  meta = with lib; {
    description = "Real-time person tracking pipeline for the horus-33 camera gimbal";
    license = licenses.mit;
    platforms = platforms.linux;
    mainProgram = "capture-eye";
  };
}
