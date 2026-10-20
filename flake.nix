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
      packages = forSystems (
        pkgs:
        let
          lib = pkgs.lib;

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

          # Upstream protobuf's cmake builds the static libraries by default,
          # so it only needs shared linking disabled; abseil only builds
          # archives when asked to.
          abseil-cpp-static = pkgs.abseil-cpp_202608.override { static = true; };
          protobuf-static = pkgs.protobuf.override {
            enableShared = false;
            abseil-cpp = abseil-cpp-static;
          };

          mkGfxstreamHost =
            { static ? false }:
            pkgs.stdenv.mkDerivation {
              pname = if static then "gfxstream-host-static" else "gfxstream-host";
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
                (if static then protobuf-static else protobuf)
                vulkan-headers
                perfetto-sdk
              ] ++ lib.optionals static [
                # Explicit: the .pc chain of the static protobuf resolves
                # through these paths even when nothing propagates them.
                abseil-cpp-static
              ];

              preConfigure = ''
                for proto in host/*snapshot.proto; do
                  protoc -Ihost --cpp_out=host "$proto"
                done
              '';

              mesonFlags =
                [
                  "-Dgfxstream-build=host"
                  "-Dcpp_std=gnu++20"
                  "-Dsnapshot=enabled"
                ]
                ++ lib.optionals static [
                  "-Ddefault_library=static"
                ];

              # meson does not split -Dcpp_args on commas; env vars it splits correctly.
              CXXFLAGS = "-DGFXSTREAM_BUILD_WITH_TRACING -DGFXSTREAM_BUILD_WITH_PERFETTO_SDK -I${perfetto-sdk}/include";
              LDFLAGS = "-L${perfetto-sdk}/lib -lperfetto_sdk";

              # A static archive cannot carry link args: everything the
              # shared build resolves at load time must reach the consumer
              # through the pkg-config file. meson puts the dependencies in
              # Libs.private (correct for pkg-config --static, but cargo's
              # pkg-config crate queries plain --libs), and the perfetto SDK
              # is only linked via LDFLAGS in the shared profile. Fold both
              # into Libs: for a static-only library the full closure is
              # public by definition.
              postInstall = lib.optionalString static ''
                pc=$out/lib/pkgconfig/gfxstream_backend.pc
                echo "gfxstream_backend.pc before folding:"; cat "$pc"
                # For a static library the whole dependency closure is
                # public: a plain `pkg-config --libs` must resolve it, so
                # merge the private sections (pkg-config accumulates
                # repeated keywords) and add the perfetto SDK, which the
                # shared profile links via LDFLAGS at load time.
                sed -i 's/^Requires\.private:/Requires:/' "$pc"
                sed -i 's/^Libs\.private:/Libs:/' "$pc"
                sed -i "s#^Libs:#Libs: -L${perfetto-sdk}/lib -lperfetto_sdk#" "$pc"
                echo "gfxstream_backend.pc after folding:"; cat "$pc"
              '';
            };
        in
        rec {
          default = mkGfxstreamHost { };
          static = mkGfxstreamHost { static = true; };
          inherit perfetto-sdk;
        }
      );

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
