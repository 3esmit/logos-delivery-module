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
      externalLibInputs = {
        logosdelivery = {
          input = inputs.logos-delivery;
          packages.default = "liblogosdelivery";
        };
      };
      # Bundle runtime libraries alongside the plugin.
      postInstall = ''
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
