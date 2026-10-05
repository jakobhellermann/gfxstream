{
  description = "gfxstream host backend for Kumquat";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";
  };

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs { inherit system; }));
    in
    {
      packages = forSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "gfxstream-host-wip";
          version = "0.1.2";
          src = ./.;

          nativeBuildInputs = with pkgs; [
            meson
            ninja
            pkg-config
            protobuf
            python3
          ];
          buildInputs = with pkgs; [
            libxcb.dev
            libx11.dev
            protobuf
            vulkan-headers
          ];

          preConfigure = ''
            for proto in host/*snapshot.proto; do
              protoc -Ihost --cpp_out=host "$proto"
            done
          '';

          mesonFlags = [
            "-Dgfxstream-build=host"
            "-Dcpp_std=gnu++20"
          ];
        };
      });

      devShells = forSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${pkgs.stdenv.hostPlatform.system}.default ];
          packages = with pkgs; [
            meson
            ninja
            pkg-config
            python3
          ];
        };
      });
    };
}
