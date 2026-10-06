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
      packages = forSystems (pkgs: rec {
        # Perfetto C++ SDK amalgamation: compile perfetto.cc once, link statically.
        perfetto-sdk = pkgs.stdenv.mkDerivation {
          pname = "perfetto-cpp-sdk";
          version = "58.2";
          src = pkgs.fetchurl {
            url = "https://github.com/google/perfetto/releases/download/v58.2/perfetto-cpp-sdk-src.zip";
            sha256 = "e56126ff81f0f914d35fe03c72419d147acf2890f36fbda7d4558fd5b84c4b09";
          };
          nativeBuildInputs = [ pkgs.unzip ];
          sourceRoot = ".";

          buildPhase = ''
            runHook preBuild
            $CXX -std=c++17 -O2 -c perfetto.cc -o perfetto.o
            runHook postBuild
          '';

          installPhase = ''
            install -Dm644 perfetto.h $out/include/perfetto.h
            mkdir -p $out/lib
            ar rcs $out/lib/libperfetto_sdk.a perfetto.o
          '';
        };

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
            perfetto-sdk
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

          # meson does not split -Dcpp_args on commas; env vars it splits correctly.
          CXXFLAGS = "-DGFXSTREAM_BUILD_WITH_TRACING -DGFXSTREAM_BUILD_WITH_PERFETTO_SDK -I${perfetto-sdk}/include";
          LDFLAGS = "-L${perfetto-sdk}/lib -lperfetto_sdk";
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
