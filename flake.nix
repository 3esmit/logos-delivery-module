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
        delivery = inputs.logos-delivery;
      };
      # Copy liblogosdelivery header from the logos-delivery source tree.
      # The .so is not needed at compile time — shared libs allow undefined symbols.
      preConfigure = ''
        mkdir -p lib
        for f in $(find /nix/store -maxdepth 5 -name "liblogosdelivery.h" 2>/dev/null); do
          cp "$f" lib/ 2>/dev/null || true
        done
      '';
    };
}
