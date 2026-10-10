# Supple Audio plugin template

A framework and a starting product for CLAP-first audio plugins: CLAP, VST3
and AUv2 from one implementation, an SDK-free DSP engine, and a Pugl/NanoVG
editor toolkit with Metal on macOS and OpenGL on Linux and Windows.

![The starter product's editor](docs/editor.png)

A new product is one command away and builds, passes every test and opens in
a host as created. You then write three things: a parameter table, a
processor and an editor layout. Host plumbing, sample-accurate automation,
state, bypass, latency, telemetry, metering, gestures, keyboard access, text
entry, menus, HiDPI and resizing are the framework's.

## Start a product

```sh
cmake -DNAME="Name" -DID=com.suppleaudio.name -DCODE=NmXx -DDEST=../name \
      -P framework/tools/new_product.cmake            # add -DKIND=instrument for a synth
cd ../name && git init
cmake --preset release && cmake --build --preset release && ctest --preset release
```

`ID` and `CODE` (the AU subtype) identify sessions forever once shipped.

## What you edit

```
product/
  product.cmake   identity, kind, bus widths, sources
  Params.h        every parameter: one row each
  Processor.h     the DSP
  Editor.cpp      layout and the product's own drawing
  Theme.h         the palette
  test/           measured claims about the DSP
framework/        shared, identical in every product -- change it in the template
```

The starter product here is a utility filter (input gain, 12/24 dB/oct
Butterworth low and high cuts, mix, output gain), bit-transparent at its
defaults, with a live response graph whose handles drag. It shows every kind of
control and every pattern the framework expects; replace it.
`framework/examples/instrument` is the same for an instrument.

## Read next

- [framework/AGENTS.md](framework/AGENTS.md): how to build a product on
  this, for agents and people alike.
- [framework/docs/BUILDING.md](framework/docs/BUILDING.md): platforms,
  presets, every test, the tools.
- [framework/docs/CONVENTIONS.md](framework/docs/CONVENTIONS.md): why
  things are the way they are.

## Status

Built and tested on Linux (X11/OpenGL): every test in the release, debug,
AddressSanitizer and ThreadSanitizer presets, clap-validator, X11 embedding,
and REAPER loading the CLAP. macOS (Metal, AUv2) and Windows have not been
built since the framework's 1.0.0 rebuild; build and test there before shipping
for them.

## License

The template, framework included, is MIT licensed, copyright (c) 2026 Supple
Audio ([LICENSE](LICENSE)). Products choose their own: `new_product.cmake`
defaults to GPL-3.0-or-later for `product/` and leaves `framework/` MIT.
Third-party notices: [framework/THIRD_PARTY.md](framework/THIRD_PARTY.md),
full texts in [framework/NOTICES.txt](framework/NOTICES.txt).
