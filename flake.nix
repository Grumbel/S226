{
  description = "S226 BLE reverse engineering tool";

  inputs = {
    nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  };

  outputs = { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      forAllSystems = f:
        nixpkgs.lib.genAttrs systems (
          system:
          f {
            inherit system;
            pkgs = import nixpkgs { inherit system; };
          }
        );

      versionBase =
        nixpkgs.lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);

      gitRev = self.shortRev or self.dirtyShortRev or "dirty";
      isDev = nixpkgs.lib.strings.hasInfix "-dev" versionBase;
      version =
        if isDev then
          "${versionBase}.${toString (self.revCount or 0)}+g${gitRev}"
        else
          versionBase;
    in
    {
      packages = forAllSystems ({ pkgs, ... }:
        let
          # Bumble is not always packaged; build from PyPI.
          bumble = pkgs.python3Packages.buildPythonPackage rec {
            pname = "bumble";
            version = "0.0.215";
            format = "pyproject";

            src = pkgs.fetchPypi {
              inherit pname version;
              hash = "sha256-W6M6lWDm/OvfXPz7HfzMhjKrRqAtuUmF2ThNGBTO04U=";
            };

            nativeBuildInputs = with pkgs.python3Packages; [
              setuptools
              setuptools-scm
              wheel
            ];

            propagatedBuildInputs = with pkgs.python3Packages; [
              aiohttp
              click
              cryptography
              humanize
              platformdirs
              appdirs
              prompt-toolkit
              prettytable
              pyee
              pyserial
              pyserial-asyncio
              pyusb
              libusb1
              grpcio
              protobuf
              # Metadata pins websockets==13.1; 16.x works for our use.
              websockets
            ];

            SETUPTOOLS_SCM_PRETEND_VERSION = version;

            doCheck = false;
            # Upstream metadata pins optional/android extras and an old
            # websockets; Nix provides compatible runtime modules instead.
            dontCheckRuntimeDeps = true;
            pythonImportsCheck = [ "bumble" ];
          };

          mkScript = { name, srcFile }:
            pkgs.python3Packages.buildPythonApplication {
              pname = name;
              inherit version;
              src = ./.;
              format = "other";

              propagatedBuildInputs = with pkgs.python3Packages; [
                bleak
              ] ++ (if name == "s226-bumble" then [ bumble ] else [ ]);

              nativeBuildInputs = [ pkgs.makeWrapper ];

              installPhase = ''
                runHook preInstall
                mkdir -p $out/bin

                if ! grep -q '@S226_VERSION@' ${srcFile}; then
                  echo "error: @S226_VERSION@ token missing from ${srcFile}" >&2
                  exit 1
                fi

                sed "s|@S226_VERSION@|${version}|g" ${srcFile} > $out/bin/${name}

                if grep -q '@S226_VERSION@' $out/bin/${name}; then
                  echo "error: version token was not substituted" >&2
                  exit 1
                fi

                chmod +x $out/bin/${name}

                # libusb for Bumble USB transport
                ${if name == "s226-bumble" then ''
                wrapProgram $out/bin/${name} \
                  --prefix LD_LIBRARY_PATH : "${pkgs.libusb1}/lib"
                '' else ""}

                runHook postInstall
              '';
            };

          s226-hr = pkgs.stdenv.mkDerivation {
            pname = "s226-hr";
            inherit version;
            src = nixpkgs.lib.fileset.toSource {
              root = ./.;
              fileset = nixpkgs.lib.fileset.unions [
                ./CMakeLists.txt
                ./VERSION
                ./lib
                ./cli
                ./app
                ./udev
                ./data
                ./man
              ];
            };

            nativeBuildInputs = [
              pkgs.cmake
              pkgs.pkg-config
              pkgs.qt6.wrapQtAppsHook
              pkgs.librsvg # rsvg-convert renders the PNG icon sizes
            ];
            nativeCheckInputs = [
              pkgs.desktop-file-utils
              pkgs.appstream
              pkgs.mandoc
            ];
            buildInputs = [
              pkgs.libusb1
              pkgs.qt6.qtbase
              pkgs.qt6.qtmultimedia
              pkgs.qt6.qtsvg # SVG window icon
              # Qt6 DBus (MPRIS) is part of qtbase; no separate qtdbus attr.
            ];
            cmakeFlags = [ "-DS226_VERSION=${version}" ];
            # Qt Multimedia dlopen()s PipeWire for audio output.
            qtWrapperArgs = [
              "--prefix LD_LIBRARY_PATH : ${pkgs.lib.makeLibraryPath [ pkgs.pipewire ]}"
            ];
            doCheck = true;
          };
        in
        {
          default = s226-hr;
          inherit s226-hr;
          s226 = mkScript { name = "s226"; srcFile = "s226.py"; };
          s226-bumble = mkScript { name = "s226-bumble"; srcFile = "s226_bumble.py"; };
        });

      apps = forAllSystems ({ pkgs, system }: {
        default = {
          type = "app";
          program = "${self.packages.${system}.s226-hr}/bin/s226-hr";
        };
        s226-hr = self.apps.${system}.default;
        s226-cli = {
          type = "app";
          program = "${self.packages.${system}.s226-hr}/bin/s226-cli";
        };
        s226 = {
          type = "app";
          program = "${self.packages.${system}.s226}/bin/s226";
        };
        s226-bumble = {
          type = "app";
          program = "${self.packages.${system}.s226-bumble}/bin/s226-bumble";
        };
      });

      devShells = forAllSystems ({ pkgs, system }: {
        default = pkgs.mkShell {
          inputsFrom = [ self.packages.${system}.s226-hr ];
        };
      });
    };
}
