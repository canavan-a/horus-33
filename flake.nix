{
  description = "horus-33 — camera-gimbal person tracking: vision pipeline, control relay, REST/WebSocket API, web UI";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      # Only capture-eye is a real Nix derivation. horus-server and horusctl
      # are Go — see nix/module.nix's "horus-server is built by an activation
      # script" for why they aren't buildGoModule packages: a vendorHash kept
      # in sync by hand is the same maintenance cost as npm's npmDepsHash, for
      # artifacts with no reproducibility requirement that would justify it.
      # The devShell below already carries a Go toolchain for `go run`/`go build`.
      packages = forAllSystems (pkgs: {
        capture-eye = pkgs.callPackage ./nix/capture-eye.nix { };
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.capture-eye;
      });

      # Covers the whole repo — C++ (capture-eye), Go (server/, tui-controller/)
      # and Node (web/) in one `nix develop`. capture-eye/flake.nix stays the
      # fast inner-loop shell for pure C++ work; this is for touching more
      # than one language at once.
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.capture-eye ];
          nativeBuildInputs = with pkgs; [
            go
            gopls
            nodejs_22
            v4l-utils
            mediamtx
            socat # exercising capture-eye/src/control_relay.cpp by hand
          ];
        };
      });

      nixosModules.default = import ./nix/module.nix;

      formatter = forAllSystems (pkgs: pkgs.nixpkgs-fmt);
    };
}
