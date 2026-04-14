{
  description = "Logos Delivery Module";

  inputs = {
    logos-module-builder.url = "github:logos-co/logos-module-builder";
    nix-bundle-lgx.url = "github:logos-co/nix-bundle-lgx";
    logos-delivery.url = "git+https://github.com/logos-messaging/logos-delivery?submodules=1";
  };

  outputs = inputs@{ logos-module-builder, ... }:
    logos-module-builder.lib.mkLogosModule {
      src = ./.;
      configFile = ./metadata.json;
      flakeInputs = inputs;
      # logos-delivery's default package = libwaku, not liblogosdelivery.
      # Wrap it so mkLogosModule resolves .default → liblogosdelivery per-system,
      # which feeds into mkExternalLib and then buildPlugin.externalLibCopies.
      externalLibInputs = {
        logosdelivery = {
          packages = builtins.listToAttrs (map (system: {
            name = system;
            value = { default = inputs.logos-delivery.packages.${system}.liblogosdelivery; };
          }) [ "aarch64-darwin" "x86_64-darwin" "aarch64-linux" "x86_64-linux" ]);
        };
      };
      # buildPlugin.externalLibCopies copies externalLibs/lib/* → lib/ but not include/*.
      # The header must land in lib/ for the CMake INCLUDE_DIRS. The liblogosdelivery
      # derivation is already a build dep (via externalLibs), so its store path is present.
      preConfigure = ''
        mkdir -p lib
        find /nix/store -maxdepth 3 -path "*logos-external*liblogosdelivery.h" 2>/dev/null | head -1 | xargs -I{} cp {} lib/ || true
      '';
      # Bundle runtime libraries alongside the plugin.
      postInstall = ''
        # liblogosdelivery.dylib is copied to build/modules/ by cmake PRE_LINK.
        # buildPlugin's installPhase checks lib/ (= build/lib/) which doesn't exist,
        # so we copy it here explicitly.
        cp modules/liblogosdelivery.dylib $out/lib/ 2>/dev/null || true

        # Use pkg-config to locate the exact libpq from the build environment
        LIBPQ_LIBDIR=$(pkg-config --variable=libdir libpq 2>/dev/null || true)
        if [ -n "$LIBPQ_LIBDIR" ] && [ -d "$LIBPQ_LIBDIR" ]; then
          for f in "$LIBPQ_LIBDIR"/libpq.*; do
            [ -f "$f" ] && cp -L "$f" $out/lib/ 2>/dev/null || true
          done
        fi
      '';
    };
}
