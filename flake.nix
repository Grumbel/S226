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

      # Single source of truth: ./VERSION (e.g. "0.2.0-dev").
      versionBase =
        nixpkgs.lib.strings.removeSuffix "\n" (builtins.readFile ./VERSION);

      # Development builds: 0.2.0-dev.<revCount>+g<shortRev>
      # Release builds (no "-dev" in VERSION): use VERSION as-is.
      gitRev = self.shortRev or self.dirtyShortRev or "dirty";
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

          # Single-file script; do not expect setuptools/pyproject.
          format = "other";

          propagatedBuildInputs = with pkgs.python3Packages; [
            bleak
          ];

          # Embed the full version string into the installed script.
          # sed + explicit checks: build fails if the token is missing or
          # substitution does not take effect (no silent fallback).
          installPhase = ''
            runHook preInstall

            mkdir -p $out/bin

            if ! grep -q '@S226_VERSION@' s226.py; then
              echo "error: @S226_VERSION@ token missing from s226.py" >&2
              exit 1
            fi

            sed "s|@S226_VERSION@|${version}|g" s226.py > $out/bin/s226

            if grep -q '@S226_VERSION@' $out/bin/s226; then
              echo "error: version token was not substituted" >&2
              exit 1
            fi
            if ! grep -qF '${version}' $out/bin/s226; then
              echo "error: expected version string not found in installed script" >&2
              exit 1
            fi

            chmod +x $out/bin/s226

            runHook postInstall
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
