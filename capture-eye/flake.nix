{
  description = "capture-eye — host-side vision + tracking for Horus-33";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      devShells = forAllSystems (pkgs:
        let
          mkShell = { withOpenVINO ? false }: pkgs.mkShell {
            name = "capture-eye" + (if withOpenVINO then "-openvino" else "");

            nativeBuildInputs = with pkgs; [
              cmake
              ninja
              pkg-config
              clang-tools # clang-format, clang-tidy, clangd
              gdb
              v4l-utils   # v4l2-ctl, for cross-checking --list-formats
              mediamtx    # WebRTC/RTSP fan-out for the H.264 sink (M5)
            ];

            buildInputs = with pkgs; [
              opencv4          # decode + preprocessing + overlay drawing
              onnxruntime      # inference
              nlohmann_json    # config files
              libserialport    # /dev/ttyACM0 link to the ESP32-S3
              curl             # model download (model_store.cpp only)
              openssl          # sha256 verification of the downloaded model
              catch2_3         # tests
              # Must match the ffmpeg major that opencv4 links, or both ABIs end
              # up in one process: opencv's videoio pulls in libavcodec.so.62,
              # and the default ffmpeg is a major ahead of that.
              ffmpeg_8         # H.264 encode for the video sink
              libva            # VAAPI hardware encode path
            ] ++ pkgs.lib.optional withOpenVINO pkgs.openvino;

            # Surfaced to CMake so the OpenVINO backend is compiled in only when
            # the shell actually provides it.
            CAPTURE_EYE_OPENVINO = if withOpenVINO then "ON" else "OFF";

            shellHook = ''
              echo "capture-eye dev shell — OpenVINO: $CAPTURE_EYE_OPENVINO"
              echo "  cmake -B build -G Ninja -DCAPTURE_EYE_OPENVINO=$CAPTURE_EYE_OPENVINO"
              echo "  ninja -C build"
            '';
          };
        in
        {
          default = mkShell { };
          openvino = mkShell { withOpenVINO = true; };
        });

      formatter = forAllSystems (pkgs: pkgs.nixpkgs-fmt);
    };
}
