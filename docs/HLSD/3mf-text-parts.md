# Text parts without a mesh in .3mf — High Level Design

## Purpose

A text part may be stored in a .3mf without its mesh. For example, a script
that changes the text will drop the now-stale mesh. A GUI project load rebuilds
such a part from its text configuration and font.

## Importer

- The importer keeps a mesh-less part only when it carries both a text
  configuration and an emboss shape, and only for `LoadStrategy::KeepEmptyText`.
  Only `Plater::priv::load_files` sets it, because it is the only caller that
  rebuilds the parts (see Limitations).
- A mesh-less part that is not text is dropped by every loader, with a log line
  only; there is no rebuild path for it.

## Rebuild

- `Slic3r::GUI::Emboss::rebuild_missing_text_meshes` rebuilds the meshes before the
  model is validated: per-glyph and use-surface text is projected onto the
  object's other parts, and a font from another OS or one that is not installed
  falls back to a similar font, following the emboss gizmo's wxFont fallback. A
  font stored as a file path is loaded from that file directly. When the font
  can't be loaded or the text has no shape, the gizmo's placeholder mesh is used.
  The user is warned about placeholders and substituted fonts.
- The rebuild runs synchronously on the UI thread, before validation and plate
  placement, while the project loads. Many texts, or per-glyph and use-surface
  cutting, block the UI without progress; this is acceptable for the scripting
  use case it serves. A font not installed also re-enumerates all system fonts
  per text, since checking whether a face name is settable invalidates the
  enumerator's cache.

## Ordering

- Flat texts are rebuilt before per-glyph and use-surface ones, because those need
  the other parts' meshes. Two use-surface texts that lie on each other depend on
  volume order: the one rebuilt first finds no surface and is created flat, and its
  `use_surface` setting is kept until it is re-embossed. The user is warned about
  these texts. The same happens when the object's other parts are only negative
  volumes or modifiers. A per-glyph text that finds no surface keeps its glyphs on
  straight lines and is not reported. A text that is the object's only part is flat
  by design and not reported.

## Transform contract

For a part without a mesh the component transform is the text frame, and the
shape's `fix_3mf_tr` is ignored. `fix_3mf_tr` only undoes the centering applied
when a mesh is loaded, which an empty volume does not get.

### Scripting

A script stripping the mesh of a text part saved by Orca must write the component
transform `comp * T(c) * fix^-1` and drop the shape's `transform` attribute. Here
`comp` is the saved component transform, `c` the center of the stripped mesh's
vertex bbox and `fix` the shape's `transform`. A script writing a text part from
scratch writes the text frame directly.

## Limitations

- The CLI, Reload from disk and other loaders drop mesh-less text parts with
  only a log line, so slicing a script-edited file with `--slice` loses the
  text, logged but otherwise silent. Rebuilding needs fonts through wxWidgets,
  which the CLI build does not link.
- A substituted or placeholder font is not written back to the file on disk, but
  the rebuilt mesh is: re-saving the project from the GUI makes the substitution
  or placeholder permanent, since the next load finds a mesh, runs no rebuild,
  and shows no warning — until the text is re-embossed.
