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
    in
    {
      packages = forAllSystems (pkgs: {
        default = pkgs.python3Packages.buildPythonApplication {
          pname = "s226";
          version = "0.1.0";

          src = ./.;

          pyproject = false;

          propagatedBuildInputs = with pkgs.python3Packages; [
            bleak
          ];

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
          program = "${self.packages.${pkgs.system}.default}/bin/s226";
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
