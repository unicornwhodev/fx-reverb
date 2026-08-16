# Musique Reverb

Free Windows x64 reverb effect from the Musique FX collection.

## Download
Release assets are intended to include a Windows x64 installer and a portable package with Standalone + VST3 + factory presets.

## Build
```powershell
.\_build_all.ps1 -Configuration Release -BootstrapJuce
```
Or use `-JuceDir C:\Dev\JUCE` with JUCE 8.0.4.

## Package
```powershell
.\_package_release.ps1 -Configuration Release -BootstrapJuce
```

The repository contains product source, factory presets, the small local `FXShared` dependency and release tooling. Internal DSP tests and QA artefacts are excluded.

The plugin is free to use; source is **source-available**, not open source. See [LICENSE.md](LICENSE.md).
