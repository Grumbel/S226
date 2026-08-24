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
        nixpkgs.lib.genAttrs systems (system:
          f (import nixpkgs { inherit system; }));

      versionBase =
        nixpkgs.lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);
      gitRev = "${self.shortRev or self.dirtyShortRev or "dirty"}";
      isDev = nixpkgs.lib.strings.hasInfix "-dev" versionBase;
      version =
        if isDev then
          "${versionBase}.${toString (self.revCount or 0)}+g${gitRev}"
        else
          versionBase;
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.python3Packages.buildPythonApplication {
          pname = "s226";
          inherit version;

          src = ./.;

          pyproject = false;

          propagatedBuildInputs = with pkgs.python3Packages; [
            bleak
          ];

          # Bake the full version string into s226.py at build time.
          # --subst-var-by replaces @S226_VERSION@ with the value.
          postPatch = ''
            substituteInPlace s226.py --subst-var-by S226_VERSION ${pkgs.lib.escapeShellArg version}
          '';

          installPhase = ''
            mkdir -p $out/bin
            cp s226.py $out/bin/s226
            chmod +x $out/bin/s226
          '';
        };
      });

      apps = forAllSystems (pkgs: {
        default = {
          type = "app";
          program = "${self.packages.${pkgs.stdenv.hostPlatform.system}.default}/bin/s226";
        };
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          packages = [
            pkgs.python3
            pkgs.python3Packages.bleak
          ];
        };
      });
    };
}
