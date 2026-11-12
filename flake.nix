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

          # the manual is built before CMake, which installs it from MANUAL_DIR
          preConfigure = ''
            python3 tools/manual.py html
            cmakeFlagsArray+=("-DMANUAL_DIR=$PWD/build/manual")
          '';

          cmakeFlags = [
            "-DQTVERSION=6"
            # linked, not loaded at run time: the store has no global libcurl.so.4
            "-DCURL_DLOPEN=OFF"
            # the commit's date: the sandbox's clock says 1980
            "-DXDATE=${builtins.substring 0 8 (self.lastModifiedDate or "")}"
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
