{
  description = "Xpeccy+ ZX Spectrum emulator";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs =
    { self, nixpkgs, ... }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.stdenv.mkDerivation {
          pname = "xpeccy-plus";
          version = builtins.replaceStrings [ "\n" "\r" ] [ "" "" ] (builtins.readFile ./VERSION);

          # Build the revision represented by this flake instead of fetching a
          # separate checkout from GitHub.
          src = self;

          nativeBuildInputs = [
            pkgs.cmake
            pkgs.pkg-config
            pkgs.qt6.wrapQtAppsHook
            # the user manual (tools/manual.py, docs/manual/requirements.txt)
            (pkgs.python3.withPackages (ps: [
              ps.sphinx
              ps.myst-parser
              ps.furo
            ]))
          ];

          buildInputs = [
            pkgs.qt6.qtbase
            pkgs.qt6.qt5compat
            pkgs.libGL
            pkgs.SDL2
            pkgs.zlib
            pkgs.curl
          ];

          # the version's date: the commit's, not the 1980 stdenv sets for every build
          env = nixpkgs.lib.optionalAttrs (self ? lastModified) {
            SOURCE_DATE_EPOCH = toString self.lastModified;
          };

          # the manual is built before CMake, which installs it from MANUAL_DIR; nixpkgs'
          # Sphinx is not the one docs/manual/requirements.txt pins, so a new warning there
          # leaves the manual out instead of failing the build
          preConfigure = ''
            if python3 tools/manual.py html; then
              cmakeFlagsArray+=("-DMANUAL_DIR=$PWD/build/manual")
            else
              echo "the manual did not build: left out"
            fi
          '';

          cmakeFlags = [
            "-DQTVERSION=6"
            # linked, not loaded at run time: the store has no global libcurl.so.4
            "-DCURL_DLOPEN=OFF"
          ];

          installPhase = ''
            runHook preInstall
            cmake --install . --prefix "$out"
            runHook postInstall
          '';

          meta = {
            description = "ZX Spectrum emulator";
            homepage = "https://github.com/dotkoval/xpeccy-plus";
            license = pkgs.lib.licenses.mit;
            mainProgram = "xpeccy-plus";
          };
        };
      });
    };
}
