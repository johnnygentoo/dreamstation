# reshadefx

The ReShade FX effect compiler (preprocessor, parser and GLSL code generator),
copied unmodified from ReShade by Patrick Mours:

- Source: https://github.com/crosire/reshade, tag `v6.8.0`
  (commit 18deaa52de0c425a78b329e9cb3c497281cd00ec), directory `source/`.
- Licence: BSD-3-Clause, see `LICENSE.md`.

Only the files the GLSL back end needs are here; the HLSL, DXBC, DXIL and
SPIR-V back ends and the ReShade runtime are not. DreamStation runs the
compiled effects itself (`frontend-gtk/src/ui/reshade_effect.cpp`).
